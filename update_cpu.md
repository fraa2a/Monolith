# Monolith CPU Optimization Update

**Target audience:** AI developer agent / C++ systems developer  
**Project:** Monolith — Windows clipping/recording software  
**Scope:** Reduce CPU usage that scales with recording/replay FPS  
**Status:** New focused implementation plan, separate from the general `update.md` RAM/replay-buffer document.

---

## 0. Executive Summary

Monolith currently shows a clear and reproducible behavior: **the higher the configured recording/replay FPS, the higher the CPU usage**. This is expected with the current architecture because each emitted frame causes CPU-side work:

1. Windows Graphics Capture receives a frame.
2. The GPU texture is copied into a CPU-readable D3D11 staging texture.
3. The staging texture is mapped with `D3D11_MAP_READ`.
4. The frame is copied into a CPU BGRA buffer.
5. The pacer emits frames at the configured FPS.
6. The encoder receives CPU BGRA frames.
7. `sws_scale()` converts/scales BGRA into the encoder pixel format.
8. The encoded packets are routed into replay/recording.

This means that increasing FPS from 30 to 60 roughly doubles much of the CPU-side video work. Increasing from 60 to 120 can multiply it again.

OBS Studio also has CPU work that scales with FPS, but its design is more efficient because it separates:

- graphics/render tick;
- raw CPU video outputs;
- GPU encoder outputs;
- frame-rate-divided output consumers;
- GPU conversion/download only when raw output is active;
- GPU encoder path that can avoid unnecessary CPU readback.

Monolith has already improved RAM/replay buffer behavior with ref-counted encoded packet payloads, but the **CPU path is still fundamentally CPU-readback + CPU-conversion based**.

The goal of this document is to give a developer agent a concrete, staged plan to reduce CPU usage without breaking clipping, manual recording, audio sync, or replay buffer correctness.

---

## 1. Current Problem Observed by User

The user confirmed:

> Increasing recording FPS increases CPU usage. Lowering recording FPS reduces CPU usage.

This is the most important diagnostic clue.

It means the CPU cost is mainly per-frame, not a constant background loop. The most likely per-frame costs are:

- D3D11 GPU → CPU readback;
- BGRA memory copies;
- CPU scaling/color conversion;
- encoder frame submission;
- pacer wakeups/catch-up behavior;
- possible audio mixer work, but audio is secondary for FPS-linked CPU.

---

## 2. Current Monolith Video Pipeline

Based on the current source, Monolith still uses this pipeline:

```text
Windows Graphics Capture frame
    ↓
D3D11 GPU texture
    ↓ CopyResource
D3D11 staging texture, CPU-readable
    ↓ Map(D3D11_MAP_READ)
CPU pointer to BGRA pixels
    ↓ memcpy
pacer shared BGRA buffer
    ↓ pacer thread at configured FPS
VideoEncoder::push_bgra(...)
    ↓ sws_scale
encoder pixel format frame
    ↓ avcodec_send_frame / receive packets
encoded packets
    ↓
replay buffer / manual recorder
```

This design is simple and correct, but it is not lightweight. It makes the CPU touch full-resolution image memory for every frame that reaches the encoder.

Approximate raw BGRA size:

| Resolution | BGRA bytes per frame | 30 FPS raw throughput | 60 FPS raw throughput | 120 FPS raw throughput |
|---|---:|---:|---:|---:|
| 1280x720 | ~3.7 MB | ~111 MB/s | ~221 MB/s | ~442 MB/s |
| 1920x1080 | ~8.3 MB | ~249 MB/s | ~498 MB/s | ~995 MB/s |
| 2560x1440 | ~14.7 MB | ~442 MB/s | ~884 MB/s | ~1.77 GB/s |
| 3840x2160 | ~33.2 MB | ~995 MB/s | ~1.99 GB/s | ~3.98 GB/s |

This is only the raw memory traffic. It does not include:

- GPU readback synchronization cost;
- cache misses;
- scaling;
- color conversion;
- encoder overhead;
- thread synchronization;
- mutex cost;
- audio processing.

Therefore a stable ~10% CPU at 60 FPS is not surprising with this architecture.

---

## 3. Current Monolith Weak Points

### 3.1 WGC currently performs CPU readback before application-level dropping

The capture layer currently receives a WGC frame, obtains the D3D11 texture, creates/reuses a staging texture, calls `CopyResource`, maps the texture, then calls the callback with BGRA data.

The problem is that any frame dropping/throttling in the main recorder code happens **after** this expensive readback. If WGC delivers frames at monitor refresh rate or faster than the configured recording FPS, Monolith may still pay for frames it does not ultimately need.

#### Required change

Move frame gating into `libs/capture/capture.cpp`, before:

```cpp
CopyResource(...)
Map(...)
```

The capture layer should know the target readback FPS and reject frames early.

---

### 3.2 `MinUpdateInterval(1ms)` is too aggressive for a lightweight recorder

The capture session currently disables the default WGC throttle with a 1 ms minimum update interval. This can be useful for low-latency/high-FPS capture, but for a lightweight replay recorder it is dangerous.

With a 1 ms interval, WGC is allowed to wake Monolith frequently. Even if the pacer later emits only 60 FPS, the capture callback can still run more often, and each accepted frame can be expensive.

#### Required change

Do not hardcode 1 ms. Compute it from the effective capture FPS:

```cpp
int target_fps = max(1, configured_capture_fps);
int interval_ms = max(1, 1000 / target_fps);
session.MinUpdateInterval(std::chrono::milliseconds(interval_ms));
```

For example:

| Target FPS | Suggested MinUpdateInterval |
|---:|---:|
| 30 | 33 ms |
| 60 | 16 ms |
| 120 | 8 ms |
| Unlimited/debug | 1 ms |

Do not use unlimited/1 ms in the normal lightweight profile.

---

### 3.3 Pacer is useful but does not reduce readback cost enough

Monolith has an OBS-style pacer thread that emits frames at a fixed configured FPS. This is conceptually correct for CFR output and timestamp stability.

However, the pacer only controls when frames are submitted to the encoder. It does not automatically prevent expensive capture-side readback.

The current pacer path still copies BGRA into a shared `std::vector`, then the pacer thread submits it to the encoder.

#### Required change

Keep the pacer, but pair it with capture-side gating. The pacer should not be the first and only throttle.

The final design should be:

```text
WGC frame arrived
    ↓
Capture gate: is this frame needed for target FPS?
    ↓ no
Drop before CopyResource/Map
    ↓ yes
CopyResource + Map
    ↓
Pacer latest-frame buffer
    ↓
Deadline-based CFR emission
```

---

### 3.4 CPU-side `sws_scale()` runs per emitted frame

The encoder still receives CPU BGRA frames through `push_bgra()`, and `encoding.cpp` uses `sws_scale()` to convert/scale BGRA into the encoder frame format.

This cost scales almost directly with output FPS and resolution.

#### Required change

Short-term:

- minimize how often `push_bgra()` is called;
- reduce resolution/FPS in light presets;
- avoid scaling when source size already matches output size;
- prefer fast scaler flags for light mode.

Medium-term:

- perform downscale/color conversion on GPU before CPU readback;
- read back NV12/I420-sized output instead of full BGRA where possible.

Long-term:

- implement GPU texture encoder path and avoid CPU readback/conversion for hardware encoders.

---

### 3.5 Media pipeline may start when it is not needed

The recorder must not run WGC/pacer/encoder in cases where no video is needed.

Expected behavior:

| Replay enabled | Manual recording state | Should capture video? |
|---|---|---|
| OFF | Idle | No |
| OFF | Recording | Yes |
| OFF | Paused | Usually yes or configurable |
| ON | Idle | Yes, because replay needs buffer |
| ON | Recording | Yes |

If replay is off and manual recording is idle, video capture should be fully stopped.

#### Required change

Do not start:

- WGC capture;
- pacer thread;
- video encoder;
- BGRA frame buffers;

unless video is actually required.

Manual recording start should be able to start the video pipeline on demand. Manual recording stop should stop it again when replay is disabled.

---

## 4. How OBS Handles the Same Problem

OBS also has frame-rate-related CPU/GPU work, but its architecture is more mature.

Important design ideas to learn from OBS:

### 4.1 OBS has a central video timing system

OBS uses a video output thread/model where frame timing is explicit. Output consumers can connect to video output with a frame-rate divisor.

Conceptually:

```text
base video rate = 60 FPS
consumer divisor = 1 → receives 60 FPS
consumer divisor = 2 → receives 30 FPS
consumer divisor = 4 → receives 15 FPS
```

This means OBS can run one source/render pipeline while allowing consumers to receive fewer frames without each consumer doing its own busy polling.

Monolith's pacer is a good start, but it needs capture-side gating and better deadline handling.

### 4.2 OBS separates raw CPU output from GPU encoder output

OBS tracks whether raw CPU output is active and whether GPU encoder output is active. In its render path, GPU conversion and raw download are not the same thing.

Simplified:

```text
if GPU encoder active:
    render/convert for GPU encoder path

if raw output active:
    stage/download frame to CPU
```

This distinction is critical.

Monolith currently always follows a CPU-readable BGRA path before encoding. This is the opposite of the most efficient hardware-encoder architecture.

### 4.3 OBS avoids CPU download when it can use GPU encoder textures

OBS has code paths for GPU encoder output that queue GPU textures/handles for encoding. This avoids forcing every frame through CPU memory.

For Monolith, this is the biggest long-term optimization:

```text
WGC D3D11 texture
    ↓
GPU conversion/downscale if needed
    ↓
D3D11/NV12/hardware frames
    ↓
NVENC/AMF/QSV/Media Foundation encoder
    ↓
encoded packets
```

No full BGRA CPU readback should be necessary in this path.

### 4.4 OBS has profiling hooks and skipped-frame metrics

OBS measures frame timing, skipped frames, lagged frames, and profiling sections such as rendering, download, and output.

Monolith needs similar telemetry to prove that CPU optimizations work.

---

## 5. Target Architecture for Monolith

### 5.1 Short-term target architecture

This is achievable without rewriting the encoder completely:

```text
Capture session configured with target FPS
    ↓
WGC frame arrived
    ↓
CaptureFrameGate checks timestamp/frame interval
    ↓ drop early if not needed
D3D11 CopyResource + Map only for accepted frames
    ↓
LatestFrameBuffer stores newest accepted BGRA frame
    ↓
Pacer emits CFR frames using deadline-based wait
    ↓
VideoEncoder::push_bgra
    ↓
Replay/Recording
```

Expected improvement:

- CPU should drop significantly at 30/60 FPS on high-refresh monitors.
- CPU should no longer scale with monitor refresh rate when output FPS is lower.
- CPU will still scale with configured output FPS, but less wastefully.

### 5.2 Medium-term target architecture

```text
WGC D3D11 texture
    ↓
GPU shader/downscale to output resolution
    ↓
Read back only output-sized frame
    ↓
Optional CPU conversion / encoder
```

Expected improvement:

- Big CPU/memory bandwidth reduction when capturing high-res monitor but recording 1080p/720p.
- Still not as good as true GPU encoding, but much better than reading full source BGRA.

### 5.3 Long-term target architecture

```text
WGC D3D11 texture
    ↓
GPU color conversion/downscale to NV12
    ↓
Hardware encoder consumes GPU texture/surface
    ↓
Encoded packet
    ↓
Replay/Recording
```

Expected improvement:

- Largest CPU reduction.
- Best chance to compete with commercial clipping software.
- Enables high FPS with lower CPU.

---

## 6. Implementation Plan

## Phase 1 — Prove and Measure

Add telemetry before making large changes.

### 6.1 Add CPU/FPS video telemetry

Add counters:

```cpp
struct VideoPerfCounters {
    std::atomic<uint64_t> wgc_frames_arrived;
    std::atomic<uint64_t> frames_dropped_before_readback;
    std::atomic<uint64_t> frames_readback;
    std::atomic<uint64_t> pacer_frames_submitted;
    std::atomic<uint64_t> encoder_frames_submitted;
    std::atomic<uint64_t> encoder_packets_output;
    std::atomic<uint64_t> readback_time_us_total;
    std::atomic<uint64_t> sws_scale_time_us_total;
    std::atomic<uint64_t> encoder_push_time_us_total;
};
```

Log every 5 seconds:

```text
[perf] wgc=734 drop_pre_readback=374 readback=360 enc_frames=360 avg_readback_ms=1.20 avg_sws_ms=2.10 avg_push_ms=3.40 fps=60
```

The developer agent must add measurements around:

- WGC callback entry;
- pre-readback gate;
- `CopyResource + Map`;
- `pacer_push_frame`;
- `VideoEncoder::push_bgra`;
- `sws_scale()`;
- `avcodec_send_frame/receive_packet`.

### 6.2 Add test matrix

The developer must test:

| Scenario | Expected |
|---|---|
| Replay OFF, recording idle | ~0% video CPU |
| Replay ON 30 FPS | CPU lower |
| Replay ON 60 FPS | CPU higher than 30, but lower than before |
| Replay ON 120 FPS | CPU higher, but no hidden extra monitor-refresh waste |
| 144/165 Hz monitor, output 60 FPS | WGC arrived may be high, readback should be ~60 FPS |
| Source 1440p, output 1080p | after medium-term GPU downscale, CPU lower |

---

## Phase 2 — Capture-Side Frame Gate

### 6.3 Add target FPS to capture API

Change capture API from:

```cpp
bool DisplayCapture::start(HMONITOR hmon, FrameCallback cb, bool show_border);
```

to something like:

```cpp
struct CaptureOptions {
    bool show_border = true;
    int max_readback_fps = 60;
    bool allow_unlimited_readback = false;
};

bool DisplayCapture::start(HMONITOR hmon, FrameCallback cb, CaptureOptions options);
```

### 6.4 Implement `FrameGate`

Add a small gate inside capture:

```cpp
class FrameGate {
public:
    void reset(int fps, int64_t qpc_freq) {
        fps_ = std::max(1, fps);
        qpc_freq_ = qpc_freq;
        next_qpc_ = 0;
    }

    bool should_accept(int64_t now_qpc) {
        if (fps_ <= 0 || qpc_freq_ <= 0)
            return true;

        const int64_t interval = qpc_freq_ / fps_;

        if (next_qpc_ == 0) {
            next_qpc_ = now_qpc + interval;
            return true;
        }

        if (now_qpc < next_qpc_)
            return false;

        // Avoid catch-up storms after stalls.
        const int64_t max_late = interval * 2;
        if (now_qpc - next_qpc_ > max_late)
            next_qpc_ = now_qpc + interval;
        else
            next_qpc_ += interval;

        return true;
    }

private:
    int fps_ = 60;
    int64_t qpc_freq_ = 0;
    int64_t next_qpc_ = 0;
};
```

Use it before readback:

```cpp
auto frame = pool.TryGetNextFrame();
if (!frame) return;

LARGE_INTEGER qpc{};
QueryPerformanceCounter(&qpc);

if (!impl_->frame_gate.should_accept(qpc.QuadPart)) {
    impl_->perf.frames_dropped_before_readback++;
    frame.Close();
    return;
}

// Only now do GPU→CPU readback.
```

### 6.5 Configure WGC `MinUpdateInterval` from FPS

Replace hardcoded 1 ms with:

```cpp
int min_update_ms = 1;
if (!options.allow_unlimited_readback && options.max_readback_fps > 0) {
    min_update_ms = std::max(1, 1000 / options.max_readback_fps);
}

impl_->session.MinUpdateInterval(std::chrono::milliseconds(min_update_ms));
```

Suggested behavior:

| Mode | max_readback_fps | MinUpdateInterval |
|---|---:|---:|
| Light 30 FPS | 30 | 33ms |
| Balanced 60 FPS | 60 | 16ms |
| High FPS 120 | 120 | 8ms |
| Debug/unlimited | unlimited | 1ms |

---

## Phase 3 — Deadline-Based Pacer Improvements

The current pacer is conceptually correct, but it must avoid unnecessary wakeups and catch-up bursts.

### 6.6 Use one frame deadline at a time

Pacer should wait until the exact next output frame deadline, then emit at most one frame in normal mode.

Avoid a loop that submits many frames after a stall. Catch-up bursts increase CPU and can make the app feel worse.

Recommended behavior:

```cpp
while (!stopping) {
    wait_until(next_deadline);

    if (stopping) break;

    if (latest_frame_available) {
        submit_latest_frame_once();
        frames_done++;
    }

    next_deadline = start_time + frames_done * frame_duration;

    if (now_is_too_late) {
        // Drop rather than catch up endlessly.
        frames_done = compute_frame_index(now);
        next_deadline = start_time + frames_done * frame_duration;
        perf.pacer_dropped_due_to_lag++;
    }
}
```

### 6.7 Make catch-up policy configurable

Add internal setting:

```json
"video": {
  "max_pacer_catchup_frames": 1
}
```

Default:

```text
Light/Balanced: 1
Quality/debug: 2-4
```

For clipping software, smooth real-time behavior matters more than encoding many duplicate frames after a stall.

### 6.8 Avoid `timeBeginPeriod(1)` unless needed

`timeBeginPeriod(1)` increases global timer resolution and can increase system power usage. If the waitable timer is precise enough, avoid it by default.

Recommended setting:

```json
"video": {
  "high_precision_timer": false
}
```

Use `timeBeginPeriod(1)` only when:

- high FPS mode is enabled;
- user explicitly enables low-latency/high-precision timing;
- profiling proves it is needed.

---

## Phase 4 — Do Not Run Video Pipeline When Not Needed

### 6.9 Add explicit video pipeline state

Create a state machine:

```cpp
enum class VideoPipelineState {
    Stopped,
    Starting,
    RunningForReplay,
    RunningForRecording,
    RunningForReplayAndRecording,
    Stopping
};
```

### 6.10 Start/stop rules

```cpp
bool needs_video_pipeline() {
    if (settings.replay_buffer.enabled)
        return true;

    if (settings.recording.enabled && recording.state() != Idle)
        return true;

    return false;
}
```

Rules:

```text
On app start:
    if replay enabled → start video pipeline
    else → do not start video pipeline

On manual recording start:
    if video pipeline stopped → start it

On manual recording stop:
    if replay disabled → stop video pipeline

On replay enable:
    start video pipeline

On replay disable:
    if recording idle → stop video pipeline
```

This ensures idle manual recording support does not waste CPU.

---

## Phase 5 — Reduce CPU Work Per Frame

### 6.11 Avoid scaling if not needed

In `VideoEncoder::push_bgra()`, avoid recreating/using `sws_scale` when:

```text
input width == output width
input height == output height
input format is already acceptable
```

BGRA usually still needs color conversion for many encoders, but avoid scaling cost when dimensions match.

### 6.12 Light preset should use faster scaler

For light mode, use faster/lower-quality scaling flags.

Recommended:

```text
Light: SWS_FAST_BILINEAR
Balanced: SWS_BILINEAR
Quality: SWS_BICUBIC or current default
```

The user cares about low overhead. Light mode should be aggressively optimized.

### 6.13 GPU downscale before CPU readback

If source monitor is 1440p/4K and output is 1080p/720p, do not read full source BGRA to CPU.

Medium-term path:

```text
WGC texture at source resolution
    ↓
D3D11 render pass downscales to output resolution
    ↓
read back output-sized BGRA/NV12
```

This can massively reduce CPU memory bandwidth.

Example:

```text
4K BGRA frame: ~33 MB
1080p BGRA frame: ~8 MB
720p BGRA frame: ~3.7 MB
```

Reading back after GPU downscale can reduce CPU memory traffic by 4x-9x.

---

## Phase 6 — Long-Term GPU Encoder Path

This is the real solution for making Monolith competitive with Medal/OBS-like efficiency.

### 6.14 Add a D3D11 texture encoder interface

Add a new encoder path, not a replacement for the CPU path at first:

```cpp
class VideoEncoder {
public:
    bool open_cpu_bgra(const VideoConfig& cfg);
    bool open_d3d11(const VideoConfig& cfg, ID3D11Device* device);

    void push_bgra(const uint8_t* data, int stride, int width, int height, int64_t pts);
    void push_d3d11_texture(ID3D11Texture2D* texture, int width, int height, int64_t pts);
};
```

### 6.15 Hardware encoder strategy

Possible implementations:

1. **FFmpeg D3D11VA / AVHWFramesContext path**
   - Harder but integrates with current FFmpeg-based muxing.

2. **Media Foundation hardware encoder path**
   - Native Windows path.
   - Potentially cleaner for D3D11 surfaces.
   - Requires careful H.264/H.265 packet handling and mux integration.

3. **Vendor-specific NVENC/AMF/QSV path**
   - Maximum control and performance.
   - More maintenance complexity.

Recommended staged approach:

```text
Stage A: keep CPU path as fallback
Stage B: implement D3D11 hardware path for one encoder family first
Stage C: expose "Auto / Hardware D3D11 / CPU fallback"
Stage D: add telemetry comparing CPU path vs GPU path
```

### 6.16 Acceptance criteria for GPU path

| Test | Expected |
|---|---|
| 1080p60 replay ON | CPU significantly below CPU-BGRA path |
| 1440p60 replay ON | CPU no longer dominated by `sws_scale` |
| 4K60 replay ON | CPU much lower than readback path, GPU usage may increase |
| Save clip | output valid and timestamp-correct |
| Manual recording + replay | no packet duplication regression |
| Encoder unavailable | fallback to CPU path |

---

## 7. Recommended Settings / Presets

Add explicit performance presets.

### 7.1 Light preset

```json
{
  "video": {
    "fps": 30,
    "width": 1280,
    "height": 720,
    "capture_readback_fps": 30,
    "high_precision_timer": false,
    "max_pacer_catchup_frames": 1,
    "scaler": "fast_bilinear"
  },
  "replay_buffer": {
    "duration_seconds": 30,
    "memory_budget_mb": 128
  }
}
```

### 7.2 Balanced preset

```json
{
  "video": {
    "fps": 60,
    "width": 1920,
    "height": 1080,
    "capture_readback_fps": 60,
    "high_precision_timer": false,
    "max_pacer_catchup_frames": 1,
    "scaler": "bilinear"
  },
  "replay_buffer": {
    "duration_seconds": 30,
    "memory_budget_mb": 128
  }
}
```

### 7.3 Quality preset

```json
{
  "video": {
    "fps": 60,
    "width": "source",
    "height": "source",
    "capture_readback_fps": 60,
    "high_precision_timer": true,
    "max_pacer_catchup_frames": 2,
    "scaler": "bicubic"
  },
  "replay_buffer": {
    "duration_seconds": 60,
    "memory_budget_mb": 256
  }
}
```

### 7.4 High FPS preset

```json
{
  "video": {
    "fps": 120,
    "capture_readback_fps": 120,
    "high_precision_timer": true,
    "max_pacer_catchup_frames": 1,
    "prefer_gpu_encoder": true
  }
}
```

High FPS should strongly recommend GPU encoder path once available.

---

## 8. Concrete Developer Checklist

### Immediate checklist

- [ ] Add video perf counters.
- [ ] Log WGC arrived/readback/dropped/encoded every 5 seconds.
- [ ] Add `CaptureOptions` with `max_readback_fps`.
- [ ] Add capture-side `FrameGate` before `CopyResource` and `Map`.
- [ ] Replace hardcoded `MinUpdateInterval(1ms)` with FPS-derived interval.
- [ ] Ensure WGC/pacer/video encoder do not start if replay is off and recording is idle.
- [ ] Ensure manual recording start can start video pipeline on demand.
- [ ] Ensure manual recording stop stops video pipeline if replay is off.
- [ ] Limit pacer catch-up bursts.
- [ ] Avoid `timeBeginPeriod(1)` by default.

### Medium checklist

- [ ] Add presets: Light/Balanced/Quality/High FPS.
- [ ] Use faster scaler for Light preset.
- [ ] Add GPU downscale before CPU readback.
- [ ] Read back output-sized frame, not source-sized frame, when output resolution is lower.

### Long-term checklist

- [ ] Add D3D11 texture encoder API.
- [ ] Implement one hardware GPU path.
- [ ] Keep CPU BGRA fallback.
- [ ] Compare CPU/GPU usage with telemetry.
- [ ] Prefer GPU encoder path in Balanced/Quality/High FPS.

---

## 9. Acceptance Criteria

### 9.1 CPU behavior

On the same machine and same scene:

| Scenario | Expected result |
|---|---|
| Replay OFF + recording idle | Near-zero Monolith video CPU |
| Replay ON 30 FPS | Clearly lower CPU than 60 FPS |
| Replay ON 60 FPS | Lower CPU than previous build at same FPS |
| 144Hz monitor + 60 FPS output | readback count should be ~60/sec, not ~144/sec |
| 165Hz monitor + 30 FPS output | readback count should be ~30/sec, not ~165/sec |
| Manual recording idle, replay off | no WGC frame readback |

### 9.2 Functional correctness

- Replay still records valid clips.
- Manual recording still starts quickly.
- Manual recording stop finalizes file correctly.
- Audio/video sync remains correct.
- Timestamp monotonicity is preserved.
- No black frames at clip start.
- No regression in OBS-style replay packet lifetime work.

### 9.3 Telemetry correctness

Every 5 seconds, logs should make it obvious where CPU is going:

```text
[perf-video]
  wgc_arrived=720
  dropped_pre_readback=360
  readback=360
  pacer_submitted=360
  encoder_submitted=360
  avg_readback_ms=...
  avg_sws_ms=...
  avg_encode_submit_ms=...
```

The ratio must prove that output 60 FPS on 144Hz monitor does not read back 144 frames per second.

---

## 10. Summary for the AI Developer Agent

Do not try to solve this by only changing encoder settings. The CPU increase with FPS is architectural.

The first real fix is:

```text
Drop unneeded WGC frames before GPU→CPU readback.
```

The second real fix is:

```text
Do not run the video pipeline when no video output is active.
```

The third real fix is:

```text
Make the pacer deadline-based and avoid catch-up bursts/timer overuse.
```

The long-term real fix is:

```text
Stop sending every frame through CPU BGRA and sws_scale. Add a D3D11/GPU encoder path.
```

OBS is efficient not because FPS is free, but because it has a more mature architecture that separates raw CPU outputs from GPU encoder outputs and only downloads frames to CPU when a raw output actually needs them.

Monolith should move toward the same model while keeping the current CPU path as fallback.

---

## 11. Reference Notes

Relevant Monolith areas to inspect:

```text
libs/capture/capture.cpp
app/recorder/src/main.cpp
libs/encoding/encoding.cpp
app/recorder/src/settings_config.cpp
```

Relevant OBS areas to study conceptually:

```text
libobs/media-io/video-io.c
libobs/obs-video.c
```

Do not copy OBS code directly. Use the architectural ideas:

- frame-rate-divided consumers;
- raw output vs GPU output separation;
- GPU encoder queue;
- explicit skipped/lagged frame metrics;
- no CPU download unless needed.
