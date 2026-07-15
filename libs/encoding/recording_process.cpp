#include "recording_process.h"
#include "ffmpeg_process.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace encoding {

static std::string rp_wide_to_utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}

static const char* rp_muxer_name(const std::string& container)
{
    return (container == "mp4") ? "mp4" : "matroska";
}

struct RecordingProcess::Impl {
    RecordingProcessConfig cfg;
    FfmpegProcess          ffmpeg;

    struct AudioPipe {
        int          stream_index = 0;
        HANDLE       handle       = INVALID_HANDLE_VALUE;
        std::wstring name;
        std::atomic<bool> connected{false};
        std::thread  connect_thread;
    };
    std::vector<std::unique_ptr<AudioPipe>> audio_pipes;

    std::atomic<bool> running{false};
    std::function<void(const std::string&)> log;

    void log_line(const std::string& s) { if (log) log(s); }

    AudioPipe* pipe_for(int stream_index)
    {
        for (auto& p : audio_pipes)
            if (p->stream_index == stream_index) return p.get();
        return nullptr;
    }
};

RecordingProcess::RecordingProcess()  : impl_(new Impl()) {}
RecordingProcess::~RecordingProcess() { if (impl_->running.load()) stop(); delete impl_; }

static std::vector<std::string> build_recording_args(
    const RecordingProcessConfig&   cfg,
    const std::vector<std::string>& audio_pipe_utf8,
    const std::string&              output_utf8)
{
    std::vector<std::string> a;
    a.push_back("-hide_banner");
    a.push_back("-loglevel"); a.push_back("warning");
    a.push_back("-y");

    // Input 0: raw video from stdin (pacer CFR).
    a.push_back("-f");            a.push_back("rawvideo");
    a.push_back("-pix_fmt");      a.push_back("bgra");
    a.push_back("-video_size");   a.push_back(std::to_string(cfg.width) + "x" + std::to_string(cfg.height));
    a.push_back("-framerate");    a.push_back(std::to_string(cfg.fps));
    a.push_back("-i");            a.push_back("pipe:0");

    // Inputs 1..N: one raw-audio named pipe per track.
    for (size_t i = 0; i < cfg.audio_tracks.size(); ++i) {
        const auto& t = cfg.audio_tracks[i];
        a.push_back("-f");        a.push_back("f32le");
        a.push_back("-ar");       a.push_back(std::to_string(t.sample_rate));
        a.push_back("-ac");       a.push_back(std::to_string(t.channels));
        a.push_back("-i");        a.push_back(audio_pipe_utf8[i]);
    }

    a.push_back("-map"); a.push_back("0:v:0");
    for (size_t i = 0; i < cfg.audio_tracks.size(); ++i) {
        a.push_back("-map"); a.push_back(std::to_string(i + 1) + ":a:0");
    }

    const std::string br = std::to_string(cfg.bitrate_kbps) + "k";
    a.push_back("-c:v");          a.push_back(cfg.video_encoder);
    a.push_back("-b:v");          a.push_back(br);
    a.push_back("-maxrate");      a.push_back(br);
    a.push_back("-bufsize");      a.push_back(br);
    a.push_back("-pix_fmt");      a.push_back("yuv420p");
    a.push_back("-g");            a.push_back(std::to_string(std::max(1, cfg.fps * 2)));
    const bool is_sw = (cfg.video_encoder == "libx264" || cfg.video_encoder == "libx265");
    if (is_sw) {
        a.push_back("-preset");   a.push_back("fast");
        a.push_back("-tune");     a.push_back("zerolatency");
    } else if (cfg.video_encoder.find("nvenc") != std::string::npos) {
        a.push_back("-preset");   a.push_back("p4");
        a.push_back("-rc");       a.push_back("cbr");
    }

    if (!cfg.audio_tracks.empty()) {
        a.push_back("-c:a");      a.push_back("aac");
        a.push_back("-b:a");      a.push_back("192k");
    }

    a.push_back("-f"); a.push_back(rp_muxer_name(cfg.container));
    if (cfg.container == "mp4") {
        // Keep the moov atom writable at stop without a full rewrite.
        a.push_back("-movflags"); a.push_back("+faststart");
    }
    a.push_back(output_utf8);
    return a;
}

bool RecordingProcess::start(const RecordingProcessConfig&           cfg,
                             std::function<void(const std::string&)> log)
{
    if (impl_->running.load()) return false;
    if (cfg.ffmpeg_path.empty() || cfg.output_path.empty() ||
        cfg.width <= 0 || cfg.height <= 0)
        return false;

    impl_->cfg = cfg;
    impl_->log = std::move(log);

    std::vector<std::string> audio_pipe_utf8;
    audio_pipe_utf8.reserve(cfg.audio_tracks.size());
    for (const auto& t : cfg.audio_tracks) {
        auto ap = std::make_unique<Impl::AudioPipe>();
        ap->stream_index = t.stream_index;
        ap->name = L"\\\\.\\pipe\\monolith_recording_audio_" +
                   std::to_wstring(GetCurrentProcessId()) + L"_" +
                   std::to_wstring(t.stream_index);
        ap->handle = CreateNamedPipeW(
            ap->name.c_str(), PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE | PIPE_WAIT,
            1, 1 << 20, 1 << 20, 0, nullptr);
        if (ap->handle == INVALID_HANDLE_VALUE) {
            impl_->log_line("recording_process: failed to create audio pipe");
            for (auto& p : impl_->audio_pipes)
                if (p->handle != INVALID_HANDLE_VALUE) CloseHandle(p->handle);
            impl_->audio_pipes.clear();
            return false;
        }
        audio_pipe_utf8.push_back(rp_wide_to_utf8(ap->name));
        impl_->audio_pipes.push_back(std::move(ap));
    }

    std::vector<std::string> args =
        build_recording_args(cfg, audio_pipe_utf8, rp_wide_to_utf8(cfg.output_path));

    if (!impl_->ffmpeg.start(cfg.ffmpeg_path, args,
                             [this](const std::string& l) { impl_->log_line("ffmpeg: " + l); })) {
        impl_->log_line("recording_process: ffmpeg failed to start");
        for (auto& p : impl_->audio_pipes)
            if (p->handle != INVALID_HANDLE_VALUE) CloseHandle(p->handle);
        impl_->audio_pipes.clear();
        return false;
    }

    for (auto& p : impl_->audio_pipes) {
        Impl::AudioPipe* raw = p.get();
        raw->connect_thread = std::thread([this, raw]() {
            BOOL ok = ConnectNamedPipe(raw->handle, nullptr);
            if (ok || GetLastError() == ERROR_PIPE_CONNECTED)
                raw->connected.store(true);
            else
                impl_->log_line("recording_process: ffmpeg never connected to an audio pipe");
        });
    }

    impl_->running.store(true);
    return true;
}

bool RecordingProcess::is_running() const
{
    return impl_->running.load() && impl_->ffmpeg.is_running();
}

bool RecordingProcess::push_video(const uint8_t* bgra, size_t size)
{
    if (!impl_->running.load() || !bgra) return false;
    return impl_->ffmpeg.write_stdin(bgra, size);
}

bool RecordingProcess::push_audio(int stream_index, const uint8_t* pcm, size_t size)
{
    if (!impl_->running.load() || !pcm) return false;
    Impl::AudioPipe* ap = impl_->pipe_for(stream_index);
    if (!ap) return true;
    if (!ap->connected.load()) return true;
    if (ap->handle == INVALID_HANDLE_VALUE) return false;

    size_t off = 0;
    while (off < size) {
        DWORD written = 0;
        DWORD chunk = (DWORD)std::min<size_t>(size - off, 1u << 20);
        if (!WriteFile(ap->handle, pcm + off, chunk, &written, nullptr) || written == 0)
            return false;
        off += written;
    }
    return true;
}

std::wstring RecordingProcess::stop()
{
    if (!impl_->running.exchange(false)) return {};

    // Close audio pipes first so ffmpeg sees EOF on every input, then close
    // stdin (video) so it finalizes and exits.
    for (auto& p : impl_->audio_pipes) {
        p->connected.store(false);
        if (p->handle != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(p->handle);
            DisconnectNamedPipe(p->handle);
            CloseHandle(p->handle);
            p->handle = INVALID_HANDLE_VALUE;
        }
    }

    int code = impl_->ffmpeg.stop(10000);

    for (auto& p : impl_->audio_pipes)
        if (p->connect_thread.joinable()) p->connect_thread.join();
    impl_->audio_pipes.clear();

    if (code != 0) {
        impl_->log_line("recording_process: ffmpeg exited with code " + std::to_string(code));
        return {};
    }
    return impl_->cfg.output_path;
}

std::wstring RecordingProcess::output_path() const
{
    return impl_->cfg.output_path;
}

} // namespace encoding
