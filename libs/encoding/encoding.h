#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace encoding {

// Decodes the first frame of an existing video file and writes it as a PNG
// thumbnail. The frame is scaled so its longest side is at most max_dim
// (aspect ratio preserved). Returns true on success. Thread-safe: opens its
// own demuxer/decoder/scaler and touches no shared state, so it is safe to
// call from the async clip-save thread or a background reconcile thread.
// Never call it from the UI/tray message loop (it does blocking decode I/O).
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
};

// Codec metadata needed by muxers to write stream headers.
struct VideoStreamParams {
    VideoCodec codec = VideoCodec::H264;
    int width, height;
    int fps_num, fps_den;
    int tb_num, tb_den;
    std::vector<uint8_t> extradata; // global headers (SPS/PPS/VPS)
};

struct AudioStreamParams {
    int stream_index = 1; // 1..6; 0 is reserved for video
    int sample_rate;
    int channels;
    int tb_num, tb_den;
    std::vector<uint8_t> extradata; // AAC AudioSpecificConfig
};

// Probes NVENC → AMF → QSV → libx264.  Returns the first encoder that opens
// successfully with the given frame dimensions, or "" if none available.
std::string probe_video_encoder(int width, int height);

// Probes every candidate encoder and returns all that open successfully,
// in probe order.  Used to populate the Settings UI encoder list.
std::vector<std::string> available_video_encoders(int width, int height);

// Resolves the user-facing choice (device: "gpu"/"cpu", codec: "h264"/"h265")
// to a concrete FFmpeg encoder name the current machine can actually open at
// the given dimensions. GPU tries the vendor HW encoders (NVENC → AMF → QSV)
// for the codec; CPU uses libx264/libx265. If the preferred device yields no
// working encoder, falls back to the other device for the same codec. Returns
// "" if nothing opens.
std::string resolve_video_encoder(const std::string& device,
                                   const std::string& codec,
                                   int width, int height);

struct VideoEncoderPerfStats {
    uint64_t frames_submitted = 0;
    uint64_t packets_output = 0;
    uint64_t sws_scale_time_us_total = 0;
    uint64_t encode_time_us_total = 0;
};

// H.264 encoder.  Not thread-safe — drive from a single thread (or serialise).
class VideoEncoder {
public:
     VideoEncoder();
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&)            = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    struct Config {
        int     width;               // output (encoded) width
        int     height;              // output (encoded) height
        int     fps     = 60;
        // Rate control is always CBR at this target bitrate (bits per second).
        // quality is retained only for probe/back-compat and is unused by the
        // recorder path.
        int64_t bitrate = 20'000'000; // CBR target, bits/s
        int     quality = 0;          // 0 = CBR (default); >0 = legacy CQP/CRF
        // Scaling filter (OBS-style): "bilinear", "bicubic", "lanczos".
        // Used by sws_scale when capture and output resolutions differ.
        std::string scaling_filter = "bilinear";
        // "" or "auto" → probe NVENC → AMF → QSV → libx264.
        // Otherwise this encoder is tried first, then the probe order.
        std::string preferred_encoder;
        // Extra AVOptions passed to avcodec_open2, "key=value:key=value"
        // (also accepts ',' or ' ' between pairs).  If the encoder rejects
        // them, open() retries without and extra_options_rejected() is set.
        std::string extra_options;
    };

    // Open codec.  sink receives encoded packets (called synchronously).
    bool open(Config const& cfg, PacketSink sink);

    // Encode one BGRA frame.  bgra must be valid for the duration of the call.
    // stride = bytes per row (>= width*4 after GPU-alignment).
    // Frames are scaled to the configured output width/height when the
    // source dimensions differ.
    // pts: frame index in the encoder timebase (1/fps).  Must be supplied by
    // a clock-locked CFR pacer so playback speed matches wall-clock.  Pass -1
    // to fall back to the internal auto-increment counter (legacy behaviour).
    void push_bgra(const uint8_t* bgra, int stride, int width, int height,
                   int64_t pts = -1);

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

// Mixes PCM from multiple capture sources into a single output stream for one
// audio track. Each source is resampled to a canonical format and buffered;
// an internal wall-clock-paced thread sums all sources (with clipping) and
// emits a steady output stream through the sink, so the downstream encoder
// timeline advances at real-time rate regardless of per-source bursts/gaps.
//
// Used only when two or more sources are routed to the same track; a single
// source still feeds its encoder directly without going through the mixer.
class TrackMixer {
public:
    // Sink receives mixed PCM in the canonical format (interleaved float).
    // Signature mirrors AudioEncoder::push_pcm so it can forward directly.
    using Sink = std::function<void(const uint8_t* data, int bytes,
                                    int sample_rate, int channels,
                                    int bit_depth, bool is_float)>;

     TrackMixer();
    ~TrackMixer();
    TrackMixer(const TrackMixer&)            = delete;
    TrackMixer& operator=(const TrackMixer&) = delete;

    // Starts the mix thread. out_channels is the canonical channel count.
    bool open(int out_sample_rate, int out_channels, Sink sink);

    // Registers a source and returns its id (>= 0), or -1 if not open.
    // Thread-safe. Optional linear gain (0.0–1.0+) applied to this source's
    // samples before they are summed into the mix.
    int add_source(float gain = 1.0f);

    // Updates a source's linear gain. Thread-safe; no-op for unknown ids.
    void set_source_gain(int source_id, float gain);

    // Removes a previously added source. Thread-safe.
    void remove_source(int source_id);

    // Number of currently registered sources.
    int source_count() const;

    // Feeds PCM for one source. Converts to the canonical format and buffers it.
    // Safe to call from that source's capture thread; data need not outlive call.
    void push(int source_id, const uint8_t* data, int bytes,
              int sample_rate, int channels, int bit_depth, bool is_float);

    void close();
    bool is_open() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace encoding
