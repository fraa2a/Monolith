#include <encoding/trim.h>
#include <recording/recording.h>
#include <disk-segments/disk_segments.h>
#include <replay-buffer/packet_ring.h>
#include <replay-buffer/replay_buffer.h>
#include "../../app/recorder/src/replay_disk_settings.h"
#include <filesystem>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <future>
#include <condition_variable>
#include <cstring>
extern "C" {
#include <libavformat/avformat.h>
}
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
using namespace std::chrono_literals;
namespace fs = std::filesystem;
std::mutex save_gate_mutex;
std::condition_variable save_gate;
bool block_save = false, save_entered = false;
bool fail_write = false;
extern "C" int __real_av_interleaved_write_frame(AVFormatContext*, AVPacket*);
extern "C" int __wrap_av_interleaved_write_frame(AVFormatContext* fmt, AVPacket* packet) {
    if (fail_write) return AVERROR(EIO);
    return __real_av_interleaved_write_frame(fmt, packet);
}
extern "C" int __real_av_write_frame(AVFormatContext*, AVPacket*);
extern "C" int __wrap_av_write_frame(AVFormatContext* fmt, AVPacket* packet) {
    {
        std::unique_lock lk(save_gate_mutex);
        if (block_save) { save_entered = true; save_gate.notify_all(); save_gate.wait(lk, [] { return !block_save; }); }
    }
    if (fail_write) return AVERROR(EIO);
    return __real_av_write_frame(fmt, packet);
}
void command(const std::string& cmd) { CHECK(std::system(cmd.c_str()) == 0); }
void decode(const fs::path& path) { command("ffmpeg -v error -xerror -i \"" + path.string() + "\" -map 0 -fps_mode passthrough -enc_time_base:v demux -f null -"); }
struct Media {
    encoding::VideoStreamParams video{};
    std::vector<encoding::AudioStreamParams> audio;
    std::vector<encoding::EncodedPacket> packets;
};
Media load(const fs::path& path) {
    AVFormatContext* fmt = nullptr; CHECK(avformat_open_input(&fmt, path.string().c_str(), nullptr, nullptr) == 0);
    CHECK(avformat_find_stream_info(fmt, nullptr) >= 0);
    Media media;
    auto* vs = fmt->streams[0]; auto* v = vs->codecpar;
    media.video = {encoding::VideoCodec::H264, v->width, v->height, vs->avg_frame_rate.num, vs->avg_frame_rate.den,
        vs->time_base.num, vs->time_base.den, {v->extradata, v->extradata + v->extradata_size}};
    for (unsigned i = 1; i < fmt->nb_streams; ++i) {
        auto* s = fmt->streams[i]; auto* a = s->codecpar;
        media.audio.push_back({static_cast<int>(i), a->sample_rate, a->ch_layout.nb_channels, s->time_base.num, s->time_base.den,
            {a->extradata, a->extradata + a->extradata_size}});
    }
    AVPacket* p = av_packet_alloc();
    while (av_read_frame(fmt, p) >= 0) {
        if (p->dts != AV_NOPTS_VALUE) {
            encoding::EncodedPacket ep;
            auto bytes = std::make_shared<encoding::EncodedBytes>(); bytes->size = static_cast<size_t>(p->size);
            bytes->data = std::make_unique<uint8_t[]>(bytes->size); std::memcpy(bytes->data.get(), p->data, bytes->size);
            ep.bytes = bytes; ep.pts = p->pts; ep.dts = p->dts; ep.stream_index = p->stream_index;
            ep.is_keyframe = (p->flags & AV_PKT_FLAG_KEY) != 0;
            const auto tb = fmt->streams[p->stream_index]->time_base;
            ep.tb_num = tb.num; ep.tb_den = tb.den; ep.dts_usec = av_rescale_q(ep.dts, tb, AV_TIME_BASE_Q);
            media.packets.push_back(ep);
        }
        av_packet_unref(p);
    }
    av_packet_free(&p); avformat_close_input(&fmt); return media;
}
encoding::EncodedPacket shifted(encoding::EncodedPacket p, int seconds) {
    const auto delta = av_rescale_q(seconds, AVRational{1,1}, AVRational{p.tb_num,p.tb_den});
    p.pts += delta; p.dts += delta; p.dts_usec += static_cast<int64_t>(seconds) * 1000000; return p;
}
size_t file_count(const fs::path& dir) {
    size_t n = 0;
    if (fs::exists(dir)) for (const auto& p : fs::recursive_directory_iterator(dir)) if (p.is_regular_file()) ++n;
    return n;
}
int main(int argc, char** argv) {
    CHECK(argc == 2); av_log_set_level(AV_LOG_ERROR);
    fs::path dir(argv[1]); fs::remove_all(dir); fs::create_directories(dir);
    auto fixture = dir / "input.mp4";
    command("ffmpeg -v error -y -f lavfi -i testsrc2=size=160x90:rate=30000/1001:duration=9 -f lavfi -i sine=frequency=440:sample_rate=48000:duration=9 -itsoffset 0.15 -f lavfi -i sine=frequency=880:sample_rate=44100:duration=8.8 -map 0:v -map 1:a -map 2:a -c:v libx264 -threads 2 -bf 3 -g 60 -sc_threshold 0 -c:a aac -video_track_timescale 90000 \"" + fixture.string() + "\"");
    command("ffmpeg -v error -y -i \"" + fixture.string() + "\" -map 0 -c copy \"" + (dir / "input.mkv").string() + "\"");
    command("ffmpeg -v error -y -f lavfi -i testsrc2=size=160x90:rate=60000/1001:duration=7 -vf \"select='if(lt(t,3),not(mod(n,2)),not(mod(n,3)))'\" -fps_mode vfr -c:v libx264 -threads 2 -bf 3 -g 48 -sc_threshold 0 -video_track_timescale 120000 \"" + (dir / "vfr.mp4").string() + "\"");
    command("ffmpeg -v error -y -i \"" + (dir/"vfr.mp4").string() + "\" -map 0 -c copy \"" + (dir/"vfr.mkv").string() + "\"");
    std::ofstream report(dir / "results.tsv"); report << std::setprecision(12);
    for (const auto& input : {"input.mp4", "input.mkv", "vfr.mp4", "vfr.mkv"}) {
        for (bool reencode : {false, true}) for (const auto& ext : {"mp4", "mkv"}) {
            std::string error; encoding::TrimResult result;
            auto out = dir / (std::string(input) + (reencode ? "-reencode." : "-lossless.") + ext);
            bool ok = reencode ? encoding::trim_clip_reencode((dir/input).wstring(), 2.7, 5.2, out.wstring(), &error, &result) :
                                 encoding::trim_clip_lossless((dir/input).wstring(), 2.7, 5.2, out.wstring(), &error, &result);
            if (!ok) throw std::runtime_error(out.string() + ": " + error);
            CHECK(result.duration_seconds > 0 && result.duration_seconds < 5);
            if (reencode) CHECK(result.start_seconds >= 2.7 && result.start_seconds < 2.76);
            else CHECK(result.start_seconds <= 2.701);
            CHECK(result.retime_bookmark(result.start_seconds) == 0.0);
            CHECK(!result.retime_bookmark(result.start_seconds - 0.01));
            CHECK(!result.retime_bookmark(result.start_seconds + result.duration_seconds + 0.01));
            if (!reencode) CHECK(result.retime_bookmark(2.4).has_value());
            else CHECK(!result.retime_bookmark(2.4));
            decode(out);
            report << input << '\t' << out.filename().string() << '\t' << result.start_seconds << '\t' << result.duration_seconds << '\n';
        }
    }
    // Mixed timebase segments, non-key window; original global offsets, not
    // requested-length accumulation, determine the second segment position.
    encoding::TrimResult cut;
    std::string error;
    auto concat = dir / "concat.mp4";
    CHECK(encoding::concat_clip_segments({{fixture.wstring(),0,9,0},{(dir/"input.mkv").wstring(),9,18,0}},
        2.7, 13.2, concat.wstring(), &error, &cut));
    decode(concat);
    CHECK(cut.start_seconds < 2.71 && cut.duration_seconds > 10);
    CHECK(!encoding::trim_clip_lossless(fixture.wstring(), 20, 21, (dir/"invalid.mp4").wstring(), &error));
    CHECK(!encoding::trim_clip_reencode(fixture.wstring(), 0, std::numeric_limits<double>::infinity(), (dir/"invalid.mp4").wstring(), &error));
    std::atomic<bool> cancel{true};
    CHECK(!encoding::concat_clip_segments({{fixture.wstring(),0,9,0}}, 0, 5, (dir/"cancel.mp4").wstring(), &error, nullptr, &cancel));

    command("ffmpeg -v error -y -i \"" + fixture.string() + "\" -map 0 -c:v mpeg4 -c:a copy \"" + (dir/"mpeg4.mp4").string() + "\"");
    const auto original_size = fs::file_size(dir/"mpeg4.mp4");
    CHECK(!encoding::trim_clip_reencode((dir/"mpeg4.mp4").wstring(), 1, 3, (dir/"unsupported.mp4").wstring(), &error));
    CHECK(error.find("same-codec") != std::string::npos);
    CHECK(fs::file_size(dir/"mpeg4.mp4") == original_size);

    fs::create_directory(dir/"unwritable.mp4");
    CHECK(!encoding::trim_clip_lossless(fixture.wstring(), 1, 3, (dir/"unwritable.mp4").wstring(), &error));
    CHECK(fs::file_size(dir/"mpeg4.mp4") == original_size);

    auto media = load(fixture);
    // R2: deliberately delay audio from an earlier time behind newer video.
    replay_buffer::detail::PacketRing ring;
    ring.configure_limits(8, 30);
    std::vector<encoding::EncodedPacket> delayed;
    auto first_video = std::find_if(media.packets.begin(), media.packets.end(), [](const auto& p) { return p.stream_index == 0 && p.is_keyframe; });
    CHECK(first_video != media.packets.end());
    ring.push_packet(*first_video);
    // Future audio before intermediate video keys used to terminate the
    // arrival-order scan at key zero instead of selecting the later key.
    for (const auto& p : media.packets) if (p.stream_index > 0) { delayed.push_back(p); ring.push_packet(p); }
    for (auto it = first_video + 1; it != media.packets.end(); ++it) if (it->stream_index == 0) ring.push_packet(*it);
    auto snapshot = ring.snapshot(3);
    CHECK(!snapshot.empty() && snapshot.front().stream_index == 0 && snapshot.front().is_keyframe);
    CHECK(snapshot.front().dts_usec > 3000000 && snapshot.front().dts_usec < 6000000);
    int64_t maximum = INT64_MIN, minimum = INT64_MAX;
    for (const auto& p : media.packets) { maximum = std::max(maximum, p.dts_usec); minimum = std::min(minimum, p.dts_usec); }
    CHECK(ring.newest_dts() == maximum && ring.oldest_dts() == minimum);
    for (const auto& p : snapshot) CHECK(p.dts_usec >= snapshot.front().dts_usec);
    replay_buffer::ReplayBuffer replay;
    replay_buffer::ReplayBuffer::Config rconfig; rconfig.duration_sec = 30; rconfig.output_dir = (dir/"ram").wstring();
    replay.configure(rconfig); replay.set_video_params(media.video); replay.set_audio_params(media.audio);
    for (const auto& p : media.packets) if (!p.stream_index) replay.push(p);
    for (const auto& p : delayed) replay.push(p);
    std::promise<std::wstring> ram_saved; replay.save_clip([&](auto p) { ram_saved.set_value(p); });
    auto ram = ram_saved.get_future().get(); CHECK(!ram.empty()); decode(ram);

    replay.save_clip([](auto) { throw std::runtime_error("completion failed"); });
    for (int i = 0; i < 500 && replay.stats().saving; ++i) std::this_thread::sleep_for(10ms);
    CHECK(!replay.stats().saving);
    std::promise<std::wstring> recovered_save;
    replay.save_clip([&](auto p) { recovered_save.set_value(p); });
    CHECK(!recovered_save.get_future().get().empty());
    recording::ManualRecorder recorder;
    recorder.set_video_params(media.video); recorder.set_audio_params(media.audio);
    CHECK(recorder.start((dir/"recordings"/"nested").wstring(), "mp4"));
    const auto first_path = recorder.current_path();
    for (const auto& p : media.packets) recorder.push(p);
    std::wstring recorded;
    CHECK(recorder.stop(&recorded) && !recorded.empty()); decode(recorded);
    std::ofstream(dir/"manual-path.txt") << fs::path(recorded).string();
    CHECK(recorder.start((dir/"recordings"/"nested").wstring(), "mp4"));
    CHECK(recorder.current_path() != first_path); CHECK(recorder.stop());
    CHECK(recorder.start((dir/"failed-recordings").wstring(), "mp4"));
    fail_write = true;
    recorder.push(*first_video);
    fail_write = false;
    recorded = L"stale";
    CHECK(recorder.state() == recording::RecordingState::Failed);
    CHECK(!recorder.error().empty());
    CHECK(!recorder.stop(&recorded) && recorded.empty());
    recorder.set_audio_params(std::vector<encoding::AudioStreamParams>{});
    CHECK(recorder.start((dir/"late-track").wstring(), "mp4"));
    recorder.push(*first_video); recorder.set_audio_params(media.audio);
    for (const auto& p : media.packets) if (&p != &*first_video) recorder.push(p);
    CHECK(recorder.state() == recording::RecordingState::Recording);
    CHECK(recorder.stop(&recorded) && !recorded.empty()); decode(recorded);
    CHECK(recorder.start((dir/"partial-recordings").wstring(), "mp4"));
    for (const auto& p : media.packets) recorder.push(p);
    recorder.fail("Capture target closed");
    CHECK(!recorder.stop(&recorded) && !recorded.empty()); decode(recorded);
    CHECK(!recorder.error().empty());
    const std::wstring long_folder(300, L'a');
    CHECK(recorder.start(long_folder, "mkv"));
    CHECK(recorder.current_path().size() > 300); CHECK(recorder.stop());

    // Continuous encoder clock: unlike the pressure fixture below, never loop
    // independently encoded AAC priming/tail packets over a reset boundary.
    const auto continuous_path = dir/"continuous.mp4";
    command("ffmpeg -v error -y -f lavfi -i testsrc2=size=160x90:rate=60000/1001:duration=21 -f lavfi -i sine=frequency=440:sample_rate=48000:duration=21 -itsoffset 0.15 -f lavfi -i sine=frequency=880:sample_rate=44100:duration=20.8 -map 0:v -map 1:a -map 2:a -vf \"select='if(lt(t,10),not(mod(n,2)),not(mod(n,3)))'\" -fps_mode vfr -c:v libx264 -threads 2 -bf 3 -g 48 -sc_threshold 0 -c:a aac -video_track_timescale 120000 \"" + continuous_path.string() + "\"");
    const auto continuous = load(continuous_path);
    for (bool mid_gop_save : {false, true}) {
        disk_segments::DiskSegmentBuffer disk;
        disk.configure({30, (dir/"continuous-segments").wstring(), "mkv", 512});
        disk.set_video_params(continuous.video); disk.set_audio_params(continuous.audio);
        bool mid_saved = false;
        for (const auto& packet : continuous.packets) {
            if (mid_gop_save && !mid_saved && packet.dts_usec >= 10700000) {
                std::promise<std::wstring> first_saved;
                CHECK(disk.save_clip((dir/"continuous-mid").wstring(), [&](auto path) { first_saved.set_value(path); }));
                const auto first_path = first_saved.get_future().get();
                CHECK(!first_path.empty()); decode(first_path);
                while (disk.stats().saving) std::this_thread::sleep_for(1ms);
                // Start a new retained window: post-save admission intentionally
                // waits for the next key; no continuity across that gap claimed.
                disk.clear();
                mid_saved = true;
            }
            disk.push(shifted(packet, 1));
        }
        CHECK(disk.stats().segment_count >= 2);
        std::promise<std::wstring> saved;
        CHECK(disk.save_clip((dir/"continuous-output").wstring(), [&](auto path) { saved.set_value(path); }));
        const auto path = saved.get_future().get();
        CHECK(!path.empty()); decode(path);
        const auto output = load(path);
        CHECK(output.audio.size() == continuous.audio.size());
        std::optional<int64_t> common_audio_shift;
        for (int stream : {1, 2}) {
            std::vector<const encoding::EncodedPacket*> input_audio, output_audio;
            for (const auto& packet : continuous.packets) if (packet.stream_index == stream) input_audio.push_back(&packet);
            for (const auto& packet : output.packets) if (packet.stream_index == stream) output_audio.push_back(&packet);
            CHECK(output_audio.size() > 300);
            auto same_payload = [](const auto* a, const auto* b) {
                return a->size() == b->size() && std::memcmp(a->bytes->data.get(), b->bytes->data.get(), a->size()) == 0;
            };
            const auto first = std::find_if(input_audio.begin(), input_audio.end(), [&](const auto* packet) {
                return same_payload(packet, output_audio.front());
            });
            CHECK(first != input_audio.end());
            const auto offset = static_cast<size_t>(first - input_audio.begin());
            CHECK(offset + output_audio.size() <= input_audio.size());
            CHECK(input_audio.size() - offset - output_audio.size() <= 5); // outer packet-boundary tail only
            const auto audio_shift = output_audio.front()->dts_usec - input_audio[offset]->dts_usec;
            if (common_audio_shift) CHECK(std::abs(audio_shift - *common_audio_shift) <= 2000);
            else common_audio_shift = audio_shift;
            for (size_t i = 0; i < output_audio.size(); ++i) {
                // Every saved audio packet must be the next source packet:
                // decode success alone cannot detect loss at segment joins.
                if (!same_payload(input_audio[offset + i], output_audio[i])) {
                    std::cerr << "continuous audio loss: stream=" << stream << " packet=" << i
                              << " expected_source_dts_us=" << input_audio[offset+i]->dts_usec
                              << " output_dts_us=" << output_audio[i]->dts_usec << '\n';
                }
                CHECK(same_payload(input_audio[offset + i], output_audio[i]));
                if (i == 0) continue;
                const auto actual = output_audio[i]->dts_usec - output_audio[i-1]->dts_usec;
                const auto expected = input_audio[offset+i]->dts_usec - input_audio[offset+i-1]->dts_usec;
                CHECK(actual > 0);
                CHECK(std::abs(actual - expected) <= 2000); // Matroska millisecond rounding.
            }
        }
        disk.clear();
        std::cout << "PASS continuous-clock disk save: both audio payload sequences contiguous, DTS strictly increasing, interval error <=2ms\n";
    }

    auto segments_dir = dir / "segments";
    {
        disk_segments::DiskSegmentBuffer disk;
        struct GateRelease { ~GateRelease() { std::lock_guard lk(save_gate_mutex); block_save = false; save_gate.notify_all(); } } gate_release;
        disk_segments::DiskSegmentBuffer::Config config{8, segments_dir.wstring(), "mkv", 2048};
        disk.configure(config); disk.set_video_params(media.video); disk.set_audio_params(media.audio);
        size_t index = 0;
        for (; index < media.packets.size() && media.packets[index].dts_usec < 2600000; ++index) disk.push(shifted(media.packets[index], 1));
        const auto before = disk.stats().segment_count;
        { std::lock_guard lk(save_gate_mutex); block_save = true; save_entered = false; }
        std::promise<std::wstring> saved;
        CHECK(disk.save_clip((dir/"disk").wstring(), [&](auto path) { saved.set_value(path); }));
        {
            std::unique_lock lk(save_gate_mutex);
            CHECK(save_gate.wait_for(lk, 5s, [] { return save_entered; }));
        }
        CHECK(disk.stats().pinned_bytes > 0);
        // Save mid-GOP: audio/inter video alone cannot open a new segment.
        while (index < media.packets.size() && !(media.packets[index].stream_index == 0 && media.packets[index].is_keyframe))
            disk.push(shifted(media.packets[index++], 1));
        CHECK(disk.stats().segment_count == before);
        for (; index < media.packets.size(); ++index) disk.push(shifted(media.packets[index], 1));
        for (int loop = 1; loop < 12; ++loop) for (const auto& p : media.packets) disk.push(shifted(p, 1 + loop*9));
        CHECK(file_count(segments_dir) <= 5); // pinned snapshot + current retained window, not whole slow save
        config.disk_budget_mb = 512; disk.configure(config);
        CHECK(disk.stats().pinned_bytes > 0 && disk.stats().dropped_packets > 0);
        { std::lock_guard lk(save_gate_mutex); block_save = false; save_gate.notify_all(); }
        auto output = saved.get_future().get(); CHECK(!output.empty()); decode(output);
        // Snapshot files are released before the completion callback returns.
        CHECK(disk.stats().pinned_bytes == 0);
        while (disk.stats().saving) std::this_thread::sleep_for(1ms);
        std::promise<std::wstring> second_saved;
        CHECK(disk.save_clip((dir/"disk2").wstring(), [&](auto p) { second_saved.set_value(p); }));
        auto second_output = second_saved.get_future().get(); CHECK(!second_output.empty()); decode(second_output);
        disk.clear(); CHECK(file_count(segments_dir) == 0);
        CHECK(!disk.stats().saving);
        // Oversized payload is rejected before dereference/allocation/mux.
        auto huge = media.packets.front(); auto bytes = std::make_shared<encoding::EncodedBytes>();
        bytes->size = disk_segments::kMaxSegmentPayload + 1; huge.bytes = bytes;
        disk.push(huge); CHECK(disk.stats().logical_bytes == 0 && disk.stats().pressure);
    }
    CHECK(file_count(segments_dir) == 0);
    // A nonempty directory at an owned segment pathname simulates a failed
    // unlink even when tests run as root. Never recursively delete its child.
    {
        disk_segments::DiskSegmentBuffer disk;
        disk.configure({8, (dir/"failed-delete").wstring(), "mkv", 512});
        disk.set_video_params(media.video); disk.set_audio_params(media.audio);
        for (const auto& p : media.packets) disk.push(shifted(p, 1));
        fs::path closed;
        for (const auto& p : fs::recursive_directory_iterator(dir/"failed-delete"))
            if (p.is_regular_file() && p.path().filename() == "seg_0.mkv") closed = p.path();
        CHECK(!closed.empty());
        auto backup = closed; backup += ".test-backup"; fs::rename(closed, backup);
        fs::create_directory(closed); std::ofstream(closed/"unowned-child") << "preserve";
        for (const auto& p : media.packets) disk.push(shifted(p, 30));
        CHECK(disk.stats().delete_failures > 0 && disk.stats().pressure);
        CHECK(disk.stats().logical_bytes > 0 && fs::exists(closed/"unowned-child"));
        fs::remove(closed/"unowned-child"); fs::remove(closed); fs::rename(backup, closed);
        disk.clear(); CHECK(file_count(dir/"failed-delete") == 0);
    }
    // Cancel a reader while it is held in the deterministic slow-mux gate.
    {
        auto disk = std::make_unique<disk_segments::DiskSegmentBuffer>();
        disk->configure({8, (dir/"cancel-segments").wstring(), "mkv", 512});
        disk->set_video_params(media.video); disk->set_audio_params(media.audio);
        for (const auto& p : media.packets) disk->push(shifted(p, 1));
        { std::lock_guard lk(save_gate_mutex); block_save = true; save_entered = false; }
        std::promise<std::wstring> saved;
        CHECK(disk->save_clip((dir/"cancel-output").wstring(), [&](auto path) { saved.set_value(path); }));
        { std::unique_lock lk(save_gate_mutex); CHECK(save_gate.wait_for(lk, 5s, [] { return save_entered; })); }
        auto clear = std::async(std::launch::async, [&] { disk->clear(); });
        std::this_thread::sleep_for(50ms);
        { std::lock_guard lk(save_gate_mutex); block_save = false; save_gate.notify_all(); }
        CHECK(clear.wait_for(5s) == std::future_status::ready); clear.get();
        CHECK(saved.get_future().get().empty());
        CHECK(file_count(dir/"cancel-output") == 0 && file_count(dir/"cancel-segments") == 0);
    }
    using nlohmann::json;
    for (const auto& value : {json(), json("4096"), json(true), json(512.0), json(-1), json(511), json(65537), json(UINT64_MAX)})
        CHECK(settings::replay_disk_budget_mb_from_json({{"replay_buffer", {{"disk_budget_mb", value}}}}) == 2048);
    CHECK(settings::replay_disk_budget_mb_from_json({}) == 2048);
    CHECK(settings::replay_disk_budget_mb_from_json({{"replay_buffer", {{"disk_budget_mb", 4096}}}}) == 4096);
    CHECK(disk_segments::validated_disk_budget_mb(-1) == 2048);
    CHECK(disk_segments::validated_disk_budget_mb(512) == 512);
    CHECK(disk_segments::validated_disk_budget_mb(65536) == 65536);
    CHECK(disk_segments::validated_disk_budget_mb(INT64_MAX) == 2048);
    report.close();
    command("ffmpeg -v error -y -f lavfi -i testsrc2=size=160x90:rate=30:duration=60 -f lavfi -i sine=frequency=440:sample_rate=48000:duration=60 -f lavfi -i sine=frequency=880:sample_rate=44100:duration=60 -map 0:v -map 1:a -map 2:a -c:v libx264 -preset veryfast -threads 2 -b:v 8M -minrate 8M -maxrate 8M -bufsize 16M -x264-params nal-hrd=cbr -c:a aac \"" + (dir/"long.mp4").string() + "\"");
    CHECK(fs::file_size(dir/"long.mp4") > 50 * 1024 * 1024);
    CHECK(encoding::trim_clip_reencode((dir/"long.mp4").wstring(), 0.3, 59.2, (dir/"long-trim.mp4").wstring(), &error, &cut));
    CHECK(cut.duration_seconds > 58 && cut.duration_seconds < 60); decode(dir/"long-trim.mp4");
    std::cout << "PASS phase2 media: trim/remux/reencode, fractional/VFR B-frames, all audio, skewed RAM replay, disk key starts and pinned slow-save retention\n";
}
