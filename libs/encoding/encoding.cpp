#include "encoding.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace encoding {

static uint64_t steady_now_us()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

static EncodedBytesRef make_encoded_bytes(const uint8_t* data, int size)
{
    if (!data || size <= 0) return {};
    auto mutable_bytes = std::make_shared<EncodedBytes>();
    mutable_bytes->size = static_cast<size_t>(size);
    mutable_bytes->data = std::make_unique<uint8_t[]>(mutable_bytes->size);
    memcpy(mutable_bytes->data.get(), data, mutable_bytes->size);
    return std::static_pointer_cast<const EncodedBytes>(mutable_bytes);
}

// ── probe_video_encoder ───────────────────────────────────────────────────────

static const char* kEncoderCandidates[] = {
    "h264_nvenc", "h264_amf", "h264_qsv", "libx264",
    "hevc_nvenc", "hevc_amf", "hevc_qsv", "libx265",
    "av1_nvenc", "av1_amf", "av1_qsv", "libaom-av1",
    nullptr
};

static bool try_open_probe(const char* name, int width, int height)
{
    const AVCodec* codec = avcodec_find_encoder_by_name(name);
    if (!codec) return false;

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) return false;

    ctx->width      = (width  + 1) & ~1; // NVENC requires even dimensions
    ctx->height     = (height + 1) & ~1;
    ctx->time_base  = {1, 60};
    ctx->framerate  = {60, 1};
    ctx->pix_fmt    = AV_PIX_FMT_YUV420P;
    ctx->bit_rate   = 4'000'000;
    ctx->gop_size   = 60;

    AVDictionary* opts = nullptr;
    // libx264/libx265 accept "ultrafast"; libaom-av1 does not (it has
    // cpu-used instead), so leave it unset there.
    if (strcmp(name, "libx264") == 0 || strcmp(name, "libx265") == 0)
        av_dict_set(&opts, "preset", "ultrafast", 0);

    bool ok = (avcodec_open2(ctx, codec, &opts) == 0);
    av_dict_free(&opts);
    avcodec_free_context(&ctx);
    return ok;
}

std::string probe_video_encoder(int width, int height)
{
    // Suppress noise during probing.
    const int saved_level = av_log_get_level();
    av_log_set_level(AV_LOG_QUIET);

    std::string result;
    for (int i = 0; kEncoderCandidates[i]; ++i) {
        if (try_open_probe(kEncoderCandidates[i], width, height)) {
            result = kEncoderCandidates[i];
            break;
        }
    }

    av_log_set_level(saved_level);
    return result;
}

std::vector<std::string> available_video_encoders(int width, int height)
{
    const int saved_level = av_log_get_level();
    av_log_set_level(AV_LOG_QUIET);

    std::vector<std::string> result;
    for (int i = 0; kEncoderCandidates[i]; ++i) {
        if (try_open_probe(kEncoderCandidates[i], width, height))
            result.push_back(kEncoderCandidates[i]);
    }

    av_log_set_level(saved_level);
    return result;
}

std::string resolve_video_encoder(const std::string& device,
                                  const std::string& codec,
                                  int width, int height)
{
    const bool h265 = (codec == "h265" || codec == "hevc");
    const bool av1  = (codec == "av1");
    // HW candidates for the codec, in vendor-preference order.
    const std::vector<std::string> hw = h265
        ? std::vector<std::string>{"hevc_nvenc", "hevc_amf", "hevc_qsv"}
        : (av1
            ? std::vector<std::string>{"av1_nvenc", "av1_amf", "av1_qsv"}
            : std::vector<std::string>{"h264_nvenc", "h264_amf", "h264_qsv"});
    const std::string sw = h265 ? "libx265" : (av1 ? "libaom-av1" : "libx264");

    auto first_openable = [&](const std::vector<std::string>& names) -> std::string {
        const int saved_level = av_log_get_level();
        av_log_set_level(AV_LOG_QUIET);
        std::string found;
        for (const auto& n : names) {
            if (try_open_probe(n.c_str(), width, height)) { found = n; break; }
        }
        av_log_set_level(saved_level);
        return found;
    };

    if (device == "cpu") {
        std::string r = first_openable({sw});
        if (!r.empty()) return r;
        return first_openable(hw); // fall back to HW if SW somehow fails
    }

    // device == "gpu" (default): try HW encoders, then fall back to SW.
    std::string r = first_openable(hw);
    if (!r.empty()) return r;
    return first_openable({sw});
}

// ── VideoEncoder ──────────────────────────────────────────────────────────────

struct VideoEncoder::Impl {
    std::mutex       mutex;
    AVCodecContext* ctx      = nullptr;
    AVFrame*        frame    = nullptr;
    SwsContext*     sws      = nullptr;
    PacketSink      sink;
    int64_t         next_pts = 0;
    int             cfg_w    = 0;   // output (encoded) dimensions
    int             cfg_h    = 0;
    int             src_w    = 0;   // source dimensions the sws ctx was built for
    int             src_h    = 0;
    int             src_fmt  = AV_PIX_FMT_NONE; // and its pixel format
    std::string     enc_name;
    bool            extra_rejected = false;
    int             sws_flags = SWS_BILINEAR;
    int             cfg_fps  = 0;
    VideoStreamParams vsp;
    VideoEncoderPerfStats perf;
};

VideoEncoder::VideoEncoder()  : impl_(new Impl()) {}
VideoEncoder::~VideoEncoder() { close(); delete impl_; }

bool VideoEncoder::is_open() const
{
    std::lock_guard lk(impl_->mutex);
    return impl_->ctx != nullptr;
}

VideoStreamParams VideoEncoder::stream_params() const
{
    std::lock_guard lk(impl_->mutex);
    return impl_->vsp;
}

std::string VideoEncoder::encoder_name() const
{
    std::lock_guard lk(impl_->mutex);
    return impl_->enc_name;
}

bool VideoEncoder::extra_options_rejected() const
{
    std::lock_guard lk(impl_->mutex);
    return impl_->extra_rejected;
}

VideoEncoderPerfStats VideoEncoder::perf_stats() const
{
    std::lock_guard lk(impl_->mutex);
    return impl_->perf;
}

bool VideoEncoder::open(Config const& cfg, PacketSink sink)
{
    if (cfg.width <= 0 || cfg.height <= 0 || cfg.fps <= 0)
        return false;
    if (cfg.bitrate <= 0 && cfg.quality <= 0)
        return false;
    // Legacy quality knob, clamped to the encoder-supported range without
    // mutating the caller's config.
    const int quality = (cfg.quality > 0)
        ? std::max(10, std::min(30, cfg.quality)) : 0;
    // HW encoders (NVENC/QSV) refuse odd dimensions; align once here so the
    // probe (which also aligns) and the real open agree on the frame size.
    const int out_w = (cfg.width  + 1) & ~1;
    const int out_h = (cfg.height + 1) & ~1;

    close();
    std::lock_guard lk(impl_->mutex);
    auto cleanup = [this]() {
        if (impl_->sws)   { sws_freeContext(impl_->sws);   impl_->sws   = nullptr; }
        if (impl_->frame) { av_frame_free(&impl_->frame);               }
        if (impl_->ctx)   { avcodec_free_context(&impl_->ctx);           }
        impl_->next_pts = 0;
        impl_->cfg_w    = 0;
        impl_->cfg_h    = 0;
        impl_->src_w    = 0;
        impl_->src_h    = 0;
        impl_->src_fmt  = AV_PIX_FMT_NONE;
        impl_->enc_name.clear();
        impl_->vsp      = {};
        impl_->sink     = nullptr;
        impl_->cfg_fps  = 0;
        impl_->perf     = {};
    };

    impl_->sink  = std::move(sink);
    impl_->cfg_w = out_w;
    impl_->cfg_h = out_h;
    impl_->extra_rejected = false;

    // Candidate order: preferred encoder first (when set), then probe order.
    std::vector<std::string> order;
    if (!cfg.preferred_encoder.empty() && cfg.preferred_encoder != "auto")
        order.push_back(cfg.preferred_encoder);
    for (int i = 0; kEncoderCandidates[i]; ++i) {
        if (order.empty() || order[0] != kEncoderCandidates[i])
            order.push_back(kEncoderCandidates[i]);
    }

    auto try_open = [&](const std::string& name, bool use_extras) -> bool {
        const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
        if (!codec) return false;

        impl_->ctx = avcodec_alloc_context3(codec);
        if (!impl_->ctx) return false;
        AVCodecContext* ctx = impl_->ctx;

        const bool is_hw = (name.find("nvenc") != std::string::npos ||
                            name.find("amf")   != std::string::npos ||
                            name.find("qsv")   != std::string::npos);
        const bool is_sw = (name == "libx264" || name == "libx265" ||
                            name == "libaom-av1");

        ctx->width     = out_w;
        ctx->height    = out_h;
        ctx->time_base = {1, cfg.fps};
        ctx->framerate = {cfg.fps, 1};
        ctx->gop_size  = cfg.fps * 2;  // keyframe every 2 s
        ctx->pix_fmt   = AV_PIX_FMT_YUV420P;
        ctx->flags    |= AV_CODEC_FLAG_GLOBAL_HEADER; // SPS/PPS in extradata

        // Offline encodes (trim re-encode) want all cores; the live path keeps
        // libavcodec defaults so capture latency is unchanged.
        if (is_sw && !cfg.low_latency)
            ctx->thread_count = 0; // auto

        AVDictionary* opts = nullptr;

        if (quality > 0 && (is_hw || is_sw)) {
            // Legacy path (probe/back-compat only): CQP for HW, CRF for SW.
            if (is_hw) {
                if (name.find("nvenc") != std::string::npos) {
                    av_dict_set(&opts, "rc",  "constqp", 0);
                    av_dict_set_int(&opts, "qp", quality, 0);
                } else if (name.find("amf") != std::string::npos) {
                    av_dict_set(&opts, "rc",     "cqp", 0);
                    av_dict_set_int(&opts, "qp_i", quality, 0);
                    av_dict_set_int(&opts, "qp_p", quality, 0);
                    av_dict_set_int(&opts, "qp_b", quality, 0);
                } else if (name.find("qsv") != std::string::npos) {
                    av_dict_set(&opts, "rc", "CQP", 0);
                    av_dict_set_int(&opts, "qp", quality, 0);
                }
            } else {
                av_dict_set_int(&opts, "crf", quality, 0);
            }
        } else {
            // CBR: pin the average, ceiling and floor to the same target so the
            // muxed file honours the exact bitrate the user picked, with a 1 s
            // VBV buffer. Per-vendor RC hints keep HW encoders in true CBR.
            const int64_t br = cfg.bitrate;
            ctx->bit_rate     = br;
            ctx->rc_max_rate  = br;
            ctx->rc_min_rate  = br;
            ctx->rc_buffer_size = static_cast<int>(std::min<int64_t>(br, INT_MAX));
            if (is_hw) {
                if (name.find("nvenc") != std::string::npos) {
                    av_dict_set(&opts, "rc", "cbr", 0);
                } else if (name.find("amf") != std::string::npos) {
                    av_dict_set(&opts, "rc", "cbr", 0);
                } else if (name.find("qsv") != std::string::npos) {
                    av_dict_set_int(&opts, "maxrate", br, 0);
                }
            } else if (is_sw) {
                // libx264/libx265: nal-hrd=cbr forces constant-rate output.
                // libaom-av1: rc-end-usage=cbr pins the rate-control mode.
                av_dict_set(&opts, "nal-hrd", "cbr", 0);
                if (name == "libx265") {
                    char x265[128];
                    std::snprintf(x265, sizeof(x265),
                        "vbv-maxrate=%lld:vbv-bufsize=%lld:strict-cbr=1",
                        static_cast<long long>(br / 1000),
                        static_cast<long long>(br / 1000));
                    av_dict_set(&opts, "x265-params", x265, 0);
                } else if (name == "libaom-av1") {
                    av_dict_set(&opts, "rc-end-usage", "cbr", 0);
                }
            }
        }

        if (is_sw) {
            // libx264/libx265 use "preset"/"tune"; libaom-av1 has neither
            // (its speed knob is cpu-used), so only the x26x pair gets them.
            // Offline encodes drop zerolatency (B-frames + frame threading:
            // better quality per bit) and use a quality-leaning speed knob.
            if (name == "libx264" || name == "libx265") {
                av_dict_set(&opts, "preset",
                            cfg.low_latency ? "fast" : "medium", 0);
                if (cfg.low_latency)
                    av_dict_set(&opts, "tune", "zerolatency", 0);
            } else if (name == "libaom-av1") {
                av_dict_set(&opts, "cpu-used",
                            cfg.low_latency ? "8" : "6", 0);
            }
        } else if (name.find("nvenc") != std::string::npos) {
            av_dict_set(&opts, "preset", "p4", 0);
        }
        if (use_extras && !cfg.extra_options.empty()) {
            av_dict_parse_string(&opts, cfg.extra_options.c_str(), "=", ":, ", 0);
        }

        bool ok = (avcodec_open2(ctx, codec, &opts) == 0);
        av_dict_free(&opts);
        if (!ok) avcodec_free_context(&impl_->ctx);
        return ok;
    };

    std::string opened;
    for (const auto& name : order) {
        if (try_open(name, true)) { opened = name; break; }
        if (!cfg.extra_options.empty() && try_open(name, false)) {
            impl_->extra_rejected = true;
            opened = name;
            break;
        }
    }
    if (opened.empty()) { cleanup(); return false; }
    impl_->cfg_fps  = cfg.fps;
    impl_->enc_name = opened;
    if (cfg.scaling_filter == "bicubic") {
        impl_->sws_flags = SWS_BICUBIC;
    } else if (cfg.scaling_filter == "lanczos") {
        impl_->sws_flags = SWS_LANCZOS;
    } else {
        impl_->sws_flags = SWS_BILINEAR;
    }
    AVCodecContext* ctx = impl_->ctx;

    impl_->frame = av_frame_alloc();
    if (!impl_->frame) { cleanup(); return false; }
    impl_->frame->format = ctx->pix_fmt;
    impl_->frame->width  = out_w;
    impl_->frame->height = out_h;
    if (av_frame_get_buffer(impl_->frame, 0) < 0) { cleanup(); return false; }

    // sws ctx is (re-)created lazily in push_bgra/push_frame for the actual
    // source size/format.
    impl_->src_w = 0;
    impl_->src_h = 0;
    impl_->src_fmt = AV_PIX_FMT_NONE;

    // Store stream params for replay buffer.
    impl_->vsp.codec   = (ctx->codec_id == AV_CODEC_ID_HEVC)
        ? VideoCodec::H265
        : (ctx->codec_id == AV_CODEC_ID_AV1 ? VideoCodec::AV1 : VideoCodec::H264);
    impl_->vsp.width   = out_w;
    impl_->vsp.height  = out_h;
    impl_->vsp.fps_num = cfg.fps;
    impl_->vsp.fps_den = 1;
    impl_->vsp.tb_num  = 1;
    impl_->vsp.tb_den  = cfg.fps;
    if (ctx->extradata_size > 0)
        impl_->vsp.extradata.assign(ctx->extradata,
                                    ctx->extradata + ctx->extradata_size);
    return true;
}

template <typename ImplT>
static void drain_video(ImplT* impl)
{
    // Zero-initialised stack packet: receive overwrites it, unref after each
    // use — no per-frame heap allocation in the hot path.
    AVPacket pkt{};
    while (avcodec_receive_packet(impl->ctx, &pkt) == 0) {
        EncodedPacket ep;
        ep.bytes        = make_encoded_bytes(pkt.data, pkt.size);
        ep.pts          = pkt.pts;
        ep.dts          = pkt.dts;
        ep.dts_usec     = av_rescale(pkt.dts, 1000000,
                                     impl->cfg_fps);
        ep.stream_index = 0;
        ep.is_keyframe  = (pkt.flags & AV_PKT_FLAG_KEY) != 0;
        ep.tb_num       = 1;
        ep.tb_den       = impl->cfg_fps;
        impl->perf.packets_output++;
        if (impl->sink) impl->sink(std::move(ep));
        av_packet_unref(&pkt);
    }
}

// Shared tail of push_bgra/push_frame: stamp pts and hand the frame to the
// encoder. Returns avcodec_send_frame's value. Caller holds the mutex.
// (Templated like drain_video so it never names the private Impl type.)
template <typename ImplT>
static int submit_current_frame(ImplT* impl, int64_t pts)
{
    // PTS: clock-locked frame index from the pacer (preferred), or fall back
    // to the internal counter when pts < 0.  The pacer is the single source
    // of timing truth — it emits exactly fps frames per real second (dup/skip),
    // so this PTS advances at wall-clock rate.
    if (pts >= 0) {
        impl->frame->pts = pts;
        impl->next_pts   = pts + 1;
    } else {
        impl->frame->pts = impl->next_pts;
    }
    const int ret = avcodec_send_frame(impl->ctx, impl->frame);
    if (ret == 0 && pts < 0) impl->next_pts++;
    return ret;
}

void VideoEncoder::push_bgra(const uint8_t* bgra, int stride, int width, int height,
                             int64_t pts)
{
    std::lock_guard lk(impl_->mutex);
    if (!impl_->ctx || !bgra) return;
    if (width <= 0 || height <= 0) return;

    // (Re-)create the scaler when the source size changes (display mode
    // switches, or output resolution differs from capture resolution).
    if (!impl_->sws || width != impl_->src_w || height != impl_->src_h ||
        impl_->src_fmt != AV_PIX_FMT_BGRA) {
        if (impl_->sws) sws_freeContext(impl_->sws);
        impl_->sws = sws_getContext(
            width, height, AV_PIX_FMT_BGRA,
            impl_->cfg_w, impl_->cfg_h, impl_->ctx->pix_fmt,
            impl_->sws_flags, nullptr, nullptr, nullptr);
        if (!impl_->sws) return;
        impl_->src_w = width;
        impl_->src_h = height;
        impl_->src_fmt = AV_PIX_FMT_BGRA;
    }

    if (av_frame_make_writable(impl_->frame) < 0) return;

    const uint8_t* src_slices[1] = { bgra };
    int            src_stride[1] = { stride };
    const uint64_t sws_start_us = steady_now_us();
    sws_scale(impl_->sws,
        src_slices, src_stride, 0, height,
        impl_->frame->data, impl_->frame->linesize);
    impl_->perf.sws_scale_time_us_total += steady_now_us() - sws_start_us;

    const uint64_t encode_start_us = steady_now_us();
    if (submit_current_frame(impl_, pts) == 0)
        impl_->perf.frames_submitted++;
    drain_video(impl_);
    impl_->perf.encode_time_us_total += steady_now_us() - encode_start_us;
}

void VideoEncoder::push_frame(const AVFrame* frame, int64_t pts)
{
    std::lock_guard lk(impl_->mutex);
    if (!impl_->ctx || !frame || frame->width <= 0 || frame->height <= 0) return;

    if (frame->format == static_cast<int>(impl_->ctx->pix_fmt) &&
        frame->width == impl_->cfg_w && frame->height == impl_->cfg_h) {
        // Decoder output already matches the encoder format and size: share
        // the planes instead of round-tripping YUV→BGRA→YUV (saves a full
        // sws pass per frame and its rounding loss).
        av_frame_unref(impl_->frame);
        if (av_frame_ref(impl_->frame, frame) < 0) return;
    } else {
        if (!impl_->sws || frame->width != impl_->src_w ||
            frame->height != impl_->src_h ||
            frame->format != impl_->src_fmt) {
            if (impl_->sws) sws_freeContext(impl_->sws);
            impl_->sws = sws_getContext(
                frame->width, frame->height,
                static_cast<AVPixelFormat>(frame->format),
                impl_->cfg_w, impl_->cfg_h, impl_->ctx->pix_fmt,
                impl_->sws_flags, nullptr, nullptr, nullptr);
            if (!impl_->sws) return;
            impl_->src_w = frame->width;
            impl_->src_h = frame->height;
            impl_->src_fmt = frame->format;
        }
        if (av_frame_make_writable(impl_->frame) < 0) return;
        sws_scale(impl_->sws,
            frame->data, frame->linesize, 0, frame->height,
            impl_->frame->data, impl_->frame->linesize);
    }

    if (submit_current_frame(impl_, pts) == 0)
        impl_->perf.frames_submitted++;
    drain_video(impl_);
}

void VideoEncoder::flush()
{
    std::lock_guard lk(impl_->mutex);
    if (!impl_->ctx) return;
    avcodec_send_frame(impl_->ctx, nullptr);
    drain_video(impl_);
}

void VideoEncoder::close()
{
    std::lock_guard lk(impl_->mutex);
    if (impl_->ctx) {
        avcodec_send_frame(impl_->ctx, nullptr);
        drain_video(impl_);
    }
    if (impl_->sws)   { sws_freeContext(impl_->sws);   impl_->sws   = nullptr; }
    if (impl_->frame) { av_frame_free(&impl_->frame);               }
    if (impl_->ctx)   { avcodec_free_context(&impl_->ctx);           }
    impl_->next_pts = 0;
    impl_->src_w    = 0;
    impl_->src_h    = 0;
    impl_->src_fmt  = AV_PIX_FMT_NONE;
    impl_->enc_name.clear();
    impl_->vsp      = {};
}

// ── AudioEncoder ──────────────────────────────────────────────────────────────

struct AudioEncoder::Impl {
    std::mutex       mutex;
    AVCodecContext* ctx      = nullptr;
    AVFrame*        frame    = nullptr;
    SwrContext*     swr      = nullptr;
    AVAudioFifo*    fifo     = nullptr;
    PacketSink      sink;
    int64_t         next_pts = 0;
    // Remembered input format for swr reuse.
    int             swr_src_rate = 0;
    int             swr_src_ch   = 0;
    AVSampleFormat  swr_src_fmt  = AV_SAMPLE_FMT_NONE;
    // Reusable swr output planes (grown on demand). push_pcm runs every
    // ~10 ms per track — per-call alloc/free is avoidable churn.
    uint8_t**       conv_data    = nullptr;
    int             conv_linesize = 0;
    int             conv_cap     = 0; // capacity in samples
    int             stream_index = 1;
    int             cfg_sample_rate = 0;
    AudioStreamParams asp;

    void free_conv()
    {
        if (conv_data) {
            av_freep(&conv_data[0]);
            av_freep(&conv_data);
        }
        conv_cap = 0;
    }
};

AudioEncoder::AudioEncoder()  : impl_(new Impl()) {}
AudioEncoder::~AudioEncoder() { close(); delete impl_; }

bool AudioEncoder::is_open() const
{
    std::lock_guard lk(impl_->mutex);
    return impl_->ctx != nullptr;
}

AudioStreamParams AudioEncoder::stream_params() const
{
    std::lock_guard lk(impl_->mutex);
    return impl_->asp;
}

bool AudioEncoder::open(Config const& cfg, PacketSink sink)
{
    if (cfg.sample_rate <= 0 || cfg.channels <= 0 || cfg.bitrate <= 0)
        return false;
    if (cfg.stream_index < 1 || cfg.stream_index > 6)
        return false;

    close();
    std::lock_guard lk(impl_->mutex);
    auto cleanup = [this]() {
        if (impl_->fifo)  { av_audio_fifo_free(impl_->fifo); impl_->fifo  = nullptr; }
        if (impl_->swr)   { swr_free(&impl_->swr);                                   }
        if (impl_->frame) { av_frame_free(&impl_->frame);                            }
        if (impl_->ctx)   { avcodec_free_context(&impl_->ctx);                       }
        impl_->free_conv();
        impl_->next_pts        = 0;
        impl_->swr_src_rate    = 0;
        impl_->swr_src_ch      = 0;
        impl_->swr_src_fmt     = AV_SAMPLE_FMT_NONE;
        impl_->stream_index    = 1;
        impl_->asp             = {};
        impl_->sink            = nullptr;
        impl_->cfg_sample_rate = 0;
    };

    impl_->sink = std::move(sink);
    impl_->stream_index    = cfg.stream_index;
    impl_->cfg_sample_rate = cfg.sample_rate;

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) return false;

    impl_->ctx = avcodec_alloc_context3(codec);
    if (!impl_->ctx) return false;
    AVCodecContext* ctx = impl_->ctx;

    ctx->sample_rate = cfg.sample_rate;
    ctx->bit_rate    = cfg.bitrate;
    ctx->sample_fmt  = AV_SAMPLE_FMT_FLTP; // AAC encoder expects planar float
    ctx->time_base   = {1, cfg.sample_rate};
    ctx->flags      |= AV_CODEC_FLAG_GLOBAL_HEADER;
    av_channel_layout_default(&ctx->ch_layout, cfg.channels);

    if (avcodec_open2(ctx, codec, nullptr) < 0) { cleanup(); return false; }

    impl_->frame = av_frame_alloc();
    if (!impl_->frame) { cleanup(); return false; }
    impl_->frame->format      = ctx->sample_fmt;
    impl_->frame->sample_rate = ctx->sample_rate;
    av_channel_layout_copy(&impl_->frame->ch_layout, &ctx->ch_layout);
    impl_->frame->nb_samples  = ctx->frame_size; // typically 1024 for AAC
    if (av_frame_get_buffer(impl_->frame, 0) < 0) { cleanup(); return false; }

    impl_->fifo = av_audio_fifo_alloc(ctx->sample_fmt,
                                      ctx->ch_layout.nb_channels,
                                      ctx->frame_size * 4);
    if (!impl_->fifo) { cleanup(); return false; }

    impl_->asp.stream_index = cfg.stream_index;
    impl_->asp.sample_rate  = cfg.sample_rate;
    impl_->asp.channels     = cfg.channels;
    impl_->asp.tb_num       = 1;
    impl_->asp.tb_den       = cfg.sample_rate;
    if (ctx->extradata_size > 0)
        impl_->asp.extradata.assign(ctx->extradata,
                                    ctx->extradata + ctx->extradata_size);
    return true;
}

template <typename ImplT>
static void drain_audio(ImplT* impl)
{
    AVPacket pkt{}; // zero-initialised stack packet, unref after each use
    while (avcodec_receive_packet(impl->ctx, &pkt) == 0) {
        EncodedPacket ep;
        ep.bytes        = make_encoded_bytes(pkt.data, pkt.size);
        ep.pts          = pkt.pts;
        ep.dts          = pkt.dts;
        ep.dts_usec     = av_rescale(pkt.dts, 1000000,
                                     impl->cfg_sample_rate);
        ep.stream_index = impl->stream_index;
        ep.is_keyframe  = true; // AAC frames are all independently decodable
        ep.tb_num       = 1;
        ep.tb_den       = impl->cfg_sample_rate;
        if (impl->sink) impl->sink(std::move(ep));
        av_packet_unref(&pkt);
    }
}

static AVSampleFormat wasapi_to_av_fmt(int bit_depth, bool is_float)
{
    if (is_float) return (bit_depth == 64) ? AV_SAMPLE_FMT_DBL : AV_SAMPLE_FMT_FLT;
    switch (bit_depth) {
        case 16: return AV_SAMPLE_FMT_S16;
        case 32: return AV_SAMPLE_FMT_S32;
        default: return AV_SAMPLE_FMT_FLT; // best guess
    }
}

void AudioEncoder::push_pcm(const uint8_t* data, int bytes,
                             int sample_rate, int channels,
                             int bit_depth, bool is_float)
{
    std::lock_guard lk(impl_->mutex);
    if (!impl_->ctx || bytes <= 0) return;
    if (sample_rate <= 0 || channels <= 0 || bit_depth <= 0) return;

    AVSampleFormat src_fmt   = wasapi_to_av_fmt(bit_depth, is_float);
    int            src_frames = bytes / (bit_depth / 8 * channels);
    if (src_frames <= 0) return;

    std::vector<uint8_t> silence;
    const uint8_t* src_data = data;
    if (!src_data) {
        silence.resize(static_cast<size_t>(bytes), 0);
        src_data = silence.data();
    }

    // (Re-)init swr if the input format changed.
    bool need_swr = !impl_->swr
        || impl_->swr_src_rate != sample_rate
        || impl_->swr_src_ch   != channels
        || impl_->swr_src_fmt  != src_fmt;

    if (need_swr) {
        if (impl_->swr) swr_free(&impl_->swr);

        AVChannelLayout src_chl{};
        av_channel_layout_default(&src_chl, channels);

        swr_alloc_set_opts2(&impl_->swr,
            &impl_->ctx->ch_layout, impl_->ctx->sample_fmt, impl_->ctx->sample_rate,
            &src_chl,               src_fmt,                sample_rate,
            0, nullptr);
        av_channel_layout_uninit(&src_chl);

        if (!impl_->swr || swr_init(impl_->swr) < 0) {
            swr_free(&impl_->swr);
            return;
        }
        impl_->swr_src_rate = sample_rate;
        impl_->swr_src_ch   = channels;
        impl_->swr_src_fmt  = src_fmt;
    }

    // Grow (only) the reusable conversion buffer for this call's output.
    int max_out = (int)av_rescale_rnd(
        swr_get_delay(impl_->swr, sample_rate) + src_frames,
        impl_->ctx->sample_rate, sample_rate, AV_ROUND_UP);

    if (max_out > impl_->conv_cap) {
        impl_->free_conv();
        if (av_samples_alloc_array_and_samples(
                &impl_->conv_data, &impl_->conv_linesize,
                impl_->ctx->ch_layout.nb_channels,
                max_out, impl_->ctx->sample_fmt, 0) < 0) {
            impl_->conv_data = nullptr;
            return;
        }
        impl_->conv_cap = max_out;
    }

    int converted = swr_convert(impl_->swr,
        impl_->conv_data, impl_->conv_cap,
        &src_data, src_frames);
    if (converted > 0)
        av_audio_fifo_write(impl_->fifo, (void**)impl_->conv_data, converted);

    // Drain fifo in frame_size chunks.
    const int frame_size = impl_->ctx->frame_size;
    while (av_audio_fifo_size(impl_->fifo) >= frame_size) {
        av_frame_make_writable(impl_->frame);
        impl_->frame->nb_samples = frame_size;
        av_audio_fifo_read(impl_->fifo, (void**)impl_->frame->data, frame_size);
        impl_->frame->pts = impl_->next_pts;
        int ret = avcodec_send_frame(impl_->ctx, impl_->frame);
        if (ret == 0) {
            impl_->next_pts += frame_size;
        }
        drain_audio(impl_);
    }
}

void AudioEncoder::flush()
{
    std::lock_guard lk(impl_->mutex);
    if (!impl_->ctx) return;

    // Flush any leftover samples in the fifo.
    int rem = av_audio_fifo_size(impl_->fifo);
    if (rem > 0) {
        av_frame_make_writable(impl_->frame);
        impl_->frame->nb_samples = rem;
        av_audio_fifo_read(impl_->fifo, (void**)impl_->frame->data, rem);
        impl_->frame->pts = impl_->next_pts;
        int ret = avcodec_send_frame(impl_->ctx, impl_->frame);
        if (ret == 0) {
            impl_->next_pts += rem;
        }
        drain_audio(impl_);
    }

    avcodec_send_frame(impl_->ctx, nullptr);
    drain_audio(impl_);
}

void AudioEncoder::close()
{
    std::lock_guard lk(impl_->mutex);
    if (impl_->ctx) {
        int rem = impl_->fifo ? av_audio_fifo_size(impl_->fifo) : 0;
        if (rem > 0) {
            av_frame_make_writable(impl_->frame);
            impl_->frame->nb_samples = rem;
            av_audio_fifo_read(impl_->fifo, (void**)impl_->frame->data, rem);
            impl_->frame->pts = impl_->next_pts;
            int ret = avcodec_send_frame(impl_->ctx, impl_->frame);
            if (ret == 0) {
                impl_->next_pts += rem;
            }
            drain_audio(impl_);
        }
        avcodec_send_frame(impl_->ctx, nullptr);
        drain_audio(impl_);
    }
    if (impl_->fifo)  { av_audio_fifo_free(impl_->fifo); impl_->fifo  = nullptr; }
    if (impl_->swr)   { swr_free(&impl_->swr);                                   }
    if (impl_->frame) { av_frame_free(&impl_->frame);                            }
    if (impl_->ctx)   { avcodec_free_context(&impl_->ctx);                       }
    impl_->free_conv();
    impl_->next_pts      = 0;
    impl_->swr_src_rate  = 0;
    impl_->swr_src_ch    = 0;
    impl_->swr_src_fmt   = AV_SAMPLE_FMT_NONE;
    impl_->stream_index  = 1;
    impl_->asp           = {};
}

// ── TrackMixer ────────────────────────────────────────────────────────────────
//
// Canonical mix format is interleaved 32-bit float (AV_SAMPLE_FMT_FLT). Each
// source owns a swr context (its native WASAPI format → canonical) plus a FIFO
// of converted samples. A wall-clock-paced thread wakes every kMixIntervalMs,
// computes how many output frames correspond to elapsed real time, sums that
// many frames from every source (zero-filling sources that are behind), clips
// to [-1, 1], and forwards the block to the sink.

namespace {
constexpr int kMixIntervalMs   = 10;   // mix cadence
constexpr int kMaxFifoFrames   = 48000 * 2; // ~2 s cap per source at 48 kHz
}

struct MixSource {
    SwrContext*    swr      = nullptr;
    AVAudioFifo*   fifo     = nullptr;
    uint8_t**      conv     = nullptr; // reusable swr output planes
    int            conv_cap = 0;       // capacity in samples
    int            src_rate = 0;
    int            src_ch   = 0;
    AVSampleFormat src_fmt  = AV_SAMPLE_FMT_NONE;
    float          gain     = 1.0f;  // linear gain applied before summing

    void free_conv()
    {
        if (conv) {
            av_freep(&conv[0]);
            av_freep(&conv);
        }
        conv_cap = 0;
    }
};

struct TrackMixer::Impl {
    std::mutex                          mutex;       // guards sources map + swr/fifo
    std::map<int, std::unique_ptr<MixSource>> sources;
    int                                 next_id   = 0;
    int                                 out_rate  = 48000;
    int                                 out_ch    = 2;
    Sink                                sink;
    std::thread                         thread;
    std::atomic<bool>                   running{false};

    void reset_source_swr(MixSource& s, int rate, int ch, AVSampleFormat fmt)
    {
        if (s.swr) swr_free(&s.swr);
        AVChannelLayout src_chl{}; av_channel_layout_default(&src_chl, ch);
        AVChannelLayout dst_chl{}; av_channel_layout_default(&dst_chl, out_ch);
        swr_alloc_set_opts2(&s.swr,
            &dst_chl, AV_SAMPLE_FMT_FLT, out_rate,
            &src_chl, fmt,               rate,
            0, nullptr);
        av_channel_layout_uninit(&src_chl);
        av_channel_layout_uninit(&dst_chl);
        if (s.swr && swr_init(s.swr) < 0) swr_free(&s.swr);
        s.src_rate = rate; s.src_ch = ch; s.src_fmt = fmt;
    }
};

TrackMixer::TrackMixer()  : impl_(new Impl()) {}
TrackMixer::~TrackMixer() { close(); delete impl_; }

bool TrackMixer::is_open() const { return impl_->running.load(std::memory_order_acquire); }

int TrackMixer::source_count() const
{
    std::lock_guard lk(impl_->mutex);
    return static_cast<int>(impl_->sources.size());
}

bool TrackMixer::open(int out_sample_rate, int out_channels, Sink sink)
{
    if (out_sample_rate <= 0 || out_channels <= 0) return false;
    close();
    impl_->out_rate = out_sample_rate;
    impl_->out_ch   = out_channels;
    impl_->sink     = std::move(sink);
    impl_->running.store(true, std::memory_order_release);

    impl_->thread = std::thread([this] {
        using clock = std::chrono::steady_clock;
        const auto start = clock::now();
        int64_t emitted_frames = 0; // frames already sent since `start`
        const int ch = impl_->out_ch;
        std::vector<float> acc;   // accumulator (interleaved)
        std::vector<float> tmp;   // per-source read buffer (interleaved)

        while (impl_->running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kMixIntervalMs));

            // Clock-locked CFR: the number of frames emitted is a function of the
            // real time elapsed since `start`, not of loop iterations. Tracking a
            // running total against a fixed origin avoids the cumulative drift of
            // truncating per-tick deltas, keeping audio aligned over long sessions.
            int64_t elapsed_us =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    clock::now() - start).count();
            int64_t target = (elapsed_us * impl_->out_rate) / 1'000'000;
            int frames = static_cast<int>(target - emitted_frames);
            if (frames <= 0) continue;
            emitted_frames = target;

            acc.assign(static_cast<size_t>(frames) * ch, 0.0f);
            {
                std::lock_guard lk(impl_->mutex);
                tmp.resize(static_cast<size_t>(frames) * ch);
                for (auto& [id, sp] : impl_->sources) {
                    if (!sp->fifo) continue;
                    int avail = av_audio_fifo_size(sp->fifo);
                    int take  = std::min(frames, avail);
                    if (take <= 0) continue;
                    void* dst[1] = { tmp.data() };
                    int got = av_audio_fifo_read(sp->fifo, dst, take);
                    if (got <= 0) continue;
                    size_t n = static_cast<size_t>(got) * ch;
                    const float g = sp->gain;
                    if (g == 1.0f) {
                        for (size_t i = 0; i < n; ++i) acc[i] += tmp[i];
                    } else {
                        for (size_t i = 0; i < n; ++i) acc[i] += tmp[i] * g;
                    }
                }
            }

            // Clip to [-1, 1] to avoid wrap/overflow on summed sources.
            for (float& v : acc) {
                if (v >  1.0f) v =  1.0f;
                else if (v < -1.0f) v = -1.0f;
            }

            // Always emit, even when no source contributed (acc is then silence).
            // A continuous output keeps every configured track present in the
            // file and the downstream encoder timeline advancing at real time.
            if (impl_->sink) {
                impl_->sink(
                    reinterpret_cast<const uint8_t*>(acc.data()),
                    static_cast<int>(acc.size() * sizeof(float)),
                    impl_->out_rate, ch, 32, /*is_float=*/true);
            }
        }
    });
    return true;
}

int TrackMixer::add_source(float gain)
{
    if (!is_open()) return -1;
    std::lock_guard lk(impl_->mutex);
    int id = impl_->next_id++;
    auto src = std::make_unique<MixSource>();
    src->fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLT, impl_->out_ch, impl_->out_rate);
    if (!src->fifo) return -1;
    src->gain = (gain < 0.0f) ? 0.0f : gain;
    impl_->sources.emplace(id, std::move(src));
    return id;
}

void TrackMixer::set_source_gain(int source_id, float gain)
{
    std::lock_guard lk(impl_->mutex);
    auto it = impl_->sources.find(source_id);
    if (it == impl_->sources.end()) return;
    it->second->gain = (gain < 0.0f) ? 0.0f : gain;
}

void TrackMixer::remove_source(int source_id)
{
    std::lock_guard lk(impl_->mutex);
    auto it = impl_->sources.find(source_id);
    if (it == impl_->sources.end()) return;
    if (it->second->swr)  swr_free(&it->second->swr);
    if (it->second->fifo) av_audio_fifo_free(it->second->fifo);
    it->second->free_conv();
    impl_->sources.erase(it);
}

void TrackMixer::push(int source_id, const uint8_t* data, int bytes,
                      int sample_rate, int channels, int bit_depth, bool is_float)
{
    if (bytes <= 0 || sample_rate <= 0 || channels <= 0 || bit_depth <= 0) return;
    AVSampleFormat src_fmt = wasapi_to_av_fmt(bit_depth, is_float);
    int src_frames = bytes / (bit_depth / 8 * channels);
    if (src_frames <= 0) return;

    std::vector<uint8_t> silence;
    const uint8_t* src_data = data;
    if (!src_data) { silence.assign(static_cast<size_t>(bytes), 0); src_data = silence.data(); }

    std::lock_guard lk(impl_->mutex);
    auto it = impl_->sources.find(source_id);
    if (it == impl_->sources.end()) return;
    MixSource& s = *it->second;

    if (!s.swr || s.src_rate != sample_rate || s.src_ch != channels || s.src_fmt != src_fmt)
        impl_->reset_source_swr(s, sample_rate, channels, src_fmt);
    if (!s.swr) return;

    int max_out = static_cast<int>(av_rescale_rnd(
        swr_get_delay(s.swr, sample_rate) + src_frames,
        impl_->out_rate, sample_rate, AV_ROUND_UP));
    if (max_out <= 0) return;

    if (max_out > s.conv_cap) {
        s.free_conv();
        int conv_linesize = 0;
        if (av_samples_alloc_array_and_samples(
                &s.conv, &conv_linesize,
                impl_->out_ch, max_out, AV_SAMPLE_FMT_FLT, 0) < 0) {
            s.conv = nullptr;
            return;
        }
        s.conv_cap = max_out;
    }

    int converted = swr_convert(s.swr, s.conv, s.conv_cap, &src_data, src_frames);
    if (converted > 0) {
        // Drop oldest data if a source overruns (downstream stalled / paused).
        if (av_audio_fifo_size(s.fifo) + converted > kMaxFifoFrames) {
            int drop = av_audio_fifo_size(s.fifo) + converted - kMaxFifoFrames;
            av_audio_fifo_drain(s.fifo, drop);
        }
        av_audio_fifo_write(s.fifo, reinterpret_cast<void**>(s.conv), converted);
    }
}

void TrackMixer::close()
{
    if (impl_->running.exchange(false, std::memory_order_acq_rel)) {
        if (impl_->thread.joinable()) impl_->thread.join();
    }
    std::lock_guard lk(impl_->mutex);
    for (auto& [id, sp] : impl_->sources) {
        if (sp->swr)  swr_free(&sp->swr);
        if (sp->fifo) av_audio_fifo_free(sp->fifo);
        sp->free_conv();
    }
    impl_->sources.clear();
    impl_->next_id = 0;
    impl_->sink    = nullptr;
}

} // namespace encoding
