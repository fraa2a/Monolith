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

// Allocates an output AVFormatContext for `path_utf` using the container's muxer
// and creates the video stream plus one stream per valid audio param.  On success
// *out_fmt holds the context and *out_streams the stream mapping; returns false
// (and frees any partial context) on failure.
bool alloc_output(const std::string&                          path_utf,
                  const std::string&                          container,
                  const VideoStreamParams&                    vsp,
                  const std::vector<AudioStreamParams>&       audio_params,
                  AVFormatContext**                           out_fmt,
                  StreamSet*                                  out_streams);

// Opens the file and writes the container header. mp4 keeps the moov atom at the
// end (no +faststart) so finalizing never rewrites the whole file on stop.
// Returns false without closing on failure (caller frees the context).
bool open_file_and_write_header(AVFormatContext* fmt,
                                const std::string& path_utf,
                                const std::string& container);

// Builds an AVPacket from `ep`, applies the per-stream pts/dts offset (subtracted
// before rescale), rescales from the packet timebase to the destination stream
// timebase, and interleaved-writes it.  Returns false if allocation fails.
bool write_packet(AVFormatContext* fmt,
                  AVStream*        dst_stream,
                  const EncodedPacket& ep,
                  int64_t          pts_offset,
                  int64_t          dts_offset);

// Captures the pts/dts of the first packet observed on a stream, so callers
// can offset every subsequent packet back to zero. Shared by both mux modes:
// replay-buffer's one-shot snapshot (anchor only, never reset) and manual
// recording's continuous-with-pause stream (anchor plus caller-tracked pause
// accumulation added on top of anchor.pts/anchor.dts).
struct TimingAnchor {
    bool    set = false;
    int64_t pts = 0;
    int64_t dts = 0;

    // No-op after the first call for this instance.
    void observe(int64_t p, int64_t d)
    {
        if (set) return;
        set = true;
        pts = p;
        dts = d;
    }
};

} // namespace encoding::mux
