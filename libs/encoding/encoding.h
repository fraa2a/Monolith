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
    int32_t  stream_index;  // 0 = video, 1 = audio
    bool     is_keyframe;
    int32_t  tb_num;        // timebase numerator
    int32_t  tb_den;        // timebase denominator
};

using PacketSink = std::function<void(EncodedPacket)>;

// Codec metadata needed by the replay buffer to write MKV stream headers.
struct VideoStreamParams {
    int width, height;
    int fps_num, fps_den;
    int tb_num, tb_den;
    std::vector<uint8_t> extradata; // SPS/PPS (H.264 global header)
};

struct AudioStreamParams {
    int sample_rate;
    int channels;
    int tb_num, tb_den;
    std::vector<uint8_t> extradata; // AAC AudioSpecificConfig
};

// Probes NVENC → AMF → QSV → libx264.  Returns the first encoder that opens
// successfully with the given frame dimensions, or "" if none available.
std::string probe_video_encoder(int width, int height);

// H.264 encoder.  Not thread-safe — drive from a single thread (or serialise).
class VideoEncoder {
public:
     VideoEncoder();
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&)            = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    struct Config {
        int     width;
        int     height;
        int     fps     = 60;
        int64_t bitrate = 20'000'000;
    };

    // Open codec.  sink receives encoded packets (called synchronously).
    bool open(Config const& cfg, PacketSink sink);

    // Encode one BGRA frame.  bgra must be valid for the duration of the call.
    // stride = bytes per row (>= width*4 after GPU-alignment).
    // Frames with dimensions != the configured width/height are silently skipped.
    void push_bgra(const uint8_t* bgra, int stride, int width, int height);

    void flush();
    void close();
    bool is_open() const;

    VideoStreamParams stream_params() const;

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

} // namespace encoding
