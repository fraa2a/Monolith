#include "trim.h"
#include "mux_common.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <deque>

namespace encoding {
namespace {
constexpr int kMaxPacketBytes = 256 * 1024 * 1024;
constexpr unsigned kMaxStreams = 64;
struct Input { AVFormatContext* p = nullptr; ~Input() { avformat_close_input(&p); } };
struct Output { AVFormatContext* p = nullptr; ~Output() { if (p) { avio_closep(&p->pb); avformat_free_context(p); } } };
struct Codec { AVCodecContext* p = nullptr; ~Codec() { avcodec_free_context(&p); } };
struct Packet { AVPacket* p = av_packet_alloc(); ~Packet() { av_packet_free(&p); } };
struct Frame { AVFrame* p = av_frame_alloc(); ~Frame() { av_frame_free(&p); } };
bool fail(std::string* err, const char* text) { if (err) *err = text; return false; }
bool cancelled(const std::atomic<bool>* cancel) { return cancel && cancel->load(); }
int interrupt_io(void* opaque) { return static_cast<const std::atomic<bool>*>(opaque)->load() ? 1 : 0; }
bool valid_range(double start, double end) {
    return std::isfinite(start) && std::isfinite(end) && start >= 0 && end > start && end < 1e9;
}
int64_t us(double seconds) { return static_cast<int64_t>(std::llround(seconds * AV_TIME_BASE)); }
int64_t origin(AVFormatContext* in) { return in->start_time == AV_NOPTS_VALUE ? 0 : in->start_time; }
double seconds(int64_t ts, AVRational tb) { return static_cast<double>(ts) * av_q2d(tb); }

bool open_input(const std::wstring& path, Input& in, std::vector<int>& order,
                std::string* err, const std::atomic<bool>* cancel = nullptr) {
    in.p = avformat_alloc_context();
    if (!in.p) return fail(err, "out of memory");
    if (cancel) in.p->interrupt_callback = {interrupt_io, const_cast<std::atomic<bool>*>(cancel)};
    if (avformat_open_input(&in.p, mux::wcs_to_utf8(path).c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(in.p, nullptr) < 0) return fail(err, "could not read input");
    if (in.p->nb_streams > kMaxStreams) return fail(err, "too many input streams");
    int video = av_find_best_stream(in.p, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video < 0) return fail(err, "input has no video stream");
    order.push_back(video);
    for (unsigned i = 0; i < in.p->nb_streams; ++i) {
        auto* s = in.p->streams[i];
        if (s->time_base.num <= 0 || s->time_base.den <= 0) return fail(err, "invalid input timebase");
        if (static_cast<int>(i) != video && s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            order.push_back(static_cast<int>(i));
        else if (static_cast<int>(i) != video && s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            return fail(err, "multiple video streams are not supported");
    }
    return true;
}

struct Window {
    int64_t anchor = AV_NOPTS_VALUE; // exact video PTS, not a rounded microsecond anchor
    AVRational tb{};
    int64_t decode_end_us = 0;
    double source_start = 0;
    double extent = 0;
};
int64_t packet_duration(const AVPacket* p, AVStream* s) {
    if (p->duration > 0) return p->duration;
    const AVRational rate = s->avg_frame_rate;
    return rate.num > 0 && rate.den > 0 ? std::max<int64_t>(1, av_rescale_q(1, av_inv_q(rate), s->time_base)) : 1;
}
// Matroska omits leading decode timestamps for reordered video. Recover only
// from the next known DTS by subtracting the intervening packet durations in
// the SAME input timebase. Never clamp DTS to PTS or fabricate a CFR timeline.
// Unresolvable/malformed prefixes fail explicitly rather than grow a queue.
struct TimestampReader {
    AVFormatContext* in;
    std::deque<AVPacket*> pending;
    size_t pending_bytes = 0;
    ~TimestampReader() { for (auto* p : pending) av_packet_free(&p); }
    int read(AVPacket* out) {
        for (;;) {
            if (!pending.empty()) {
                auto* first = pending.front();
                auto* stream = in->streams[first->stream_index];
                if (first->dts == AV_NOPTS_VALUE && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                    first->dts = first->pts;
                if (first->dts != AV_NOPTS_VALUE) {
                    pending_bytes -= static_cast<size_t>(first->size);
                    pending.pop_front(); av_packet_move_ref(out, first); av_packet_free(&first); return 0;
                }
                int64_t duration = 0;
                for (auto* next : pending) {
                    if (next->stream_index != first->stream_index) continue;
                    if (next->dts != AV_NOPTS_VALUE) { first->dts = next->dts - duration; break; }
                    if (next->duration <= 0) return AVERROR_INVALIDDATA;
                    duration += next->duration;
                }
                if (first->dts != AV_NOPTS_VALUE) continue;
            }
            if (pending.size() >= 256 || pending_bytes >= 64 * 1024 * 1024) return AVERROR_INVALIDDATA;
            auto* next = av_packet_alloc();
            if (!next) return AVERROR(ENOMEM);
            const int rc = av_read_frame(in, next);
            if (rc < 0) { av_packet_free(&next); return pending.empty() ? rc : AVERROR_INVALIDDATA; }
            if (next->size < 0 || static_cast<size_t>(next->size) > 64 * 1024 * 1024 - pending_bytes) {
                av_packet_free(&next); return AVERROR_INVALIDDATA;
            }
            pending_bytes += static_cast<size_t>(next->size);
            pending.push_back(next);
        }
    }
};
bool seek(AVFormatContext* in, int video, int64_t time_us, std::string* err) {
    if (av_seek_frame(in, video, av_rescale_q(time_us, AV_TIME_BASE_Q, in->streams[video]->time_base),
                      AVSEEK_FLAG_BACKWARD) < 0) return fail(err, "could not seek to trim start");
    return true;
}
// A bounded-memory planning pass finds the key and presentation extent of a
// decode-order prefix. Do not stop at the first future PTS: it may be a reference
// for B frames whose presentation lies inside the requested interval.
bool plan_window(AVFormatContext* in, int video, double start, double end,
                 Window& w, std::string* err, const std::atomic<bool>* cancel) {
    w.tb = in->streams[video]->time_base;
    w.decode_end_us = origin(in) + us(end);
    if (!seek(in, video, origin(in) + us(start), err)) return false;
    Packet pkt;
    if (!pkt.p) return fail(err, "out of memory");
    int rc = 0;
    TimestampReader reader{in, {}};
    while (!cancelled(cancel) && (rc = reader.read(pkt.p)) >= 0) {
        if (pkt.p->size > kMaxPacketBytes) return fail(err, "packet exceeds trim limit");
        if (pkt.p->stream_index == video) {
            if (pkt.p->pts == AV_NOPTS_VALUE || pkt.p->dts == AV_NOPTS_VALUE)
                return fail(err, "video packet has no timestamp");
            if (av_compare_ts(pkt.p->dts, w.tb, w.decode_end_us, AV_TIME_BASE_Q) >= 0) break;
            if (w.anchor == AV_NOPTS_VALUE && (pkt.p->flags & AV_PKT_FLAG_KEY)) w.anchor = pkt.p->pts;
            if (w.anchor != AV_NOPTS_VALUE) {
                const double packet_end = seconds(pkt.p->pts - w.anchor + packet_duration(pkt.p, in->streams[video]), w.tb);
                w.extent = std::max(w.extent, packet_end);
            }
        }
        av_packet_unref(pkt.p);
    }
    if (cancelled(cancel)) return fail(err, "save cancelled");
    if (rc < 0 && rc != AVERROR_EOF) return fail(err, "input read failed");
    if (w.anchor == AV_NOPTS_VALUE || w.extent <= 0) return fail(err, "no decodable video in trim window");
    w.source_start = seconds(w.anchor, w.tb) - static_cast<double>(origin(in)) / AV_TIME_BASE;
    if (w.source_start + w.extent <= start) return fail(err, "start is past the end of the clip");
    return true;
}

bool alloc_output(const std::wstring& path, AVFormatContext* in, const std::vector<int>& order,
                  Output& out, std::string* err, const std::atomic<bool>* cancel = nullptr) {
    auto ext = std::filesystem::path(path).extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(c >= L'A' && c <= L'Z' ? c + (L'a' - L'A') : c); });
    if (avformat_alloc_output_context2(&out.p, nullptr, ext == L".mp4" ? "mp4" : "matroska",
                                       mux::wcs_to_utf8(path).c_str()) < 0 || !out.p)
        return fail(err, "could not allocate output muxer");
    if (cancel) out.p->interrupt_callback = {interrupt_io, const_cast<std::atomic<bool>*>(cancel)};
    // Preserve negative decode timestamps rather than shifting streams by
    // independent anchors. MP4 edit lists represent initial decode preroll.
    out.p->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
    for (int idx : order) {
        auto* src = in->streams[idx];
        if (avformat_query_codec(out.p->oformat, src->codecpar->codec_id, FF_COMPLIANCE_NORMAL) == 0)
            return fail(err, "output container cannot preserve an input codec");
        auto* dst = avformat_new_stream(out.p, nullptr);
        if (!dst || avcodec_parameters_copy(dst->codecpar, src->codecpar) < 0) return fail(err, "could not copy stream");
        dst->codecpar->codec_tag = 0;
        dst->time_base = src->time_base;
        dst->avg_frame_rate = src->avg_frame_rate;
        dst->sample_aspect_ratio = src->sample_aspect_ratio;
        dst->disposition = src->disposition;
        av_dict_copy(&dst->metadata, src->metadata, 0);
    }
    return true;
}
bool header(Output& out, const std::wstring& path, std::string* err) {
    if (avio_open2(&out.p->pb, mux::wcs_to_utf8(path).c_str(), AVIO_FLAG_WRITE,
                   &out.p->interrupt_callback, nullptr) < 0 || avformat_write_header(out.p, nullptr) < 0)
        return fail(err, "could not write output header");
    return true;
}
bool finish(Output& out, std::string* err) {
    if (av_write_trailer(out.p) < 0 || avio_closep(&out.p->pb) < 0) return fail(err, "could not finalize output");
    return true;
}
bool retained_result(const std::wstring& path, double anchor, TrimResult* result, std::string* err,
                     AVFormatContext* expected, const std::atomic<bool>* cancel = nullptr) {
    if (cancelled(cancel)) return fail(err, "save cancelled");
    Input check;
    check.p = avformat_alloc_context();
    if (!check.p) return fail(err, "out of memory");
    if (cancel) check.p->interrupt_callback = {interrupt_io, const_cast<std::atomic<bool>*>(cancel)};
    if (avformat_open_input(&check.p, mux::wcs_to_utf8(path).c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(check.p, nullptr) < 0 || check.p->duration <= 0)
        return fail(err, "could not validate finalized output duration");
    if (check.p->nb_streams != expected->nb_streams) return fail(err, "output lost an input stream");
    for (unsigned i = 0; i < expected->nb_streams; ++i) {
        auto* a = expected->streams[i]->codecpar; auto* b = check.p->streams[i]->codecpar;
        if (a->codec_id != b->codec_id || a->codec_type != b->codec_type ||
            a->sample_rate != b->sample_rate || a->ch_layout.nb_channels != b->ch_layout.nb_channels)
            return fail(err, "output changed stream compatibility");
    }
    if (result) *result = {anchor, static_cast<double>(check.p->duration) / AV_TIME_BASE};
    return true;
}
// Rescale every timestamp/duration AFTER header. One exact video anchor is
// converted to each destination timebase; PTS-DTS and interstream offsets survive.
bool write_packet(AVPacket* pkt, AVRational src_tb, int dst_idx, const Window& w,
                  int64_t offset_us, Output& out, std::string* err) {
    if (pkt->size > kMaxPacketBytes || pkt->pts == AV_NOPTS_VALUE || pkt->dts == AV_NOPTS_VALUE)
        return fail(err, "invalid or oversized packet");
    const auto dst_tb = out.p->streams[dst_idx]->time_base;
    av_packet_rescale_ts(pkt, src_tb, dst_tb);
    const int64_t shift = av_rescale_q(w.anchor, w.tb, dst_tb) - av_rescale_q(offset_us, AV_TIME_BASE_Q, dst_tb);
    pkt->pts -= shift; pkt->dts -= shift; pkt->pos = -1; pkt->stream_index = dst_idx;
    // No libavformat interleaving queue proportional to skew or clip length.
    // Demux/encoder order is streamed directly; muxers accept cross-stream skew.
    return av_write_frame(out.p, pkt) >= 0 || fail(err, "failed to mux output packet");
}
bool copy_window(AVFormatContext* in, const std::vector<int>& order, const Window& w,
                 int64_t offset_us, Output& out, std::string* err, const std::atomic<bool>* cancel,
                 std::optional<std::pair<double, double>> audio_window = std::nullopt) {
    if (!seek(in, order[0], av_rescale_q(w.anchor, w.tb, AV_TIME_BASE_Q), err)) return false;
    Packet pkt;
    if (!pkt.p) return fail(err, "out of memory");
    std::vector<bool> done(order.size(), false);
    bool video_started = false;
    int rc = 0;
    TimestampReader reader{in, {}};
    while (!cancelled(cancel) && (rc = reader.read(pkt.p)) >= 0) {
        auto it = std::find(order.begin(), order.end(), pkt.p->stream_index);
        if (it != order.end()) {
            const auto idx = static_cast<size_t>(it - order.begin());
            const auto tb = in->streams[*it]->time_base;
            if (pkt.p->pts == AV_NOPTS_VALUE || pkt.p->dts == AV_NOPTS_VALUE) return fail(err, "missing packet timestamp");
            bool keep = false;
            if (idx == 0) {
                if ((pkt.p->flags & AV_PKT_FLAG_KEY) && pkt.p->pts == w.anchor) video_started = true;
                done[idx] = av_compare_ts(pkt.p->dts, tb, w.decode_end_us, AV_TIME_BASE_Q) >= 0;
                keep = video_started && !done[idx];
            } else {
                const double lower = audio_window ? audio_window->first : seconds(w.anchor, w.tb);
                const double upper = audio_window ? audio_window->second : seconds(w.anchor, w.tb) + w.extent;
                done[idx] = seconds(pkt.p->pts, tb) >= upper;
                keep = !done[idx] && (audio_window ? seconds(pkt.p->pts, tb) >= lower
                    : av_compare_ts(pkt.p->pts, tb, w.anchor, w.tb) >= 0);
            }
            if (keep && !write_packet(pkt.p, tb, static_cast<int>(idx), w, offset_us, out, err)) return false;
        }
        av_packet_unref(pkt.p);
        if (std::all_of(done.begin(), done.end(), [](bool v) { return v; })) break;
    }
    if (cancelled(cancel)) return fail(err, "save cancelled");
    if (rc < 0 && rc != AVERROR_EOF) return fail(err, "input read failed");
    return true;
}
bool same_layout(AVFormatContext* a, const std::vector<int>& ao, AVFormatContext* b, const std::vector<int>& bo) {
    if (ao.size() != bo.size()) return false;
    for (size_t i = 0; i < ao.size(); ++i) {
        auto* x = a->streams[ao[i]]->codecpar; auto* y = b->streams[bo[i]]->codecpar;
        if (x->codec_id != y->codec_id || x->width != y->width || x->height != y->height ||
            x->sample_rate != y->sample_rate || av_channel_layout_compare(&x->ch_layout, &y->ch_layout) != 0 ||
            x->extradata_size != y->extradata_size || (x->extradata_size && std::memcmp(x->extradata, y->extradata, x->extradata_size))) return false;
    }
    return true;
}
} // namespace

bool trim_clip_lossless(const std::wstring& path, double start, double end, const std::wstring& out_path,
                        std::string* err, TrimResult* result) {
    if (result) *result = {};
    if (!valid_range(start, end)) return fail(err, "invalid trim interval");
    Input in; std::vector<int> order;
    if (!open_input(path, in, order, err)) return false;
    Window w;
    if (!plan_window(in.p, order[0], start, end, w, err, nullptr)) return false;
    Output out;
    if (!alloc_output(out_path, in.p, order, out, err) || !header(out, out_path, err) ||
        !copy_window(in.p, order, w, 0, out, err, nullptr) || !finish(out, err)) return false;
    return retained_result(out_path, w.source_start, result, err, out.p);
}

bool trim_clip_reencode(const std::wstring& path, double start, double end, const std::wstring& out_path,
                        std::string* err, TrimResult* result) {
    if (result) *result = {};
    if (!valid_range(start, end)) return fail(err, "invalid trim interval");
    Input in; std::vector<int> order;
    if (!open_input(path, in, order, err)) return false;
    auto* vs = in.p->streams[order[0]];
    const char* name = vs->codecpar->codec_id == AV_CODEC_ID_H264 ? "libx264" :
                       vs->codecpar->codec_id == AV_CODEC_ID_HEVC ? "libx265" :
                       vs->codecpar->codec_id == AV_CODEC_ID_AV1 ? "libaom-av1" : nullptr;
    const AVCodec* encoder = name ? avcodec_find_encoder_by_name(name) : nullptr;
    const AVCodec* decoder = avcodec_find_decoder(vs->codecpar->codec_id);
    if (!encoder || !decoder) return fail(err, "same-codec software fallback unavailable");
    Codec dec, enc;
    dec.p = avcodec_alloc_context3(decoder); enc.p = avcodec_alloc_context3(encoder);
    if (!dec.p || !enc.p || avcodec_parameters_to_context(dec.p, vs->codecpar) < 0) return fail(err, "out of memory");
    dec.p->thread_count = 2; dec.p->pkt_timebase = vs->time_base;
    if (avcodec_open2(dec.p, decoder, nullptr) < 0) return fail(err, "could not open video decoder");
    Output out;
    if (!alloc_output(out_path, in.p, order, out, err)) return false;
    enc.p->width = vs->codecpar->width; enc.p->height = vs->codecpar->height;
    enc.p->pix_fmt = static_cast<AVPixelFormat>(vs->codecpar->format);
    enc.p->time_base = vs->time_base; enc.p->framerate = vs->avg_frame_rate;
    enc.p->sample_aspect_ratio = vs->sample_aspect_ratio;
    enc.p->color_range = vs->codecpar->color_range; enc.p->colorspace = vs->codecpar->color_space;
    enc.p->color_primaries = vs->codecpar->color_primaries; enc.p->color_trc = vs->codecpar->color_trc;
    enc.p->bit_rate = vs->codecpar->bit_rate > 0 ? vs->codecpar->bit_rate : 20000000;
    enc.p->thread_count = 2; enc.p->max_b_frames = 2; enc.p->gop_size = 120;
    if (out.p->oformat->flags & AVFMT_GLOBALHEADER) enc.p->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(enc.p, encoder, nullptr) < 0) return fail(err, "same-codec encoder cannot preserve input format");
    if (avcodec_parameters_from_context(out.p->streams[0]->codecpar, enc.p) < 0 || !header(out, out_path, err)) return false;
    if (!seek(in.p, order[0], origin(in.p) + us(start), err)) return false;
    Packet input, encoded; Frame frame;
    if (!input.p || !encoded.p || !frame.p) return fail(err, "out of memory");
    Window w; w.tb = vs->time_base;
    bool video_done = false;
    bool planning = true;
    // A decoder-only planning pass finds actual frame boundaries. A second
    // bounded-memory pass encodes video and copies all audio incrementally.
    auto drain_encoder = [&]() {
        int rc;
        while ((rc = avcodec_receive_packet(enc.p, encoded.p)) >= 0) {
            bool ok = write_packet(encoded.p, enc.p->time_base, 0, w, 0, out, err);
            av_packet_unref(encoded.p);
            if (!ok) return false;
        }
        return rc == AVERROR(EAGAIN) || rc == AVERROR_EOF || fail(err, "video encode failed");
    };
    auto drain_decoder = [&]() {
        int rc;
        while ((rc = avcodec_receive_frame(dec.p, frame.p)) >= 0) {
            const int64_t pts = frame.p->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) return fail(err, "decoded frame has no timestamp");
            const double t = seconds(pts, vs->time_base) - static_cast<double>(origin(in.p)) / AV_TIME_BASE;
            if (t >= end) video_done = true;
            if (t >= start && t < end) {
                if (frame.p->format != enc.p->pix_fmt || frame.p->width != enc.p->width || frame.p->height != enc.p->height)
                    return fail(err, "video format changed during trim");
                if (w.anchor == AV_NOPTS_VALUE) { w.anchor = pts; w.source_start = t; }
                frame.p->pts = pts; frame.p->pict_type = AV_PICTURE_TYPE_NONE;
                const int64_t duration = frame.p->duration > 0 ? frame.p->duration :
                    (vs->avg_frame_rate.num > 0 ? av_rescale_q(1, av_inv_q(vs->avg_frame_rate), vs->time_base) : 1);
                w.extent = std::max(w.extent, seconds(pts - w.anchor + duration, vs->time_base));
                if (!planning && (avcodec_send_frame(enc.p, frame.p) < 0 || !drain_encoder())) return fail(err, "video encode failed");
            }
            av_frame_unref(frame.p);
        }
        return rc == AVERROR(EAGAIN) || rc == AVERROR_EOF || fail(err, "video decode failed");
    };
    int rc = 0;
    while (!video_done && (rc = av_read_frame(in.p, input.p)) >= 0) {
        if (input.p->size > kMaxPacketBytes) return fail(err, "packet exceeds trim limit");
        // Feed ALL preroll packets. Only decoded, presentation-ordered frames
        // are filtered, including frames emitted during decoder drain.
        if (input.p->stream_index == order[0] &&
            (avcodec_send_packet(dec.p, input.p) < 0 || !drain_decoder())) return fail(err, "video decode failed");
        av_packet_unref(input.p);
    }
    if (rc < 0 && rc != AVERROR_EOF) return fail(err, "input read failed");
    if (avcodec_send_packet(dec.p, nullptr) < 0 || !drain_decoder()) return false;
    if (w.anchor == AV_NOPTS_VALUE) return fail(err, "no video frames in trim window");
    planning = false; video_done = false;
    avcodec_flush_buffers(dec.p);
    if (!seek(in.p, order[0], origin(in.p) + us(start), err)) return false;
    std::vector<bool> done(order.size(), false);
    while ((rc = av_read_frame(in.p, input.p)) >= 0) {
        if (input.p->size > kMaxPacketBytes) return fail(err, "packet exceeds trim limit");
        if (input.p->stream_index == order[0] && !video_done) {
            if (avcodec_send_packet(dec.p, input.p) < 0 || !drain_decoder()) return fail(err, "video decode failed");
            done[0] = video_done;
        } else {
            auto it = std::find(order.begin() + 1, order.end(), input.p->stream_index);
            if (it != order.end()) {
                const auto idx = static_cast<size_t>(it - order.begin());
                const auto tb = in.p->streams[*it]->time_base;
                if (input.p->pts == AV_NOPTS_VALUE) return fail(err, "audio packet has no timestamp");
                const double t = seconds(input.p->pts, tb) - seconds(w.anchor, w.tb);
                done[idx] = t >= w.extent;
                if (t >= 0 && !done[idx] && !write_packet(input.p, tb, static_cast<int>(idx), w, 0, out, err)) return false;
            }
        }
        av_packet_unref(input.p);
        if (std::all_of(done.begin(), done.end(), [](bool v) { return v; })) break;
    }
    if (rc < 0 && rc != AVERROR_EOF) return fail(err, "input read failed");
    if (avcodec_send_packet(dec.p, nullptr) < 0 || !drain_decoder()) return false;
    if (avcodec_send_frame(enc.p, nullptr) < 0 || !drain_encoder()) return false;
    if (!finish(out, err)) return false;
    return retained_result(out_path, w.source_start, result, err, out.p);
}

bool concat_clip_segments(const std::vector<ClipSegment>& segs, double start, double end,
                          const std::wstring& path, std::string* err, TrimResult* result,
                          const std::atomic<bool>* cancel) {
    if (result) *result = {};
    if (!std::isfinite(start) || !std::isfinite(end) || start <= -1e9 || end >= 1e9 || end <= start || segs.empty() || segs.size() > 4096) return fail(err, "invalid segment window");
    Input first; std::vector<int> first_order; Output out;
    bool opened = false; double global_anchor = 0;
    for (const auto& seg : segs) {
        if (seg.end_seconds <= start || seg.start_seconds >= end) continue;
        if (cancelled(cancel)) return fail(err, "save cancelled");
        Input in; std::vector<int> order;
        if (!open_input(seg.path, in, order, err, cancel)) return false;
        const double local_start = std::max(0.0, start - seg.start_seconds) + seg.file_origin_seconds;
        const double local_end = std::min(end, seg.end_seconds) - seg.start_seconds + seg.file_origin_seconds;
        Window w;
        if (!plan_window(in.p, order[0], local_start, local_end, w, err, cancel)) return false;
        const double actual_start = seg.start_seconds + w.source_start - seg.file_origin_seconds;
        const bool first_segment = !opened;
        if (!opened) {
            if (!open_input(seg.path, first, first_order, err, cancel) ||
                !alloc_output(path, in.p, order, out, err, cancel) || !header(out, path, err)) return false;
            opened = true; global_anchor = actual_start;
        } else if (!same_layout(first.p, first_order, in.p, order)) return fail(err, "segments have incompatible stream parameters");
        const double offset = actual_start - global_anchor;
        // Segment ownership changes at video DTS, not its delayed presentation
        // timestamp. Applying each key's PTS as a fresh audio trim drops AAC at
        // every B-frame GOP boundary. Only the outer start uses the retained
        // presentation anchor; internal audio uses the common source timeline.
        const double local_origin = static_cast<double>(origin(in.p)) / AV_TIME_BASE;
        const double audio_start = first_segment ? seconds(w.anchor, w.tb)
            : seg.file_origin_seconds + local_origin;
        const double audio_end = seg.end_seconds < end
            ? seg.end_seconds - seg.start_seconds + seg.file_origin_seconds + local_origin
            : seconds(w.anchor, w.tb) + w.extent;
        if (!copy_window(in.p, order, w, us(offset), out, err, cancel,
                         std::pair{audio_start, audio_end})) return false;

    }
    if (!opened) return fail(err, "no segments in window");
    if (!finish(out, err)) return false;
    return retained_result(path, global_anchor, result, err, out.p, cancel);
}
} // namespace encoding
