#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct AVFrame;

namespace encoding {

// Blocking decode/PNG I/O: call off the UI thread. max_dim bounds the longest side.
bool generate_thumbnail(const std::wstring& video_path,
                        const std::wstring& thumb_path,
                        int max_dim = 480);

// Reads media duration from the container/stream metadata in seconds. Returns
// 0 when the file cannot be probed or has no usable duration.
double probe_duration_seconds(const std::wstring& video_path);

// Immutable encoded payload shared by replay snapshots, mux queues and
// recording sinks. Copying EncodedPacket must not duplicate packet bytes.
struct EncodedBytes {
    std::unique_ptr<uint8_t[]> data;
    size_t size = 0;

    EncodedBytes() = default;
    EncodedBytes(const EncodedBytes&) = delete;
    EncodedBytes& operator=(const EncodedBytes&) = delete;
};

using EncodedBytesRef = std::shared_ptr<const EncodedBytes>;

// Unit stored in the replay ring buffer and written to MKV.
struct EncodedPacket {
    EncodedBytesRef bytes;
    int64_t  pts = 0;           // in packet timebase
    int64_t  dts = 0;
    int64_t  dts_usec = 0;      // dts converted to microseconds (for replay buffer ordering/purge)
    int32_t  stream_index = 0;  // 0 = video, 1..6 = audio tracks
    bool     is_keyframe = false;
    int32_t  tb_num = 1;        // timebase numerator
    int32_t  tb_den = 1;        // timebase denominator

    const uint8_t* data() const noexcept { return bytes ? bytes->data.get() : nullptr; }
    size_t size() const noexcept { return bytes ? bytes->size : 0; }
    bool empty() const noexcept { return size() == 0; }
};

using PacketSink = std::function<void(EncodedPacket)>;

enum class VideoCodec {
    H264,
    H265,
    AV1,
};

// Codec metadata needed by muxers to write stream headers.
struct VideoStreamParams {
    VideoCodec codec = VideoCodec::H264;
    int width = 0, height = 0;
    int fps_num = 0, fps_den = 0;
    int tb_num = 0, tb_den = 0;
    std::vector<uint8_t> extradata; // global headers (SPS/PPS/VPS)
};

struct AudioStreamParams {
    int stream_index = 1; // 1..6; 0 is reserved for video
    int sample_rate = 0;
    int channels = 0;
    int tb_num = 0, tb_den = 0;
    std::vector<uint8_t> extradata; // AAC AudioSpecificConfig
};

// Probes NVENC → AMF → QSV → libx264.  Returns the first encoder that opens
// successfully with the given frame dimensions, or "" if none available.
std::string probe_video_encoder(int width, int height);

// Probes every candidate encoder and returns all that open successfully,
// in probe order.  Used to populate the Settings UI encoder list.
std::vector<std::string> available_video_encoders(int width, int height);

// Try the requested device, then the other device within the same codec family.
// Return an empty name if no encoder opens.
std::string resolve_video_encoder(const std::string& device,
                                   const std::string& codec,
                                   int width, int height);

struct VideoEncoderPerfStats {
    uint64_t frames_submitted = 0;
    uint64_t packets_output = 0;
    uint64_t sws_scale_time_us_total = 0;
    uint64_t encode_time_us_total = 0;
};

// Serialize calls to the video encoder.
class VideoEncoder {
public:
     VideoEncoder();
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&)            = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    struct Config {
        int     width = 0;           // output (encoded) width
        int     height = 0;          // output (encoded) height
        VideoCodec codec = VideoCodec::H264;
        int     fps     = 60;
        // bitrate is in bits per second; positive quality selects the legacy quality mode.
        int64_t bitrate = 20'000'000; // CBR target, bits/s
        int     quality = 0;          // 0 = CBR (default); >0 = legacy CQP/CRF
        std::string scaling_filter = "bilinear";
        // "" or "auto" → probe NVENC → AMF → QSV → libx264.
        // Otherwise this encoder is tried first, then the probe order.
        std::string preferred_encoder;
        // Extra AVOptions use key=value pairs separated by colon, comma or space.
        // Rejected options trigger a retry without extras and set extra_options_rejected.
        std::string extra_options;
        // Live capture keeps zerolatency tuning (no B-frames, fast presets).
        // Offline re-encode (quick trim) clears this for better quality per
        // bit and full frame-threading.
        bool low_latency = true;
    };

    // Open codec.  sink receives encoded packets (called synchronously).
    bool open(Config const& cfg, PacketSink sink);

    // BGRA storage must remain valid during the call; stride includes row padding.
    // PTS is a frame index in 1/fps; -1 selects internal incrementing timestamps.
    void push_bgra(const uint8_t* bgra, int stride, int width, int height,
                   int64_t pts = -1);

    // Matching format/size shares the decoded planes; other input is converted.
    // PTS follows push_bgra semantics.
    void push_frame(const AVFrame* frame, int64_t pts = -1);

    void flush();
    void close();
    bool is_open() const;

    VideoStreamParams stream_params() const;

    // Name of the encoder actually opened ("" while closed).
    std::string encoder_name() const;

    // True when Config::extra_options had to be dropped to open the encoder.
    bool extra_options_rejected() const;

    VideoEncoderPerfStats perf_stats() const;

private:
    struct Impl;
    Impl* impl_;
};

// AAC encoder.  Not thread-safe.
class AudioEncoder {
public:
     AudioEncoder();
    ~AudioEncoder();
    AudioEncoder(const AudioEncoder&)            = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;

    struct Config {
        int     sample_rate = 48000;
        int     channels    = 2;
        int64_t bitrate     = 192'000;
        int     stream_index = 1;
    };

    bool open(Config const& cfg, PacketSink sink);

    // Push raw PCM.  data is valid only for the duration of the call.
    // is_float: true for IEEE float (32-bit), false for signed PCM.
    void push_pcm(const uint8_t* data, int bytes,
                  int sample_rate, int channels,
                  int bit_depth, bool is_float);

    void flush();
    void close();
    bool is_open() const;

    AudioStreamParams stream_params() const;

private:
    struct Impl;
    Impl* impl_;
};

// Resample sources to one track and emit wall-clock-paced float PCM.
// Use direct encoder input when a track has only one source.
class TrackMixer {
public:
    // Sink PCM is interleaved float in the configured output format.
    using Sink = std::function<void(const uint8_t* data, int bytes,
                                    int sample_rate, int channels,
                                    int bit_depth, bool is_float)>;

     TrackMixer();
    ~TrackMixer();
    TrackMixer(const TrackMixer&)            = delete;
    TrackMixer& operator=(const TrackMixer&) = delete;

    // Starts the mix thread. out_channels is the canonical channel count.
    bool open(int out_sample_rate, int out_channels, Sink sink);

    // Gain is linear; negative/nonfinite input is muted. Return -1 when registration fails.
    int add_source(float gain = 1.0f);

    void set_source_gain(int source_id, float gain);

    void remove_source(int source_id);

    // Number of currently registered sources.
    int source_count() const;

    // Copy/convert PCM during the call; the caller retains input ownership.
    void push(int source_id, const uint8_t* data, int bytes,
              int sample_rate, int channels, int bit_depth, bool is_float);

    void close();
    bool is_open() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace encoding
