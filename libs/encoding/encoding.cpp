#include "encoding.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include <cstring>

namespace encoding {

// ── probe_video_encoder ───────────────────────────────────────────────────────

std::string probe_video_encoder(int width, int height)
{
    static const char* candidates[] = {
        "h264_nvenc", "h264_amf", "h264_qsv", "libx264", nullptr
    };

    // Suppress noise during probing.
    const int saved_level = av_log_get_level();
    av_log_set_level(AV_LOG_QUIET);

    std::string result;
    for (int i = 0; candidates[i]; ++i) {
        const AVCodec* codec = avcodec_find_encoder_by_name(candidates[i]);
        if (!codec) continue;

        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (!ctx) continue;

        ctx->width      = (width  + 1) & ~1; // NVENC requires even dimensions
        ctx->height     = (height + 1) & ~1;
        ctx->time_base  = {1, 60};
        ctx->framerate  = {60, 1};
        ctx->pix_fmt    = AV_PIX_FMT_YUV420P;
        ctx->bit_rate   = 4'000'000;
        ctx->gop_size   = 60;

        AVDictionary* opts = nullptr;
        if (strcmp(candidates[i], "libx264") == 0)
            av_dict_set(&opts, "preset", "ultrafast", 0);

        bool ok = (avcodec_open2(ctx, codec, &opts) == 0);
        av_dict_free(&opts);
        avcodec_free_context(&ctx);

        if (ok) { result = candidates[i]; break; }
    }

    av_log_set_level(saved_level);
    return result;
}

// ── VideoEncoder ──────────────────────────────────────────────────────────────

struct VideoEncoder::Impl {
    AVCodecContext* ctx      = nullptr;
    AVFrame*        frame    = nullptr;
    SwsContext*     sws      = nullptr;
    PacketSink      sink;
    int64_t         next_pts = 0;
    int             cfg_w    = 0;
    int             cfg_h    = 0;
    VideoStreamParams vsp;
};

VideoEncoder::VideoEncoder()  : impl_(new Impl()) {}
VideoEncoder::~VideoEncoder() { close(); delete impl_; }

bool VideoEncoder::is_open() const { return impl_->ctx != nullptr; }
VideoStreamParams VideoEncoder::stream_params() const { return impl_->vsp; }

bool VideoEncoder::open(Config const& cfg, PacketSink sink)
{
    close();
    impl_->sink  = std::move(sink);
    impl_->cfg_w = cfg.width;
    impl_->cfg_h = cfg.height;

    std::string enc_name = probe_video_encoder(cfg.width, cfg.height);
    if (enc_name.empty()) return false;

    const AVCodec* codec = avcodec_find_encoder_by_name(enc_name.c_str());
    if (!codec) return false;

    impl_->ctx = avcodec_alloc_context3(codec);
    if (!impl_->ctx) return false;
    AVCodecContext* ctx = impl_->ctx;

    ctx->width     = cfg.width;
    ctx->height    = cfg.height;
    ctx->time_base = {1, cfg.fps};
    ctx->framerate = {cfg.fps, 1};
    ctx->gop_size  = cfg.fps * 2;  // keyframe every 2 s
    ctx->bit_rate  = cfg.bitrate;
    ctx->pix_fmt   = AV_PIX_FMT_YUV420P;
    ctx->flags    |= AV_CODEC_FLAG_GLOBAL_HEADER; // SPS/PPS in extradata

    AVDictionary* opts = nullptr;
    if (enc_name == "libx264") {
        av_dict_set(&opts, "preset", "fast", 0);
        av_dict_set(&opts, "tune",   "zerolatency", 0);
    } else if (enc_name == "h264_nvenc") {
        av_dict_set(&opts, "preset", "p4", 0);
    }

    if (avcodec_open2(ctx, codec, &opts) < 0) {
        av_dict_free(&opts);
        close();
        return false;
    }
    av_dict_free(&opts);

    impl_->frame = av_frame_alloc();
    impl_->frame->format = ctx->pix_fmt;
    impl_->frame->width  = cfg.width;
    impl_->frame->height = cfg.height;
    if (av_frame_get_buffer(impl_->frame, 0) < 0) { close(); return false; }

    impl_->sws = sws_getContext(
        cfg.width, cfg.height, AV_PIX_FMT_BGRA,
        cfg.width, cfg.height, ctx->pix_fmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!impl_->sws) { close(); return false; }

    // Store stream params for replay buffer.
    impl_->vsp.width   = cfg.width;
    impl_->vsp.height  = cfg.height;
    impl_->vsp.fps_num = cfg.fps;
    impl_->vsp.fps_den = 1;
    impl_->vsp.tb_num  = ctx->time_base.num;
    impl_->vsp.tb_den  = ctx->time_base.den;
    if (ctx->extradata_size > 0)
        impl_->vsp.extradata.assign(ctx->extradata,
                                    ctx->extradata + ctx->extradata_size);
    return true;
}

static void drain_video(VideoEncoder::Impl* impl)
{
    AVPacket* pkt = av_packet_alloc();
    while (avcodec_receive_packet(impl->ctx, pkt) == 0) {
        EncodedPacket ep;
        ep.data.assign(pkt->data, pkt->data + pkt->size);
        ep.pts          = pkt->pts;
        ep.dts          = pkt->dts;
        ep.stream_index = 0;
        ep.is_keyframe  = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        ep.tb_num       = impl->ctx->time_base.num;
        ep.tb_den       = impl->ctx->time_base.den;
        if (impl->sink) impl->sink(std::move(ep));
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
}

void VideoEncoder::push_bgra(const uint8_t* bgra, int stride, int width, int height)
{
    if (!impl_->ctx || !bgra) return;
    if (width != impl_->cfg_w || height != impl_->cfg_h) return; // skip size mismatch

    av_frame_make_writable(impl_->frame);

    const uint8_t* src_slices[1] = { bgra };
    int            src_stride[1] = { stride };
    sws_scale(impl_->sws,
        src_slices, src_stride, 0, impl_->cfg_h,
        impl_->frame->data, impl_->frame->linesize);

    impl_->frame->pts = impl_->next_pts++;
    avcodec_send_frame(impl_->ctx, impl_->frame);
    drain_video(impl_);
}

void VideoEncoder::flush()
{
    if (!impl_->ctx) return;
    avcodec_send_frame(impl_->ctx, nullptr);
    drain_video(impl_);
}

void VideoEncoder::close()
{
    flush();
    if (impl_->sws)   { sws_freeContext(impl_->sws);   impl_->sws   = nullptr; }
    if (impl_->frame) { av_frame_free(&impl_->frame);               }
    if (impl_->ctx)   { avcodec_free_context(&impl_->ctx);           }
    impl_->next_pts = 0;
    impl_->vsp      = {};
}

// ── AudioEncoder ──────────────────────────────────────────────────────────────

struct AudioEncoder::Impl {
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
    AudioStreamParams asp;
};

AudioEncoder::AudioEncoder()  : impl_(new Impl()) {}
AudioEncoder::~AudioEncoder() { close(); delete impl_; }

bool AudioEncoder::is_open() const { return impl_->ctx != nullptr; }
AudioStreamParams AudioEncoder::stream_params() const { return impl_->asp; }

bool AudioEncoder::open(Config const& cfg, PacketSink sink)
{
    close();
    impl_->sink = std::move(sink);

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

    if (avcodec_open2(ctx, codec, nullptr) < 0) { close(); return false; }

    impl_->frame = av_frame_alloc();
    impl_->frame->format      = ctx->sample_fmt;
    impl_->frame->sample_rate = ctx->sample_rate;
    av_channel_layout_copy(&impl_->frame->ch_layout, &ctx->ch_layout);
    impl_->frame->nb_samples  = ctx->frame_size; // typically 1024 for AAC
    if (av_frame_get_buffer(impl_->frame, 0) < 0) { close(); return false; }

    impl_->fifo = av_audio_fifo_alloc(ctx->sample_fmt,
                                      ctx->ch_layout.nb_channels,
                                      ctx->frame_size * 4);
    if (!impl_->fifo) { close(); return false; }

    impl_->asp.sample_rate = cfg.sample_rate;
    impl_->asp.channels    = cfg.channels;
    impl_->asp.tb_num      = ctx->time_base.num;
    impl_->asp.tb_den      = ctx->time_base.den;
    if (ctx->extradata_size > 0)
        impl_->asp.extradata.assign(ctx->extradata,
                                    ctx->extradata + ctx->extradata_size);
    return true;
}

static void drain_audio(AudioEncoder::Impl* impl)
{
    AVPacket* pkt = av_packet_alloc();
    while (avcodec_receive_packet(impl->ctx, pkt) == 0) {
        EncodedPacket ep;
        ep.data.assign(pkt->data, pkt->data + pkt->size);
        ep.pts          = pkt->pts;
        ep.dts          = pkt->dts;
        ep.stream_index = 1;
        ep.is_keyframe  = true; // AAC frames are all independently decodable
        ep.tb_num       = impl->ctx->time_base.num;
        ep.tb_den       = impl->ctx->time_base.den;
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
    if (!impl_->ctx || !data || bytes <= 0) return;

    AVSampleFormat src_fmt   = wasapi_to_av_fmt(bit_depth, is_float);
    int            src_frames = bytes / (bit_depth / 8 * channels);
    if (src_frames <= 0) return;

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
        &data, src_frames);
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
        impl_->next_pts  += frame_size;
        avcodec_send_frame(impl_->ctx, impl_->frame);
        drain_audio(impl_);
    }
}

void AudioEncoder::flush()
{
    if (!impl_->ctx) return;

    // Flush any leftover samples in the fifo.
    int rem = av_audio_fifo_size(impl_->fifo);
    if (rem > 0) {
        av_frame_make_writable(impl_->frame);
        impl_->frame->nb_samples = rem;
        av_audio_fifo_read(impl_->fifo, (void**)impl_->frame->data, rem);
        impl_->frame->pts = impl_->next_pts;
        impl_->next_pts  += rem;
        avcodec_send_frame(impl_->ctx, impl_->frame);
        drain_audio(impl_);
    }

    avcodec_send_frame(impl_->ctx, nullptr);
    drain_audio(impl_);
}

void AudioEncoder::close()
{
    flush();
    if (impl_->fifo)  { av_audio_fifo_free(impl_->fifo); impl_->fifo  = nullptr; }
    if (impl_->swr)   { swr_free(&impl_->swr);                                   }
    if (impl_->frame) { av_frame_free(&impl_->frame);                            }
    if (impl_->ctx)   { avcodec_free_context(&impl_->ctx);                       }
    impl_->next_pts      = 0;
    impl_->swr_src_rate  = 0;
    impl_->swr_src_ch    = 0;
    impl_->swr_src_fmt   = AV_SAMPLE_FMT_NONE;
    impl_->asp           = {};
}

} // namespace encoding
