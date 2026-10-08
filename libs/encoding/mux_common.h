#pragma once

#include <encoding/encoding.h>

#include <array>
#include <string>
#include <vector>

struct AVFormatContext;
struct AVStream;

namespace encoding::mux {

// UTF-16 → UTF-8 (Windows wide path → FFmpeg path argument).
std::string wcs_to_utf8(const std::wstring& ws);

// libavformat short muxer name for a container ("mp4" | "mkv"/anything else).
const char* muxer_name(const std::string& container);

// File extension (no dot) for a container.
const wchar_t* file_extension(const std::string& container);

// Maps the project codec enum to the libavcodec id.
int video_codec_id(VideoCodec codec);

// Stream index 0 = video, 1..6 = audio.  audio[i] is the AVStream for track i
// (audio[0] unused).  video is the video AVStream.
struct StreamSet {
    AVStream*                video = nullptr;
    std::array<AVStream*, 7> audio{};
};

// On success the caller owns out_fmt; failure releases the partial context.
bool alloc_output(const std::string&                          path_utf,
                  const std::string&                          container,
                  const VideoStreamParams&                    vsp,
                  const std::vector<AudioStreamParams>&       audio_params,
                  AVFormatContext**                           out_fmt,
                  StreamSet*                                  out_streams);

// Close AVIO on header failure; the caller must still free the format context.
bool open_file_and_write_header(AVFormatContext* fmt,
                                const std::string& path_utf,
                                const std::string& container);

// Use local time including milliseconds for the clip filename.
std::wstring generate_clip_path(const std::wstring& dir,
                                int duration_sec,
                                const std::string& container);

// Builds an AVPacket from `ep`, applies the per-stream pts/dts offset (subtracted
// before rescale), rescales from the packet timebase to the destination stream
// timebase, and interleaved-writes it.  Returns false if allocation fails.
bool write_packet(AVFormatContext* fmt,
                  AVStream*        dst_stream,
                  const EncodedPacket& ep,
                  int64_t          pts_offset,
                  int64_t          dts_offset);

// Observe a timestamp pair once and retain it as the mux offset origin.
struct TimingAnchor {
    bool    set = false;
    int64_t pts = 0;
    int64_t dts = 0;

    void observe(int64_t p, int64_t d)
    {
        if (set) return;
        set = true;
        pts = p;
        dts = d;
    }
};

} // namespace encoding::mux
