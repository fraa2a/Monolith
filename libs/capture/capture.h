#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <functional>

namespace capture {

struct CaptureOptions {
    bool show_border = true;
    int max_readback_fps = 60;
    bool allow_unlimited_readback = false;
    HWND target_window = nullptr;

    // Desired readback size. When both are > 0 and differ from the captured
    // content size, the frame is downscaled on the GPU (D3D11 video
    // processor) before the CPU staging readback, so readback/memcpy cost
    // scales with the smaller output size instead of native resolution.
    // 0 (default) = capture at native size, no GPU scaling step.
    int output_width = 0;
    int output_height = 0;
};

struct CaptureStats {
    uint64_t frames_arrived = 0;
    uint64_t frames_dropped_before_readback = 0;
    uint64_t frames_readback = 0;
    uint64_t readback_time_us_total = 0;
};

struct FrameInfo {
    int64_t  timestamp_qpc; // QueryPerformanceCounter at ingest
    uint32_t width;
    uint32_t height;
    uint32_t seq;           // monotonic, resets on start()

    // CPU-readable BGRA pixel data via a D3D11 staging texture.
    // Valid only for the duration of the callback; do not cache the pointer.
    // nullptr on readback failure.  Stride >= width*4 (GPU row alignment).
    // Spike path: GPU-encoder path (NVENC direct) is deferred to Milestone 4.
    const uint8_t* bgra_data;
    uint32_t       bgra_stride;
};

using FrameCallback = std::function<void(const FrameInfo&)>;

// Returns false if the OS does not support Windows.Graphics.Capture.
bool is_supported();

// Captures a display via Windows.Graphics.Capture.
// FrameInfo.bgra_data provides a CPU-readable copy via a D3D11 staging texture.
// Caller must call winrt::init_apartment() before start().
class DisplayCapture {
public:
     DisplayCapture();
    ~DisplayCapture();

    DisplayCapture(const DisplayCapture&)            = delete;
    DisplayCapture& operator=(const DisplayCapture&) = delete;

    // cb is invoked from the WGC thread pool — must be thread-safe.
    // show_border = false requests yellow-border suppression via
    // GraphicsCaptureSession::IsBorderRequired(false); the OS may deny it.
    bool start(HMONITOR hmon, FrameCallback cb, bool show_border = false);
    bool start(HMONITOR hmon, FrameCallback cb, CaptureOptions options);
    void stop();
    bool running() const;
    CaptureStats stats() const;

    // True when border suppression was requested and the OS accepted it.
    bool border_suppressed() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace capture
