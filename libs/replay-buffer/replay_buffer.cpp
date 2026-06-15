#include "replay_buffer.h"

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

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace replay_buffer {

// ── Impl ──────────────────────────────────────────────────────────────────────

struct ReplayBuffer::Impl {
    mutable std::mutex              mutex;
    std::deque<encoding::EncodedPacket> ring;

    // Tracked buffer state (OBS-style).
    size_t                          total_bytes = 0;
    int64_t                         cur_time    = 0;  // dts_usec of oldest pkt
    int                             keyframes   = 0;  // KF count in ring

    Config                          cfg;
    encoding::VideoStreamParams     vsp;
    std::vector<encoding::AudioStreamParams> audio_params;
    bool                            vsp_set = false;

    std::atomic<bool>               saving{false};
    std::thread                     save_thread;
};

// ── ReplayBuffer ──────────────────────────────────────────────────────────────

ReplayBuffer::ReplayBuffer()  : impl_(new Impl()) {}
ReplayBuffer::~ReplayBuffer()
{
    if (impl_->save_thread.joinable())
        impl_->save_thread.join();
    delete impl_;
}

void ReplayBuffer::configure(Config const& cfg)
{
    std::lock_guard lk(impl_->mutex);
    impl_->cfg = cfg;
}

void ReplayBuffer::clear()
{
    std::lock_guard lk(impl_->mutex);
    impl_->ring.clear();
    impl_->total_bytes = 0;
    impl_->cur_time    = 0;
    impl_->keyframes   = 0;
}

void ReplayBuffer::set_video_params(encoding::VideoStreamParams const& p)
{
    std::lock_guard lk(impl_->mutex);
    impl_->vsp     = p;
    impl_->vsp_set = true;
}

void ReplayBuffer::set_audio_params(encoding::AudioStreamParams const& p)
{
    set_audio_params(std::vector<encoding::AudioStreamParams>{ p });
}

void ReplayBuffer::set_audio_params(std::vector<encoding::AudioStreamParams> const& p)
{
    std::lock_guard lk(impl_->mutex);
    impl_->audio_params.clear();
    for (const auto& stream : p) {
        if (stream.stream_index < 1 || stream.stream_index > 6) continue;
        if (stream.tb_den == 0 || stream.sample_rate <= 0 || stream.channels <= 0) continue;
        impl_->audio_params.push_back(stream);
    }
}

// ── OBS-style purge: before push, maintain memory + time caps with ≥2 KF ─────

void ReplayBuffer::push(encoding::EncodedPacket pkt)
{
    std::lock_guard lk(impl_->mutex);

    const size_t max_bytes = static_cast<size_t>(impl_->cfg.memory_cap_mb) * 1024 * 1024;
    const int64_t max_time = static_cast<int64_t>(impl_->cfg.duration_sec) * 1000000;

    // Guard: never purge below 2 keyframes or when ring is too small.
    auto can_purge = [&]() -> bool {
        return impl_->ring.size() >= 2 && impl_->keyframes > 2;
    };

    auto pop_front = [&]() {
        auto& front = impl_->ring.front();
        impl_->total_bytes -= front.data.size();
        if (front.stream_index == 0 && front.is_keyframe)
            impl_->keyframes--;
        impl_->ring.pop_front();
        if (!impl_->ring.empty())
            impl_->cur_time = impl_->ring.front().dts_usec;
        else
            impl_->cur_time = 0;
    };

    // 1) Memory cap — purge until (total_bytes + pkt.size) ≤ max_bytes.
    if (can_purge()) {
        int64_t needed = static_cast<int64_t>(impl_->total_bytes)
                       + static_cast<int64_t>(pkt.data.size())
                       - static_cast<int64_t>(max_bytes);
        while (needed > 0 && can_purge()) {
            needed -= static_cast<int64_t>(impl_->ring.front().data.size());
            pop_front();
        }
    }

    // 2) Time cap — purge until (pkt.dts_usec - cur_time) ≤ max_time.
    if (can_purge()) {
        while (can_purge()) {
            int64_t span = pkt.dts_usec - impl_->cur_time;
            if (span <= max_time) break;
            pop_front();
        }
    }

    // 3) Push the new packet.
    if (pkt.stream_index == 0 && pkt.is_keyframe)
        impl_->keyframes++;
    if (impl_->ring.empty())
        impl_->cur_time = pkt.dts_usec;
    impl_->total_bytes += pkt.data.size();
    impl_->ring.push_back(std::move(pkt));
}

size_t ReplayBuffer::packet_count() const
{
    std::lock_guard lk(impl_->mutex);
    return impl_->ring.size();
}

size_t ReplayBuffer::memory_bytes() const
{
    std::lock_guard lk(impl_->mutex);
    return impl_->total_bytes;
}

// ── Clip save internals ───────────────────────────────────────────────────────

// OBS-style: walk forward from ring start, track the last video keyframe
// seen, stop when dts_usec >= cutoff (newest - duration_sec), and return
// that keyframe index.  If no keyframe exists before cutoff, return the
// first keyframe in the ring (or ring.size() if the ring is empty).
static size_t find_clip_start(
    const std::deque<encoding::EncodedPacket>& ring,
    int duration_sec)
{
    if (ring.empty()) return ring.size();

    int64_t cutoff = ring.back().dts_usec
                   - static_cast<int64_t>(duration_sec) * 1000000;

    size_t best = ring.size(); // last KF encountered
    for (size_t i = 0; i < ring.size(); ++i) {
        const auto& p = ring[i];
        if (p.stream_index == 0 && p.is_keyframe)
            best = i;
        if (p.dts_usec >= cutoff)
            break;
    }

    // Fallback: no KF at or before cutoff → use first KF in ring.
    if (best == ring.size()) {
        for (size_t i = 0; i < ring.size(); ++i) {
            if (ring[i].stream_index == 0 && ring[i].is_keyframe)
                return i;
        }
    }

    return best;
}

static std::string wcs_to_utf8(const std::wstring& ws)
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

static std::wstring generate_clip_path(const std::wstring& dir, int duration_sec,
                                       const std::string& container)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    const wchar_t* ext = (container == "mp4") ? L"mp4" : L"mkv";
    wchar_t path[MAX_PATH];
    swprintf_s(path, MAX_PATH,
        L"%s\\%04d%02d%02d_%02d%02d%02d_%ds_clip.%s",
        dir.c_str(),
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond,
        duration_sec,
        ext);
    return path;
}

static AVCodecID video_codec_id(encoding::VideoCodec codec)
{
    return codec == encoding::VideoCodec::H265
        ? AV_CODEC_ID_HEVC
        : AV_CODEC_ID_H264;
}

static std::wstring write_clip(
    std::vector<encoding::EncodedPacket>         pkts,
    const encoding::VideoStreamParams&           vsp,
    const std::vector<encoding::AudioStreamParams>& audio_params,
    const std::wstring&                          out_dir,
    int                                          duration_sec,
    const std::string&                           container)
{
    if (pkts.empty()) return {};
    if (!vsp.tb_den) return {};

    // Ensure the output directory exists.
    CreateDirectoryW(out_dir.c_str(), nullptr);

    const bool   is_mp4   = (container == "mp4");
    std::wstring path     = generate_clip_path(out_dir, duration_sec, container);
    std::string  path_utf = wcs_to_utf8(path);
    if (path_utf.empty()) return {};

    // ── Reorder packets by DTS (OBS-style: B-frame safety) ──────────────────
    std::stable_sort(pkts.begin(), pkts.end(),
        [](const encoding::EncodedPacket& a, const encoding::EncodedPacket& b) {
            return a.dts_usec < b.dts_usec;
        });

    // ── Open output context ─────────────────────────────────────────────────
    AVFormatContext* fmt = nullptr;
    if (avformat_alloc_output_context2(&fmt, nullptr,
                                       is_mp4 ? "mp4" : "matroska",
                                       path_utf.c_str()) < 0)
        return {};

    // ── Video stream ─────────────────────────────────────────────────────────
    AVStream* vs = avformat_new_stream(fmt, nullptr);
    if (!vs) { avformat_free_context(fmt); return {}; }
    vs->codecpar->codec_type  = AVMEDIA_TYPE_VIDEO;
    vs->codecpar->codec_id    = video_codec_id(vsp.codec);
    vs->codecpar->width       = vsp.width;
    vs->codecpar->height      = vsp.height;
    vs->time_base             = {vsp.tb_num, vsp.tb_den};
    if (!vsp.extradata.empty()) {
        vs->codecpar->extradata = reinterpret_cast<uint8_t*>(
            av_mallocz(vsp.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (vs->codecpar->extradata) {
            memcpy(vs->codecpar->extradata,
                   vsp.extradata.data(), vsp.extradata.size());
            vs->codecpar->extradata_size = static_cast<int>(vsp.extradata.size());
        }
    }

    // ── Audio stream ─────────────────────────────────────────────────────────
    std::array<AVStream*, 7> audio_streams{};
    for (const auto& asp : audio_params) {
        if (asp.stream_index < 1 || asp.stream_index > 6) continue;
        AVStream* as = avformat_new_stream(fmt, nullptr);
        if (!as) { avformat_free_context(fmt); return {}; }
        as->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
        as->codecpar->codec_id    = AV_CODEC_ID_AAC;
        as->codecpar->sample_rate = asp.sample_rate;
        av_channel_layout_default(&as->codecpar->ch_layout, asp.channels);
        as->time_base             = {asp.tb_num, asp.tb_den};
        if (!asp.extradata.empty()) {
            as->codecpar->extradata = reinterpret_cast<uint8_t*>(
                av_mallocz(asp.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            if (as->codecpar->extradata) {
                memcpy(as->codecpar->extradata,
                       asp.extradata.data(), asp.extradata.size());
                as->codecpar->extradata_size = static_cast<int>(asp.extradata.size());
            }
        }
        audio_streams[asp.stream_index] = as;
    }

    // ── Open file + write header ──────────────────────────────────────────────
    if (avio_open(&fmt->pb, path_utf.c_str(), AVIO_FLAG_WRITE) < 0) {
        avformat_free_context(fmt);
        return {};
    }
    AVDictionary* mux_opts = nullptr;
    if (is_mp4)
        av_dict_set(&mux_opts, "movflags", "+faststart", 0);
    int header_err = avformat_write_header(fmt, &mux_opts);
    av_dict_free(&mux_opts);
    if (header_err < 0) {
        avio_closep(&fmt->pb);
        avformat_free_context(fmt);
        return {};
    }

    // ── Compute PTS/DTS offsets so the clip starts at 0 ──────────────────────
    int64_t v_pts_offset = AV_NOPTS_VALUE;
    int64_t v_dts_offset = AV_NOPTS_VALUE;
    std::array<int64_t, 7> a_pts_offsets;
    std::array<int64_t, 7> a_dts_offsets;
    a_pts_offsets.fill(AV_NOPTS_VALUE);
    a_dts_offsets.fill(AV_NOPTS_VALUE);
    for (const auto& ep : pkts) {
        if (ep.stream_index == 0) {
            if (v_pts_offset == AV_NOPTS_VALUE) v_pts_offset = ep.pts;
            if (v_dts_offset == AV_NOPTS_VALUE) v_dts_offset = ep.dts;
        } else if (ep.stream_index >= 1 && ep.stream_index <= 6 &&
                   audio_streams[ep.stream_index]) {
            if (a_pts_offsets[ep.stream_index] == AV_NOPTS_VALUE)
                a_pts_offsets[ep.stream_index] = ep.pts;
            if (a_dts_offsets[ep.stream_index] == AV_NOPTS_VALUE)
                a_dts_offsets[ep.stream_index] = ep.dts;
        }
    }
    if (v_pts_offset == AV_NOPTS_VALUE) v_pts_offset = 0;
    if (v_dts_offset == AV_NOPTS_VALUE) v_dts_offset = 0;
    for (auto& off : a_pts_offsets) { if (off == AV_NOPTS_VALUE) off = 0; }
    for (auto& off : a_dts_offsets) { if (off == AV_NOPTS_VALUE) off = 0; }

    // ── Write packets (already in DTS order) ─────────────────────────────────
    for (const auto& ep : pkts) {
        AVStream* dst_stream = nullptr;
        int64_t pts_off = 0, dts_off = 0;
        if (ep.stream_index == 0) {
            dst_stream = vs;
            pts_off = v_pts_offset;
            dts_off = v_dts_offset;
        } else if (ep.stream_index >= 1 && ep.stream_index <= 6) {
            dst_stream = audio_streams[ep.stream_index];
            pts_off = a_pts_offsets[ep.stream_index];
            dts_off = a_dts_offsets[ep.stream_index];
        }
        if (!dst_stream) continue;

        AVPacket* pkt = av_packet_alloc();
        if (!pkt) continue;
        if (av_new_packet(pkt, static_cast<int>(ep.data.size())) < 0) {
            av_packet_free(&pkt);
            continue;
        }

        pkt->stream_index = dst_stream->index;
        memcpy(pkt->data, ep.data.data(), ep.data.size());
        pkt->flags        = ep.is_keyframe ? AV_PKT_FLAG_KEY : 0;

        pkt->pts = ep.pts - pts_off;
        pkt->dts = ep.dts - dts_off;

        // Rescale to stream timebase (packet and stream use the same,
        // but this guards against future divergence).
        AVRational src_tb = {ep.tb_num, ep.tb_den};
        AVRational dst_tb = dst_stream->time_base;
        av_packet_rescale_ts(pkt, src_tb, dst_tb);

        av_interleaved_write_frame(fmt, pkt);
        av_packet_free(&pkt);
    }

    av_write_trailer(fmt);
    avio_closep(&fmt->pb);
    avformat_free_context(fmt);
    return path;
}

// ── save_clip ─────────────────────────────────────────────────────────────────

void ReplayBuffer::save_clip(std::function<void(std::wstring)> cb)
{
    // Drop concurrent save requests — don't queue.
    bool expected = false;
    if (!impl_->saving.compare_exchange_strong(expected, true))
        return;

    // Snapshot the ring buffer and stream params under the lock.
    std::vector<encoding::EncodedPacket> snapshot;
    Config                               cfg;
    encoding::VideoStreamParams          vsp;
    std::vector<encoding::AudioStreamParams> audio_params;
    bool vsp_set;
    {
        std::lock_guard lk(impl_->mutex);
        cfg     = impl_->cfg;
        vsp     = impl_->vsp;
        audio_params = impl_->audio_params;
        vsp_set = impl_->vsp_set;

        size_t start = find_clip_start(impl_->ring, impl_->cfg.duration_sec);
        if (start < impl_->ring.size()) {
            snapshot.reserve(impl_->ring.size() - start);
            for (size_t i = start; i < impl_->ring.size(); ++i)
                snapshot.push_back(impl_->ring[i]);
        }
    }

    if (impl_->save_thread.joinable())
        impl_->save_thread.join();

    impl_->save_thread = std::thread(
        [this, snapshot = std::move(snapshot), cfg, vsp,
         audio_params = std::move(audio_params), vsp_set,
         cb = std::move(cb)]() mutable
        {
            std::wstring result;
            if (!snapshot.empty() && vsp_set) {
                result = write_clip(std::move(snapshot), vsp, audio_params,
                                    cfg.output_dir, cfg.duration_sec,
                                    cfg.container);
            }
            impl_->saving.store(false);
            if (cb) cb(result);
        });
}

} // namespace replay_buffer
