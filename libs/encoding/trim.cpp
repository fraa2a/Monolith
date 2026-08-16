#include "trim.h"

#include "mux_common.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
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
#include <cstring>
#include <limits>
#include <vector>

namespace encoding {
namespace {

using mux::wcs_to_utf8;

// RAII wrappers so early returns can't leak libav objects.
struct FmtCtx {
    AVFormatContext* p = nullptr;
    ~FmtCtx() { if (p) avformat_close_input(&p); }
};
struct OutFmt {
    AVFormatContext* p = nullptr;
    ~OutFmt() {
        if (p) {
            // Error paths can leave pb open (avio_open succeeded but the
            // header/trailer never wrote) — close it so the file handle and
            // AVIOContext aren't leaked. avio_closep is a no-op on a closed pb.
            avio_closep(&p->pb);
            avformat_free_context(p);
        }
    }
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
struct SwrCtx {
    SwrContext* p = nullptr;
    ~SwrCtx() { if (p) swr_free(&p); }
};

const char* muxer_for_path(const std::wstring& out_path)
{
    const std::string utf = wcs_to_utf8(out_path);
    const char* ext = strrchr(utf.c_str(), '.');
    if (ext && _stricmp(ext, ".mp4") == 0) return "mp4";
    return "matroska";
}

// Opens `path` and returns the stream indices to copy: video first, then every
// audio stream in order of appearance.
bool open_input(const std::wstring& path, FmtCtx* fmt,
                std::vector<int>* stream_order, std::string* err)
{
    const std::string url = wcs_to_utf8(path);
    if (avformat_open_input(&fmt->p, url.c_str(), nullptr, nullptr) < 0) {
        if (err) *err = "could not open input file";
        return false;
    }
    if (avformat_find_stream_info(fmt->p, nullptr) < 0) {
        if (err) *err = "could not read input stream info";
        return false;
    }
    if (!stream_order) return true;
    int vidx = av_find_best_stream(fmt->p, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vidx < 0) {
        if (err) *err = "input has no video stream";
        return false;
    }
    stream_order->push_back(vidx);
    for (unsigned i = 0; i < fmt->p->nb_streams; ++i) {
        if (static_cast<int>(i) != vidx &&
            fmt->p->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            stream_order->push_back(static_cast<int>(i));
    }
    return true;
}

AVRational tb_of(AVStream* s)
{
    return (s->time_base.num > 0 && s->time_base.den > 0)
        ? s->time_base : AVRational{1, 1000};
}

double pts_seconds(int64_t pts, AVRational tb)
{
    return static_cast<double>(pts) * static_cast<double>(tb.num)
        / static_cast<double>(tb.den);
}

// Builds the output muxer mirroring `in`'s layout: one stream per entry of
// `stream_order` (video first, then audio streams), codecpar copied.
bool alloc_copy_output(const std::wstring& out_path,
                       AVFormatContext* in,
                       const std::vector<int>& stream_order,
                       OutFmt* out,
                       std::vector<AVStream*>* dst_streams,
                       std::string* err)
{
    const std::string url = wcs_to_utf8(out_path);
    const AVOutputFormat* guess =
        av_guess_format(muxer_for_path(out_path), url.c_str(), nullptr);
    if (!guess) {
        if (err) *err = "no muxer for output format";
        return false;
    }
    if (avformat_alloc_output_context2(&out->p, guess, nullptr, url.c_str()) < 0) {
        if (err) *err = "could not allocate output muxer";
        return false;
    }
    for (int idx : stream_order) {
        AVStream* src = in->streams[idx];
        AVStream* dst = avformat_new_stream(out->p, nullptr);
        if (!dst) {
            if (err) *err = "could not allocate output stream";
            return false;
        }
        if (avcodec_parameters_copy(dst->codecpar, src->codecpar) < 0) {
            if (err) *err = "could not copy stream parameters";
            return false;
        }
        dst->time_base = tb_of(src);
        dst_streams->push_back(dst);
    }
    return true;
}

// Opens the output file and writes the container header. Called once per
// output, before any remux_window pass.
bool open_output(AVFormatContext* out, const std::string& out_utf, std::string* err)
{
    if (avio_open(&out->pb, out_utf.c_str(), AVIO_FLAG_WRITE) < 0) {
        if (err) *err = "could not open output file";
        return false;
    }
    if (avformat_write_header(out, nullptr) < 0) {
        if (err) *err = "could not write output header";
        return false;
    }
    return true;
}

// Copies the packets of every stream whose media time falls inside
// [start_sec, end_sec] of `in`'s timeline into the already-opened output
// `out` (mapping dst[i] ← in stream `stream_order[i]`). The video stream
// drives the end cut; the start is keyframe-aligned via a backward seek.
// pts/dts are re-anchored to zero per stream, then shifted by `offset_sec`
// (used when concatenating several segments into one timeline). Writes
// packets only — the caller opens the file and finalizes.
bool remux_window(AVFormatContext* in,
                  const std::vector<int>& stream_order,
                  double start_sec, double end_sec, double offset_sec,
                  AVFormatContext* out,
                  const std::vector<AVStream*>& dst_streams,
                  std::string* err)
{
    const int vidx = stream_order.front();
    const AVRational vtb = tb_of(in->streams[vidx]);
    const int64_t seek_ts = static_cast<int64_t>(start_sec * vtb.den / vtb.num);
    if (av_seek_frame(in, vidx, seek_ts, AVSEEK_FLAG_BACKWARD) < 0) {
        if (err) *err = "could not seek to trim start";
        return false;
    }

    // Anchor per output stream: pts of the first kept packet. Video anchors
    // at the seeked keyframe (<= start); audio anchors at the first packet
    // at/after the VIDEO anchor so both timelines start together — anchoring
    // audio at `start` instead would lead the audio by up to a GOP. dts gets
    // the same offset so B-frame deltas stay intact.
    std::vector<int64_t> anchor(dst_streams.size(), std::numeric_limits<int64_t>::min());
    double vanchor_sec = -1.0;

    Packet pkt;
    pkt.p = av_packet_alloc();
    if (!pkt.p) { if (err) *err = "out of memory"; return false; }

    bool done = false;
    while (!done && av_read_frame(in, pkt.p) >= 0) {
        if (pkt.p->pts == AV_NOPTS_VALUE) { av_packet_unref(pkt.p); continue; }

        int dst_idx = -1;
        for (size_t i = 0; i < stream_order.size(); ++i) {
            if (stream_order[i] == pkt.p->stream_index) { dst_idx = static_cast<int>(i); break; }
        }
        if (dst_idx < 0) { av_packet_unref(pkt.p); continue; }

        const AVRational tb = tb_of(in->streams[pkt.p->stream_index]);
        const double sec = pts_seconds(pkt.p->pts, tb);
        if (pkt.p->stream_index == vidx && sec >= end_sec) {
            done = true;
            av_packet_unref(pkt.p);
            break;
        }

        if (anchor[dst_idx] == std::numeric_limits<int64_t>::min()) {
            if (pkt.p->stream_index == vidx) {
                anchor[dst_idx] = pkt.p->pts;
                vanchor_sec = sec;
            } else if (vanchor_sec >= 0.0 && sec >= vanchor_sec) {
                anchor[dst_idx] = pkt.p->pts;
            }
        }

        AVStream* dst = dst_streams[dst_idx];
        const int64_t acc = static_cast<int64_t>(
            offset_sec * static_cast<double>(dst->time_base.den)
            / static_cast<double>(dst->time_base.num));
        if (anchor[dst_idx] != std::numeric_limits<int64_t>::min()) {
            pkt.p->stream_index = dst->index;
            if (pkt.p->dts == AV_NOPTS_VALUE) pkt.p->dts = pkt.p->pts;
            pkt.p->pts = pkt.p->pts - anchor[dst_idx] + acc;
            pkt.p->dts = pkt.p->dts - anchor[dst_idx] + acc;
            if (pkt.p->dts > pkt.p->pts) pkt.p->dts = pkt.p->pts;
            if (av_interleaved_write_frame(out, pkt.p) < 0) {
                av_packet_unref(pkt.p);
                if (err) *err = "failed to write output packet";
                return false;
            }
        }
        av_packet_unref(pkt.p);
    }
    return true;
}

// Finalizes an output muxer (trailer + close). The context itself is owned by
// the caller's RAII (OutFmt) or freed explicitly (raw pointers).
bool finalize_output(AVFormatContext* out, std::string* err)
{
    if (av_write_trailer(out) < 0) {
        if (err) *err = "failed to finalize output";
        return false;
    }
    avio_closep(&out->pb);
    return true;
}

// ── Re-encode fallback ───────────────────────────────────────────────────────

} // namespace

bool trim_clip_lossless(const std::wstring& path,
                        double start, double end,
                        const std::wstring& out_path,
                        std::string* err)
{
    if (start < 0.0 || end <= start) {
        if (err) *err = "end must be after start";
        return false;
    }
    FmtCtx in;
    std::vector<int> order;
    if (!open_input(path, &in, &order, err)) return false;
    // Duration from the container we just opened — no second probe pass.
    const double duration = (in.p->duration > 0)
        ? static_cast<double>(in.p->duration) / AV_TIME_BASE : 0.0;
    if (duration > 0.0 && start >= duration) {
        if (err) *err = "start is past the end of the clip";
        return false;
    }
    const double eff_end = (duration > 0.0 && end > duration) ? duration : end;

    OutFmt out;
    std::vector<AVStream*> dst;
    if (!alloc_copy_output(out_path, in.p, order, &out, &dst, err)) return false;

    const std::string out_utf = wcs_to_utf8(out_path);
    if (!open_output(out.p, out_utf, err)) return false;
    if (!remux_window(in.p, order, start, eff_end, 0.0, out.p, dst, err))
        return false;
    return finalize_output(out.p, err);
}

bool trim_clip_reencode(const std::wstring& path,
                        double start, double end,
                        const std::wstring& out_path,
                        std::string* err)
{
    if (start < 0.0 || end <= start) {
        if (err) *err = "end must be after start";
        return false;
    }

    FmtCtx in;
    if (!open_input(path, &in, nullptr, err)) return false;
    // Duration from the container we just opened — no second probe pass.
    const double duration = (in.p->duration > 0)
        ? static_cast<double>(in.p->duration) / AV_TIME_BASE : 0.0;
    if (duration > 0.0 && start >= duration) {
        if (err) *err = "start is past the end of the clip";
        return false;
    }
    const double eff_end = (duration > 0.0 && end > duration) ? duration : end;

    const int vidx = av_find_best_stream(in.p, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int aidx = av_find_best_stream(in.p, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (vidx < 0) { if (err) *err = "input has no video stream"; return false; }

    AVStream* vs = in.p->streams[vidx];
    const AVCodec* vdec = avcodec_find_decoder(vs->codecpar->codec_id);
    if (!vdec) { if (err) *err = "no decoder for input video"; return false; }
    CodecCtx vdec_ctx;
    vdec_ctx.p = avcodec_alloc_context3(vdec);
    if (!vdec_ctx.p || avcodec_parameters_to_context(vdec_ctx.p, vs->codecpar) < 0) {
        if (err) *err = "could not open video decoder";
        return false;
    }
    vdec_ctx.p->thread_count = 0; // offline decode: use all cores
    if (avcodec_open2(vdec_ctx.p, vdec, nullptr) < 0) {
        if (err) *err = "could not open video decoder";
        return false;
    }

    // Encoder config mirrors the source: same dims (even-aligned), fps, bitrate.
    const int out_w = (vs->codecpar->width + 1) & ~1;
    const int out_h = (vs->codecpar->height + 1) & ~1;
    int fps = 60;
    if (vs->avg_frame_rate.num > 0 && vs->avg_frame_rate.den > 0)
        fps = std::max(1, (vs->avg_frame_rate.num + vs->avg_frame_rate.den / 2)
                          / vs->avg_frame_rate.den);
    else if (vs->r_frame_rate.num > 0 && vs->r_frame_rate.den > 0)
        fps = std::max(1, (vs->r_frame_rate.num + vs->r_frame_rate.den / 2)
                          / vs->r_frame_rate.den);
    int64_t bitrate = vs->codecpar->bit_rate;
    if (bitrate <= 0) bitrate = 20'000'000;

    std::vector<EncodedPacket> vpkt, apkt;
    VideoEncoder venc;
    // low_latency=false: offline encode keeps B-frames/frame-threading (better
    // quality per bit than zerolatency) and lets the SW encoders use all cores.
    if (!venc.open(
            VideoEncoder::Config{out_w, out_h, fps, bitrate, 0, "bilinear", "", "",
                                 /*low_latency=*/false},
            [&](EncodedPacket ep) { vpkt.push_back(std::move(ep)); })) {
        if (err) *err = "could not open video encoder";
        return false;
    }

    AudioEncoder aenc;
    CodecCtx adec_ctx;
    bool have_audio = false;
    if (aidx >= 0) {
        AVStream* as = in.p->streams[aidx];
        const AVCodec* adec = avcodec_find_decoder(as->codecpar->codec_id);
        if (adec) {
            adec_ctx.p = avcodec_alloc_context3(adec);
            if (adec_ctx.p &&
                avcodec_parameters_to_context(adec_ctx.p, as->codecpar) == 0) {
                adec_ctx.p->thread_count = 0; // offline decode: all cores
                if (avcodec_open2(adec_ctx.p, adec, nullptr) == 0)
                    have_audio = aenc.open(
                        AudioEncoder::Config{as->codecpar->sample_rate,
                                             as->codecpar->ch_layout.nb_channels,
                                             192'000, 1},
                        [&](EncodedPacket ep) { apkt.push_back(std::move(ep)); });
            }
        }
    }

    // Decode [start, eff_end): seek to the keyframe at/before start; frames
    // whose pts lands inside the window are re-encoded at CFR `fps` (frame
    // counter as pts), so the trimmed span plays at the source speed.
    const AVRational vtb = tb_of(vs);
    const int64_t seek_ts = static_cast<int64_t>(start * vtb.den / vtb.num);
    if (av_seek_frame(in.p, vidx, seek_ts, AVSEEK_FLAG_BACKWARD) < 0) {
        if (err) *err = "could not seek to trim start";
        return false;
    }

    SwrCtx swr;
    Frame pcm;
    pcm.p = av_frame_alloc();
    Frame dec;
    dec.p = av_frame_alloc();
    if (!pcm.p || !dec.p) { if (err) *err = "out of memory"; return false; }

    Packet pkt;
    pkt.p = av_packet_alloc();
    if (!pkt.p) { if (err) *err = "out of memory"; return false; }

    // Convert one decoded audio frame to interleaved S16 and feed the encoder.
    std::vector<uint8_t> audio_buf; // reused across frames
    const auto encode_audio_frame = [&]() -> bool {
        // Convert to interleaved S16 (push_pcm accepts it and resamples
        // internally); decoder output is often planar.
        if (!swr.p) {
            AVChannelLayout src_chl{};
            av_channel_layout_copy(&src_chl, &pcm.p->ch_layout);
            swr_alloc_set_opts2(&swr.p,
                &pcm.p->ch_layout, AV_SAMPLE_FMT_S16, pcm.p->sample_rate,
                &src_chl,
                static_cast<AVSampleFormat>(pcm.p->format),
                pcm.p->sample_rate, 0, nullptr);
            av_channel_layout_uninit(&src_chl);
            if (!swr.p || swr_init(swr.p) < 0) {
                if (err) *err = "could not init audio resampler";
                return false;
            }
        }
        const int out_samples = static_cast<int>(av_rescale_rnd(
            swr_get_delay(swr.p, pcm.p->sample_rate) + pcm.p->nb_samples,
            pcm.p->sample_rate, pcm.p->sample_rate, AV_ROUND_UP));
        audio_buf.resize(static_cast<size_t>(out_samples) *
                         2 * pcm.p->ch_layout.nb_channels);
        uint8_t* out_data = audio_buf.data();
        const int converted = swr_convert(swr.p, &out_data, out_samples,
            const_cast<const uint8_t**>(pcm.p->data), pcm.p->nb_samples);
        if (converted > 0)
            aenc.push_pcm(audio_buf.data(),
                          converted * 2 * pcm.p->ch_layout.nb_channels,
                          pcm.p->sample_rate, pcm.p->ch_layout.nb_channels,
                          16, false);
        return true;
    };

    int64_t frames = 0;
    bool video_done = false;
    while (!video_done && av_read_frame(in.p, pkt.p) >= 0) {
        if (pkt.p->stream_index == vidx) {
            const double sec = pkt.p->pts == AV_NOPTS_VALUE
                ? 0.0 : pts_seconds(pkt.p->pts, vtb);
            if (sec >= eff_end) { video_done = true; break; }
            if (sec < start) { av_packet_unref(pkt.p); continue; }
            if (avcodec_send_packet(vdec_ctx.p, pkt.p) >= 0) {
                while (avcodec_receive_frame(vdec_ctx.p, dec.p) == 0) {
                    // Decoder planes are shared with the encoder when they
                    // already match its format/size — no YUV→BGRA→YUV
                    // round-trip, no extra rounding loss.
                    venc.push_frame(dec.p, frames++);
                    av_frame_unref(dec.p);
                }
            }
        } else if (aidx >= 0 && pkt.p->stream_index == aidx && have_audio) {
            const AVRational atb = tb_of(in.p->streams[aidx]);
            const double sec = pkt.p->pts == AV_NOPTS_VALUE
                ? 0.0 : pts_seconds(pkt.p->pts, atb);
            if (sec >= eff_end) { av_packet_unref(pkt.p); continue; }
            // Drop pre-roll between the seeked keyframe and the trim start so
            // audio and video enter the output window together.
            if (sec < start) { av_packet_unref(pkt.p); continue; }
            if (avcodec_send_packet(adec_ctx.p, pkt.p) >= 0) {
                while (avcodec_receive_frame(adec_ctx.p, pcm.p) == 0) {
                    if (!encode_audio_frame()) return false;
                    av_frame_unref(pcm.p);
                }
            }
        }
        av_packet_unref(pkt.p);
    }

    // Drain the decoders' reorder queues so trailing frames (inputs with
    // B-frames keep frames buffered at EOF) aren't truncated.
    avcodec_send_packet(vdec_ctx.p, nullptr);
    while (avcodec_receive_frame(vdec_ctx.p, dec.p) == 0) {
        const double fsec = dec.p->pts == AV_NOPTS_VALUE
            ? 0.0 : pts_seconds(dec.p->pts, vtb);
        if (fsec < eff_end)
            venc.push_frame(dec.p, frames++);
        av_frame_unref(dec.p);
    }
    if (have_audio) {
        const AVRational atb = tb_of(in.p->streams[aidx]);
        avcodec_send_packet(adec_ctx.p, nullptr);
        while (avcodec_receive_frame(adec_ctx.p, pcm.p) == 0) {
            const double fsec = pcm.p->pts == AV_NOPTS_VALUE
                ? 0.0 : pts_seconds(pcm.p->pts, atb);
            if (fsec < eff_end && !encode_audio_frame()) return false;
            av_frame_unref(pcm.p);
        }
    }

    venc.flush();
    if (have_audio) aenc.flush();

    // Mux the re-encoded streams.
    const std::string out_utf = wcs_to_utf8(out_path);
    mux::StreamSet streams;
    AVFormatContext* fmt = nullptr;
    // Error paths free the raw context (not RAII) — close pb first, since
    // avio_open may have succeeded while a later step failed.
    const auto free_fmt = [](AVFormatContext* f) {
        if (f) {
            avio_closep(&f->pb);
            avformat_free_context(f);
        }
    };
    const std::vector<AudioStreamParams> audio_params = have_audio
        ? std::vector<AudioStreamParams>{aenc.stream_params()}
        : std::vector<AudioStreamParams>{};
    if (!mux::alloc_output(out_utf, muxer_for_path(out_path),
                           venc.stream_params(), audio_params, &fmt, &streams))
        return false;
    if (!mux::open_file_and_write_header(fmt, out_utf, muxer_for_path(out_path))) {
        free_fmt(fmt);
        return false;
    }
    for (const auto& ep : vpkt) {
        if (!mux::write_packet(fmt, streams.video, ep, 0, 0)) {
            free_fmt(fmt);
            return false;
        }
    }
    if (have_audio) {
        for (const auto& ep : apkt) {
            if (!mux::write_packet(fmt, streams.audio[1], ep, 0, 0)) {
                free_fmt(fmt);
                return false;
            }
        }
    }
    if (!finalize_output(fmt, err)) {
        free_fmt(fmt);
        return false;
    }
    avformat_free_context(fmt);
    return true;
}

bool concat_clip_segments(const std::vector<ClipSegment>& segs,
                          double start_seconds, double end_seconds,
                          const std::wstring& out_path,
                          std::string* err)
{
    if (segs.empty()) {
        if (err) *err = "no segments to concatenate";
        return false;
    }
    if (start_seconds < 0.0 || end_seconds <= start_seconds) {
        if (err) *err = "end must be after start";
        return false;
    }

    // Layout comes from the first segment that actually overlaps the window.
    size_t first = 0;
    while (first < segs.size() && segs[first].end_seconds <= start_seconds) ++first;
    if (first >= segs.size()) {
        if (err) *err = "trim range is outside the recorded buffer";
        return false;
    }

    FmtCtx probe;
    std::vector<int> order;
    if (!open_input(segs[first].path, &probe, &order, err)) return false;

    const std::string out_utf = wcs_to_utf8(out_path);
    OutFmt out;
    std::vector<AVStream*> dst;
    if (!alloc_copy_output(out_path, probe.p, order, &out, &dst, err)) return false;
    if (!open_output(out.p, out_utf, err)) return false;

    double acc_sec = 0.0;
    for (size_t k = first; k < segs.size(); ++k) {
        const auto& seg = segs[k];
        const double local_start = std::max(0.0, start_seconds - seg.start_seconds);
        const double local_end   = std::min(seg.end_seconds - seg.start_seconds,
                                            end_seconds - seg.start_seconds);
        if (local_end <= local_start) break; // past the end window

        FmtCtx in;
        std::vector<int> in_order;
        if (!open_input(seg.path, &in, &in_order, err)) return false;
        if (in_order.size() != order.size()) {
            if (err) *err = "segments have inconsistent streams";
            return false;
        }
        // Every segment is produced by the same buffer with the same stream
        // layout (video first, then audio in order), so in_order[i] maps onto
        // dst[i] directly.
        if (!remux_window(in.p, in_order, local_start, local_end, acc_sec,
                          out.p, dst, err))
            return false;
        acc_sec += (local_end - local_start);
    }

    return finalize_output(out.p, err);
}

} // namespace encoding
