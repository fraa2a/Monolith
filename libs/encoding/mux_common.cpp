#include "mux_common.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
}

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>

namespace encoding::mux {

std::string wcs_to_utf8(const std::wstring& ws)
{
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0,
        ws.c_str(), static_cast<int>(ws.size()),
        nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0,
        ws.c_str(), static_cast<int>(ws.size()),
        s.data(), n, nullptr, nullptr);
    return s;
}

const char* muxer_name(const std::string& container)
{
    return container == "mp4" ? "mp4" : "matroska";
}

const wchar_t* file_extension(const std::string& container)
{
    return container == "mp4" ? L"mp4" : L"mkv";
}

int video_codec_id(VideoCodec codec)
{
    return codec == VideoCodec::H265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
}

static bool copy_extradata(AVCodecParameters* codecpar, const std::vector<uint8_t>& data)
{
    if (data.empty()) return true;
    codecpar->extradata = reinterpret_cast<uint8_t*>(
        av_mallocz(data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!codecpar->extradata) return false;
    memcpy(codecpar->extradata, data.data(), data.size());
    codecpar->extradata_size = static_cast<int>(data.size());
    return true;
}

bool alloc_output(const std::string&                    path_utf,
                  const std::string&                    container,
                  const VideoStreamParams&              vsp,
                  const std::vector<AudioStreamParams>& audio_params,
                  AVFormatContext**                     out_fmt,
                  StreamSet*                            out_streams)
{
    *out_fmt = nullptr;
    *out_streams = StreamSet{};

    AVFormatContext* fmt = nullptr;
    if (avformat_alloc_output_context2(&fmt, nullptr, muxer_name(container),
                                       path_utf.c_str()) < 0)
        return false;

    AVStream* vs = avformat_new_stream(fmt, nullptr);
    if (!vs) { avformat_free_context(fmt); return false; }
    vs->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vs->codecpar->codec_id   = static_cast<AVCodecID>(video_codec_id(vsp.codec));
    vs->codecpar->width      = vsp.width;
    vs->codecpar->height     = vsp.height;
    vs->time_base            = {vsp.tb_num, vsp.tb_den};
    if (!copy_extradata(vs->codecpar, vsp.extradata)) {
        avformat_free_context(fmt);
        return false;
    }

    StreamSet streams;
    streams.video = vs;
    for (const auto& asp : audio_params) {
        if (asp.stream_index < 1 || asp.stream_index > 6) continue;
        AVStream* as = avformat_new_stream(fmt, nullptr);
        if (!as) { avformat_free_context(fmt); return false; }
        as->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
        as->codecpar->codec_id    = AV_CODEC_ID_AAC;
        as->codecpar->sample_rate = asp.sample_rate;
        av_channel_layout_default(&as->codecpar->ch_layout, asp.channels);
        as->time_base = {asp.tb_num, asp.tb_den};
        if (!copy_extradata(as->codecpar, asp.extradata)) {
            avformat_free_context(fmt);
            return false;
        }
        streams.audio[asp.stream_index] = as;
    }

    *out_fmt = fmt;
    *out_streams = streams;
    return true;
}

bool open_file_and_write_header(AVFormatContext* fmt,
                                const std::string& path_utf,
                                const std::string& container)
{
    (void)container; // container-specific muxer options are set in alloc_output

    if (avio_open(&fmt->pb, path_utf.c_str(), AVIO_FLAG_WRITE) < 0)
        return false;

    // NB: no "+faststart" for mp4. faststart relocates the moov atom to the
    // front of the file at av_write_trailer() time, which rewrites the ENTIRE
    // file on stop — for a multi-GB manual recording that is many seconds of
    // disk/CPU on the calling thread and froze the UI/hotkeys. We instead leave
    // the moov at the end (OBS default); local playback and editing are
    // unaffected, and finalizing only writes the index, so stop is near-instant.
    const bool ok = avformat_write_header(fmt, nullptr) >= 0;
    return ok;
}

bool write_packet(AVFormatContext* fmt,
                  AVStream*        dst_stream,
                  const EncodedPacket& ep,
                  int64_t          pts_offset,
                  int64_t          dts_offset)
{
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return false;
    if (av_new_packet(pkt, static_cast<int>(ep.data.size())) < 0) {
        av_packet_free(&pkt);
        return false;
    }

    pkt->stream_index = dst_stream->index;
    memcpy(pkt->data, ep.data.data(), ep.data.size());
    pkt->flags = ep.is_keyframe ? AV_PKT_FLAG_KEY : 0;
    pkt->pts   = ep.pts - pts_offset;
    pkt->dts   = ep.dts - dts_offset;
    if (pkt->dts > pkt->pts) pkt->dts = pkt->pts;

    AVRational src_tb = {ep.tb_num, ep.tb_den};
    av_packet_rescale_ts(pkt, src_tb, dst_stream->time_base);

    bool ok = av_interleaved_write_frame(fmt, pkt) >= 0;
    av_packet_free(&pkt);
    return ok;
}

} // namespace encoding::mux
