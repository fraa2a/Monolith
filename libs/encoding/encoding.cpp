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

#include <cstring>
#include <mutex>

namespace encoding {

// ── probe_video_encoder ───────────────────────────────────────────────────────

static const char* kEncoderCandidates[] = {
    "h264_nvenc", "h264_amf", "h264_qsv", "libx264",
    "hevc_nvenc", "hevc_amf", "hevc_qsv", "libx265",
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
    std::string     enc_name;
    bool            extra_rejected = false;
    int             sws_flags = SWS_BILINEAR;
    int             cfg_fps  = 0;
    VideoStreamParams vsp;
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

bool VideoEncoder::open(Config const& cfg, PacketSink sink)
{
    if (cfg.width <= 0 || cfg.height <= 0 || cfg.fps <= 0)
        return false;
    if (cfg.bitrate <= 0 && cfg.quality <= 0)
        return false;
    if (cfg.quality > 0) {
        int clamped = cfg.quality;
        if (clamped < 10) clamped = 10;
        if (clamped > 30) clamped = 30;
        const_cast<Config&>(cfg).quality = clamped;
    }

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
        impl_->enc_name.clear();
        impl_->vsp      = {};
        impl_->sink     = nullptr;
        impl_->cfg_fps  = 0;
    };

    impl_->sink  = std::move(sink);
    impl_->cfg_w = cfg.width;
    impl_->cfg_h = cfg.height;
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

        ctx->width     = cfg.width;
        ctx->height    = cfg.height;
        ctx->time_base = {1, cfg.fps};
        ctx->framerate = {cfg.fps, 1};
        ctx->gop_size  = cfg.fps * 2;  // keyframe every 2 s
        ctx->pix_fmt   = AV_PIX_FMT_YUV420P;
        ctx->flags    |= AV_CODEC_FLAG_GLOBAL_HEADER; // SPS/PPS in extradata

        AVDictionary* opts = nullptr;

        bool is_hw = (name.find("nvenc") != std::string::npos ||
                      name.find("amf")   != std::string::npos ||
                      name.find("qsv")   != std::string::npos);
        bool is_sw = (name == "libx264" || name == "libx265");

        if (cfg.quality > 0 && (is_hw || is_sw)) {
            // Rate control: CQP for HW, CRF for SW (OBS-style).
            if (is_hw) {
                if (name.find("nvenc") != std::string::npos) {
                    av_dict_set(&opts, "rc",  "constqp", 0);
                    av_dict_set_int(&opts, "qp", cfg.quality, 0);
                } else if (name.find("amf") != std::string::npos) {
                    av_dict_set(&opts, "rc",     "cqp", 0);
                    av_dict_set_int(&opts, "qp_i", cfg.quality, 0);
                    av_dict_set_int(&opts, "qp_p", cfg.quality, 0);
                    av_dict_set_int(&opts, "qp_b", cfg.quality, 0);
                } else if (name.find("qsv") != std::string::npos) {
                    av_dict_set(&opts, "rc", "CQP", 0);
                    av_dict_set_int(&opts, "qp", cfg.quality, 0);
                }
            } else {
                av_dict_set_int(&opts, "crf", cfg.quality, 0);
            }
        } else {
            ctx->bit_rate = cfg.bitrate;
        }

        if (is_sw) {
            av_dict_set(&opts, "preset", "fast", 0);
            av_dict_set(&opts, "tune",   "zerolatency", 0);
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
    impl_->frame->width  = cfg.width;
    impl_->frame->height = cfg.height;
    if (av_frame_get_buffer(impl_->frame, 0) < 0) { cleanup(); return false; }

    // sws ctx is (re-)created lazily in push_bgra for the actual source size.
    impl_->src_w = 0;
    impl_->src_h = 0;

    // Store stream params for replay buffer.
    impl_->vsp.codec   = (ctx->codec_id == AV_CODEC_ID_HEVC)
        ? VideoCodec::H265
        : VideoCodec::H264;
    impl_->vsp.width   = cfg.width;
    impl_->vsp.height  = cfg.height;
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
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return;
    while (avcodec_receive_packet(impl->ctx, pkt) == 0) {
        EncodedPacket ep;
        ep.data.assign(pkt->data, pkt->data + pkt->size);
        ep.pts          = pkt->pts;
        ep.dts          = pkt->dts;
        ep.dts_usec     = av_rescale(pkt->dts, 1000000,
                                     impl->cfg_fps);
        ep.stream_index = 0;
        ep.is_keyframe  = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        ep.tb_num       = 1;
        ep.tb_den       = impl->cfg_fps;
        if (impl->sink) impl->sink(std::move(ep));
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
}

void VideoEncoder::push_bgra(const uint8_t* bgra, int stride, int width, int height)
{
    std::lock_guard lk(impl_->mutex);
    if (!impl_->ctx || !bgra) return;
    if (width <= 0 || height <= 0) return;

    // (Re-)create the scaler when the source size changes (display mode
    // switches, or output resolution differs from capture resolution).
    if (!impl_->sws || width != impl_->src_w || height != impl_->src_h) {
        if (impl_->sws) sws_freeContext(impl_->sws);
        impl_->sws = sws_getContext(
            width, height, AV_PIX_FMT_BGRA,
            impl_->cfg_w, impl_->cfg_h, impl_->ctx->pix_fmt,
            impl_->sws_flags, nullptr, nullptr, nullptr);
        if (!impl_->sws) return;
        impl_->src_w = width;
        impl_->src_h = height;
    }

    av_frame_make_writable(impl_->frame);

    const uint8_t* src_slices[1] = { bgra };
    int            src_stride[1] = { stride };
    sws_scale(impl_->sws,
        src_slices, src_stride, 0, height,
        impl_->frame->data, impl_->frame->linesize);

    impl_->frame->pts = impl_->next_pts;
    int ret = avcodec_send_frame(impl_->ctx, impl_->frame);
    if (ret == 0) {
        impl_->next_pts++;
    }
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
    int             stream_index = 1;
    int             cfg_sample_rate = 0;
    AudioStreamParams asp;
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
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return;
    while (avcodec_receive_packet(impl->ctx, pkt) == 0) {
        EncodedPacket ep;
        ep.data.assign(pkt->data, pkt->data + pkt->size);
        ep.pts          = pkt->pts;
        ep.dts          = pkt->dts;
        ep.dts_usec     = av_rescale(pkt->dts, 1000000,
                                     impl->cfg_sample_rate);
        ep.stream_index = impl->stream_index;
        ep.is_keyframe  = true; // AAC frames are all independently decodable
        ep.tb_num       = 1;
        ep.tb_den       = impl->cfg_sample_rate;
        if (impl->sink) impl->sink(std::move(ep));
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
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

    // Allocate temporary buffer for the converted output.
    int max_out = (int)av_rescale_rnd(
        swr_get_delay(impl_->swr, sample_rate) + src_frames,
        impl_->ctx->sample_rate, sample_rate, AV_ROUND_UP);

    uint8_t** dst_data = nullptr;
    int       dst_linesize;
    if (av_samples_alloc_array_and_samples(
            &dst_data, &dst_linesize,
            impl_->ctx->ch_layout.nb_channels,
            max_out, impl_->ctx->sample_fmt, 0) < 0)
        return;

    int converted = swr_convert(impl_->swr,
        dst_data, max_out,
        &src_data, src_frames);
    if (converted > 0)
        av_audio_fifo_write(impl_->fifo, (void**)dst_data, converted);

    av_freep(&dst_data[0]);
    av_freep(&dst_data);

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
    impl_->next_pts      = 0;
    impl_->swr_src_rate  = 0;
    impl_->swr_src_ch    = 0;
    impl_->swr_src_fmt   = AV_SAMPLE_FMT_NONE;
    impl_->stream_index  = 1;
    impl_->asp           = {};
}

} // namespace encoding
