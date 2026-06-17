#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace encoding {

// Unit stored in the replay ring buffer and written to MKV.
struct EncodedPacket {
    std::vector<uint8_t> data;
    int64_t  pts;           // in packet timebase
    int64_t  dts;
    int64_t  dts_usec;     // dts converted to microseconds (for replay buffer ordering/purge)
    int32_t  stream_index;  // 0 = video, 1..6 = audio tracks
    bool     is_keyframe;
    int32_t  tb_num;        // timebase numerator
    int32_t  tb_den;        // timebase denominator
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
        int64_t bitrate = 0;          // legacy: CBR bitrate (0 = use quality)
        int     quality = 20;         // 0=bitrate-mode, 10-30→CQP(HW)/CRF(SW)
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
    // Thread-safe.
    int add_source();

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
