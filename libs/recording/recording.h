#pragma once

#include <encoding/encoding.h>

#include <functional>
#include <string>

namespace recording {

enum class RecordingState {
    Idle,
    Recording,
    Paused,
};

class ManualRecorder {
public:
     ManualRecorder();
    ~ManualRecorder();

    ManualRecorder(const ManualRecorder&)            = delete;
    ManualRecorder& operator=(const ManualRecorder&) = delete;

    void set_video_params(encoding::VideoStreamParams const& p);
    void set_audio_params(encoding::AudioStreamParams const& p);

    bool start(std::wstring output_dir, std::string container);
    bool pause();
    bool resume();
    bool stop(std::wstring* output_path = nullptr);

    void push(encoding::EncodedPacket pkt);

    RecordingState state() const;
    std::wstring current_path() const;

private:
    struct Impl;
    Impl* impl_;
};

const char* state_name(RecordingState state);

} // namespace recording
