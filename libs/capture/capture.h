#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <functional>

namespace capture {

struct FrameInfo {
    int64_t  timestamp_qpc; // QueryPerformanceCounter at ingest
    uint32_t width;
    uint32_t height;
    uint32_t seq;           // monotonic, resets on start()
};

using FrameCallback = std::function<void(const FrameInfo&)>;

// Returns false if the OS does not support Windows.Graphics.Capture.
bool is_supported();

// Captures a display via Windows.Graphics.Capture.
// Frames stay on GPU — no CPU readback.
// Caller must call winrt::init_apartment() before start().
class DisplayCapture {
public:
     DisplayCapture();
    ~DisplayCapture();

    DisplayCapture(const DisplayCapture&)            = delete;
    DisplayCapture& operator=(const DisplayCapture&) = delete;

    // cb is invoked from the WGC thread pool — must be thread-safe.
    bool start(HMONITOR hmon, FrameCallback cb);
    void stop();
    bool running() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace capture
