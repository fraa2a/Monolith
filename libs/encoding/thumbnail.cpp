#include "encoding.h"

#include <platform-win/platform_win.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace encoding {
namespace {

using platform_win::wide_to_utf8;

// RAII wrappers so early returns can't leak libav objects.
struct FmtCtx {
    AVFormatContext* p = nullptr;
    ~FmtCtx() { if (p) avformat_close_input(&p); }
};
struct CodecCtx {
    AVCodecContext* p = nullptr;
    ~CodecCtx() { if (p) avcodec_free_context(&p); }
};
struct Frame {
    AVFrame* p = nullptr;
    ~Frame() { if (p) av_frame_free(&p); }
};
struct Packet {
    AVPacket* p = nullptr;
    ~Packet() { if (p) av_packet_free(&p); }
};
struct SwsCtx {
    SwsContext* p = nullptr;
    ~SwsCtx() { if (p) sws_freeContext(p); }
};

// Encodes an RGB24 frame to PNG bytes using libavcodec's PNG encoder.
bool encode_png(const AVFrame* rgb, std::vector<uint8_t>& out)
{
    const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!enc) return false;

    CodecCtx ctx;
    ctx.p = avcodec_alloc_context3(enc);
    if (!ctx.p) return false;
    ctx.p->pix_fmt   = AV_PIX_FMT_RGB24;
    ctx.p->width     = rgb->width;
    ctx.p->height    = rgb->height;
    ctx.p->time_base = {1, 1};

    if (avcodec_open2(ctx.p, enc, nullptr) < 0) return false;
    if (avcodec_send_frame(ctx.p, rgb) < 0) return false;
    if (avcodec_send_frame(ctx.p, nullptr) < 0) return false; // flush

    Packet pkt;
    pkt.p = av_packet_alloc();
    if (!pkt.p) return false;

    int ret = avcodec_receive_packet(ctx.p, pkt.p);
    if (ret < 0) return false;

    out.assign(pkt.p->data, pkt.p->data + pkt.p->size);
    av_packet_unref(pkt.p);
    return !out.empty();
}

} // namespace

bool generate_thumbnail(const std::wstring& video_path,
                        const std::wstring& thumb_path,
                        int max_dim)
{
    if (video_path.empty() || thumb_path.empty()) return false;
    if (max_dim < 16) max_dim = 16;

    const std::string url = wide_to_utf8(video_path);

    FmtCtx fmt;
    if (avformat_open_input(&fmt.p, url.c_str(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(fmt.p, nullptr) < 0)
        return false;

    int vidx = av_find_best_stream(fmt.p, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vidx < 0) return false;

    AVStream* stream = fmt.p->streams[vidx];
    const AVCodec* dec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!dec) return false;

    CodecCtx dctx;
    dctx.p = avcodec_alloc_context3(dec);
    if (!dctx.p) return false;
    if (avcodec_parameters_to_context(dctx.p, stream->codecpar) < 0) return false;
    if (avcodec_open2(dctx.p, dec, nullptr) < 0) return false;

    Frame decoded;
    decoded.p = av_frame_alloc();
    Packet pkt;
    pkt.p = av_packet_alloc();
    if (!decoded.p || !pkt.p) return false;

    // Read packets from the video stream until the decoder yields one frame.
    bool got = false;
    while (!got && av_read_frame(fmt.p, pkt.p) >= 0) {
        if (pkt.p->stream_index == vidx) {
            if (avcodec_send_packet(dctx.p, pkt.p) >= 0) {
                if (avcodec_receive_frame(dctx.p, decoded.p) >= 0)
                    got = true;
            }
        }
        av_packet_unref(pkt.p);
    }
    if (!got) {
        // Flush: some codecs need a null packet to emit the first frame.
        avcodec_send_packet(dctx.p, nullptr);
        got = avcodec_receive_frame(dctx.p, decoded.p) >= 0;
    }
    if (!got || decoded.p->width <= 0 || decoded.p->height <= 0) return false;

    // Scale so the longest side is at most max_dim (aspect preserved, even dims).
    const int sw = decoded.p->width, sh = decoded.p->height;
    double scale = std::min(1.0, static_cast<double>(max_dim) / std::max(sw, sh));
    int dw = std::max(2, (static_cast<int>(sw * scale) / 2) * 2);
    int dh = std::max(2, (static_cast<int>(sh * scale) / 2) * 2);

    SwsCtx sws;
    sws.p = sws_getContext(sw, sh, static_cast<AVPixelFormat>(decoded.p->format),
                           dw, dh, AV_PIX_FMT_RGB24,
                           SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws.p) return false;

    Frame rgb;
    rgb.p = av_frame_alloc();
    if (!rgb.p) return false;
    rgb.p->format = AV_PIX_FMT_RGB24;
    rgb.p->width  = dw;
    rgb.p->height = dh;
    if (av_frame_get_buffer(rgb.p, 32) < 0) return false;

    sws_scale(sws.p, decoded.p->data, decoded.p->linesize, 0, sh,
              rgb.p->data, rgb.p->linesize);

    std::vector<uint8_t> png;
    if (!encode_png(rgb.p, png)) return false;

    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(thumb_path).parent_path(), ec);

    std::ofstream f(std::filesystem::path(thumb_path),
                    std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(png.data()),
            static_cast<std::streamsize>(png.size()));
    return static_cast<bool>(f);
}

double probe_duration_seconds(const std::wstring& video_path)
{
    if (video_path.empty()) return 0.0;

    FmtCtx fmt;
    const std::string url = wide_to_utf8(video_path);
    if (avformat_open_input(&fmt.p, url.c_str(), nullptr, nullptr) < 0)
        return 0.0;
    if (avformat_find_stream_info(fmt.p, nullptr) < 0)
        return 0.0;

    if (fmt.p->duration > 0 && fmt.p->duration != AV_NOPTS_VALUE)
        return static_cast<double>(fmt.p->duration) / AV_TIME_BASE;

    int vidx = av_find_best_stream(fmt.p, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vidx >= 0) {
        AVStream* stream = fmt.p->streams[vidx];
        if (stream->duration > 0 && stream->duration != AV_NOPTS_VALUE) {
            return static_cast<double>(stream->duration)
                * static_cast<double>(stream->time_base.num)
                / static_cast<double>(stream->time_base.den);
        }
    }

    return 0.0;
}

} // namespace encoding
