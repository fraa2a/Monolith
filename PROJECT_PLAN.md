# PROJECT_PLAN.md

## 1. Executive Decision

**Recommendation: start from scratch, but benchmark against OBS/libobs early instead of forking OBS Studio.**

### Why not fork OBS Studio

- OBS is a mature general-purpose broadcaster with a large surface area: scenes, sources, services, browser integration, streaming, plugins, scripting, and a Qt-heavy frontend. Most of that is out of scope for this product.
- Forking OBS would inherit substantial architectural and operational complexity that does not help a tray-first lightweight recorder.
- A direct fork increases upgrade cost, merge friction, and long-term ownership burden.
- Reusing OBS code directly also raises licensing and distribution constraints that should be treated cautiously from day one.

### Why not build a libobs wrapper as the primary architecture

- A thin wrapper over `libobs` is viable for a spike and provides a performance baseline.
- It is still shaped around OBS concepts rather than a recorder-first product model.
- The replay-buffer, tray, hotkey, crash-resilient background-agent model will be easier to reason about in a purpose-built architecture.

### Why start from scratch

- The product needs a narrow, recorder-first pipeline: capture, sync, encode, ring buffer, clip save, tray control, and a minimal settings UI.
- A custom architecture can minimize process count, startup cost, memory residency, and CPU/GPU copies.
- It allows explicit design for Windows 11 capture APIs, WASAPI, hardware encoders, and crash-safe containers without carrying unrelated subsystems.

### Final decision

- **Primary path:** custom native Windows-first implementation.
- **Required early validation:** build two short spikes before milestone lock:
  - Spike A: custom capture/encode prototype.
  - Spike B: minimal `libobs` clip/record-only benchmark.
- If Spike A misses performance or stability targets badly, revisit a hybrid architecture. Until that happens, do not fork OBS.

## 2. Proposed Architecture

### Process structure

Use a **two-process model**:

- **UI/Tray Process**
  - Owns system tray icon, settings window, global hotkeys, notifications, and Stream Deck IPC endpoint registration.
  - Remains responsive and lightweight.
  - Can restart independently if the engine survives.
- **Recording Engine Process**
  - Owns capture, audio ingest, sync clocking, encoding, replay buffer, file writing, health reporting, and crash dump generation.
  - Runs headless in the background with a strict non-UI event loop.

Reasoning:

- Keeps capture/encode isolated from UI faults.
- Simplifies watchdog/restart behavior.
- Matches the requirement that recorder behavior remains resilient even if UI has issues.

### Main subsystems

1. **Command Gateway**
   - Receives commands from tray, hotkeys, and Stream Deck.
   - Validates state transitions before forwarding to the engine.
   - Supported commands in v1:
     - `save_replay`
     - `start_recording`
     - `stop_recording`
     - `pause_recording`
     - `resume_recording`
     - `reload_settings`
     - `query_status`

2. **Capture Pipeline**
   - Primary API: `Windows.Graphics.Capture` for display/window capture.
   - Secondary path: `Desktop Duplication API` for display capture fallback and performance comparison.
   - Internal frame format: keep frames on GPU as `ID3D11Texture2D` as long as possible.
   - Use a dedicated D3D11 device per engine process with explicit adapter selection.
   - Timestamp frames at capture ingress using a monotonic engine clock.

3. **Audio Pipeline**
   - WASAPI loopback for system audio.
   - Separate WASAPI capture for microphone.
   - Per-device capture workers feed normalized PCM into a mixer/router.
   - Support multiple logical tracks in v1:
     - system/game mix track
     - microphone track
     - optional extra input tracks if selected
   - Resample/rechannel internally to encoder-compatible formats while preserving per-track identity.

4. **Sync and Timing Core**
   - Central monotonic timeline for audio/video synchronization.
   - Capture timestamps normalized immediately at ingest.
   - Audio acts as the continuous clock source; video frames are aligned to that timeline.
   - Drift correction happens before muxing, not in UI logic.

5. **Encoder Pipeline**
   - Abstract encoder interfaces for video, audio, and muxing.
   - Preferred path: hardware video encoding on the selected GPU.
   - CPU fallback always available.
   - Encoded packets, not raw frames, feed the replay buffer in the default design.

6. **Replay Buffer**
   - Ring buffer over encoded packets plus packet metadata.
   - Maintain:
     - packet bytes
     - stream type
     - PTS/DTS
     - keyframe markers
     - clip eligibility window
   - Clip save walks backward to the nearest valid keyframe boundary before the requested duration and remuxes forward into a new output file.
   - Default container for the rolling buffer output path: MKV semantics, not MP4-first.

7. **Recording Manager**
   - Single state machine controlling all modes:
     - `idle`
     - `replay_armed`
     - `recording`
     - `recording_paused`
     - `saving_clip`
     - `faulted`
   - Replay mode and manual recording can coexist if accepted by performance budget; otherwise v1 should explicitly choose supported combinations and reject invalid states.
   - File naming, storage cap, cleanup, and remux tasks live here.

8. **Settings and Profile System**
   - Human-readable config file on disk plus schema versioning.
   - Separate persisted domains:
     - app settings
     - capture/encode settings
     - device bindings
     - hotkeys
     - profile overrides
   - Apply settings transactionally: validate first, then restart only the affected engine components.

9. **Tray and Hotkey Subsystem**
   - Hidden Win32 message window in the UI process.
   - `Shell_NotifyIcon` for tray icon and context menu.
   - `RegisterHotKey` for global hotkeys in v1.
   - Explicit conflict detection and validation UI.

10. **Stream Deck IPC Subsystem**
    - Stream Deck plugin written separately in TypeScript/Node as required by Elgato tooling.
    - Communication to app over local named pipe using newline-delimited JSON messages.
    - Optional localhost TCP/WebSocket support is deferred unless required by plugin constraints.

11. **Logging and Diagnostics**
    - Structured logs with subsystem tags:
      - `ui`
      - `ipc`
      - `capture`
      - `audio`
      - `encoder`
      - `mux`
      - `buffer`
      - `storage`
      - `hotkey`
    - Rotating files on disk plus in-memory recent-event buffer for crash reporting.

12. **Crash Recovery**
    - Engine process writes crash dumps and last-known state.
    - UI process detects unexpected engine death and offers:
      - silent engine restart if safe
      - user notification if active recording may be incomplete
    - Default recording container is chosen to maximize salvageability after abnormal termination.

## 3. Technology Choices

### Language

- **Recommendation:** `C++23` for engine and desktop app shell, `TypeScript` for Stream Deck plugin.
- Why:
  - Best fit for Win32, COM, D3D11, Windows.Graphics.Capture interop, and FFmpeg integration.
  - Lower integration risk than Rust for this specific Windows multimedia stack.
  - Easier parity with Microsoft samples and existing capture/encode references.
- Safe future option:
  - Rust may be introduced later for isolated libraries or tools, but should not be the initial engine language.

### Build system

- **Recommendation:** `CMake`.
- Why:
  - Strong Windows support.
  - Good integration with Visual Studio, Ninja, and CI.
  - Suitable for multi-target native repo layout.

### Package manager

- **Recommendation:** `vcpkg` with manifest mode.
- Why:
  - Predictable dependency pinning on Windows.
  - Common path for `fmt`, `spdlog`, `nlohmann-json`, `gtest`, and other native dependencies.
- Note:
  - FFmpeg packaging may still require a custom acquisition/build strategy depending on hardware encoder feature requirements.

### UI framework

- **Recommendation:** `WinUI 3` for settings UI plus raw `Win32` for tray/message-loop ownership.
- Why:
  - Native Windows 11 look for settings and onboarding.
  - Keeps browser runtimes out.
  - Win32 remains the most straightforward owner for tray icon and hotkey routing.
- Safer fallback:
  - If WinUI/App SDK friction is too high during spike work, switch to `Qt Widgets` for faster desktop UI delivery while keeping the engine untouched.

### Video capture API

- **Recommendation:** `Windows.Graphics.Capture` first, `Desktop Duplication API` as fallback/benchmark path.
- Why:
  - WGC is the modern Windows capture path for display/window capture.
  - Desktop Duplication remains important for fallback, certain full-desktop scenarios, and direct performance comparison.

### Audio capture API

- **Recommendation:** `WASAPI`.
- Why:
  - Native low-latency path.
  - Supports loopback capture, device selection, and multichannel PCM capture.
  - Best alignment with Windows-only scope.

### Encoding backend

- **Recommendation:** `FFmpeg/libav*` for muxing and codec control, with hardware video encoders through:
  - `NVENC`
  - `AMF`
  - `Intel QSV/oneVPL-backed path`
  - CPU fallback such as `libx264` or equivalent software encoder if licensing/distribution permits
- Why:
  - Broad codec/container support.
  - Mature muxing and remuxing tools.
  - Flexible enough for OBS-like settings.

### IPC method for Stream Deck

- **Recommendation:** `Named Pipes` on Windows.
- Why:
  - Native, local-only, low-overhead, no firewall prompts, and strong fit for command-style IPC.
  - Simple security model versus opening a localhost server by default.

### Installer/packaging system

- **Recommendation:** `WiX Toolset` or `Inno Setup` for initial installer, with `winget` distribution metadata.
- Default choice:
  - `WiX` if MSI-based enterprise-friendly installation matters early.
  - `Inno Setup` if faster iteration is more important for initial releases.
- Initial recommendation:
  - Start with `Inno Setup`, add `winget`, revisit MSI later if needed.

### Testing and profiling tools

- Unit/integration tests: `GoogleTest`
- Performance tracing: `Windows Performance Recorder` + `Windows Performance Analyzer`
- GPU inspection: `PIX for Windows`
- CPU/memory correctness: Visual Studio Profiler, AddressSanitizer where supported
- Crash dumps: `DbgHelp`/WER integration
- Static analysis:
  - MSVC `/analyze`
  - `clang-tidy`

## 4. Risk Register

| Risk | Why it is hard | De-risk plan |
|---|---|---|
| GPU frame capture performance | High refresh rate capture can create GPU copy pressure and timing instability | Build Spike A with GPU-resident frame flow only; measure 1080p/1440p/4K at 60/120/144 Hz; reject designs that force CPU readback in steady state |
| Audio/video synchronization | Separate device clocks and capture jitter create drift and mux issues | Introduce a dedicated sync/timing core early; stamp all ingress packets; define audio as master clock; build long-session drift tests before UI work |
| Replay buffer memory usage | Raw-frame buffering becomes impractical at high resolutions/bitrates | Buffer encoded packets by default; enforce configurable memory caps; degrade gracefully by reducing max duration/options when memory budget is exceeded |
| Hardware encoder availability | NVENC/AMF/QSV support varies by GPU, driver, and FFmpeg build | Implement capability probing at startup; expose only valid encoder options; always keep a software fallback and clear diagnostics |
| Hotkey conflicts | Global hotkeys can fail to register or collide with other apps/system shortcuts | Validate registration on apply; maintain conflict/error states in UI; provide fallback shortcuts and clear user feedback |
| Saving MP4/MKV from rolling buffer | Clip extraction requires keyframe-safe boundaries and container-safe finalization | Use encoded-packet ring buffer with keyframe indexing; default output to MKV; only remux to MP4 after successful finalization |
| HDR, high refresh, multi-monitor setups | Color spaces, adapter routing, and output duplication across monitors are easy failure points | Limit v1 to SDR output with documented HDR tone-map/down-conversion behavior if needed; test mixed-refresh/multi-GPU systems explicitly; record adapter/output metadata in logs |
| Anti-cheat false positives | Aggressive game hooking can trigger anti-cheat concerns | Avoid injected game capture in v1; stick to OS-supported capture APIs; document compatibility boundaries instead of attempting invasive hooks |
| Stream Deck plugin reliability | Plugin process lifecycle and app process lifecycle may desynchronize | Use idempotent pipe commands, request/response correlation IDs, heartbeats, and reconnect logic; surface connection state in plugin feedback |
| Disk I/O spikes during clip save | Saving a clip while recording can cause stalls on slower drives | Offload clip materialization to dedicated writer threads; pre-create temp files; profile HDD/SATA SSD/NVMe separately |
| Device hotplug and default-device changes | Audio devices can disappear mid-session | Subscribe to device notifications; define explicit recovery policy; test mic unplug/replug and default-render-device changes |
| Crash recovery and file salvage | Abnormal termination can leave files unusable | Prefer MKV for active writes; emit state checkpoints; add post-crash recovery/remux utility later if needed |

## 5. Milestone Roadmap

### Milestone 0: Architecture Spikes

- Goal:
  - Validate the core technical direction before main implementation.
- Deliverables:
  - Spike A: native custom capture/audio/encode prototype.
  - Spike B: minimal `libobs` benchmark prototype.
  - Comparative report with CPU, GPU, memory, latency, startup cost, and implementation complexity.
- Acceptance criteria:
  - Decision memo clearly confirms custom engine or triggers architectural revisit.
  - At least one hardware encoder path proven on a target Windows 11 machine.
- Complexity:
  - High.
- Dependencies:
  - Windows 11 dev machine, GPU drivers, baseline FFmpeg strategy.
- Test cases:
  - 1080p60 display capture to MKV.
  - System audio + mic capture.
  - 30-second replay clip save.

### Milestone 1: Headless Engine Skeleton

- Goal:
  - Establish a stable background engine with command handling and lifecycle management.
- Deliverables:
  - Engine process, command IPC, state machine, structured logging, crash-dump wiring.
- Acceptance criteria:
  - UI-less engine can start, stop, report health, and respond to commands without deadlocks.
- Complexity:
  - Medium.
- Dependencies:
  - Milestone 0 decision.
- Test cases:
  - Command sequencing.
  - Fault injection on engine restart.
  - Log rollover behavior.

### Milestone 2: Video and Audio Ingest

- Goal:
  - Capture stable display/window video and audio streams into timestamped internal queues.
- Deliverables:
  - WGC capture path.
  - WASAPI loopback and mic capture.
  - Unified timestamps and drift metrics.
- Acceptance criteria:
  - 30-minute capture run with bounded drift and no queue explosion.
- Complexity:
  - High.
- Dependencies:
  - Milestone 1.
- Test cases:
  - Window capture.
  - Full display capture.
  - Mic mute/unmute mid-session.
  - Device unplug cases.

### Milestone 3: Encoding and Muxing

- Goal:
  - Convert synchronized media streams into valid output files.
- Deliverables:
  - FFmpeg-backed video/audio encode abstraction.
  - MKV writer.
  - Hardware capability probing.
- Acceptance criteria:
  - Stable output files across supported encoders.
  - Fallback to software when hardware path fails.
- Complexity:
  - High.
- Dependencies:
  - Milestone 2.
- Test cases:
  - NVENC/AMF/QSV detection.
  - AAC audio muxing.
  - Invalid setting rejection.

### Milestone 4: Replay Buffer

- Goal:
  - Support continuous rolling capture and clip save.
- Deliverables:
  - Encoded-packet ring buffer.
  - Keyframe index.
  - Clip materialization flow.
- Acceptance criteria:
  - Saving the last X seconds succeeds repeatedly without destabilizing active capture.
- Complexity:
  - High.
- Dependencies:
  - Milestone 3.
- Test cases:
  - 15s/30s/120s buffer durations.
  - Repeated clip presses.
  - Save during heavy disk load.

### Milestone 5: Manual Recording Mode

- Goal:
  - Support start/stop/pause/resume recording with user-visible states.
- Deliverables:
  - Recording manager state machine.
  - File naming and storage management.
  - Optional auto-remux workflow.
- Acceptance criteria:
  - Pause/resume produces coherent output behavior according to defined mux policy.
- Complexity:
  - Medium to High.
- Dependencies:
  - Milestone 4.
- Test cases:
  - Long recording.
  - Pause/resume loops.
  - Storage-cap cleanup.

### Milestone 6: Tray, Hotkeys, and Settings UI

- Goal:
  - Make the app usable as a desktop background utility.
- Deliverables:
  - Tray icon/menu.
  - Settings window.
  - Hotkey registration and validation.
- Acceptance criteria:
  - User can configure and control replay/recording entirely without console interaction.
- Complexity:
  - Medium.
- Dependencies:
  - Milestone 5.
- Test cases:
  - Hotkey conflicts.
  - Minimize-to-tray behavior.
  - Settings apply without engine corruption.

### Milestone 7: Stream Deck Integration

- Goal:
  - Add first-party Stream Deck control path.
- Deliverables:
  - Elgato plugin.
  - Named-pipe client and reconnect logic.
  - Action icons and state feedback.
- Acceptance criteria:
  - All required actions work reliably across app restarts and temporary disconnects.
- Complexity:
  - Medium.
- Dependencies:
  - Milestone 6.
- Test cases:
  - App not running.
  - Engine restarting.
  - Rapid repeated button presses.

### Milestone 8: Hardening and Packaging

- Goal:
  - Prepare the product for real users.
- Deliverables:
  - Installer.
  - Diagnostics bundle export.
  - Crash handling polish.
  - Performance baselines and known-limitations document.
- Acceptance criteria:
  - Fresh install, upgrade, uninstall, and recovery paths work on clean Windows 11 systems.
- Complexity:
  - Medium.
- Dependencies:
  - Milestone 7.
- Test cases:
  - Clean install.
  - Upgrade install.
  - Crash and restart.
  - Missing dependency scenarios.

## 6. First Prototype Plan

### Objective

Build the smallest prototype that proves the hardest v1 engineering problems without building the full app.

### Prototype scope

- Headless native executable plus minimal tray host.
- Capture a selected display using `Windows.Graphics.Capture`.
- Capture system audio via WASAPI loopback.
- Capture one microphone device via WASAPI.
- Encode video with one hardware path if available, otherwise software fallback.
- Mux into MKV.
- Keep a 30-second replay buffer of encoded packets.
- Register one global hotkey to save a clip.
- Expose tray menu items:
  - arm replay buffer
  - save clip
  - exit

### What this prototype must prove

- GPU-resident capture can feed the encoder without unnecessary CPU readback.
- Audio and video can stay synchronized for at least a 10-minute run.
- Encoded-packet replay buffering is practical for clip extraction.
- Tray/hotkey control can operate cleanly while the engine runs in the background.
- Clip save does not stall the capture thread severely.

### Explicitly out of scope for prototype

- Full settings UI
- Per-app profiles
- Pause/resume recording
- Multiple simultaneous output formats
- Stream Deck plugin
- Advanced remux workflows

### Prototype acceptance criteria

- Saves a valid 30-second MKV clip on hotkey press.
- Uses system audio plus microphone in the output.
- Remains stable for a 10-minute armed replay session.
- Logs capture timing, encoder queue depth, and clip-save latency.

## 7. Repository Structure

```text
/app
  /desktop-ui              # WinUI 3 + Win32 tray host
  /engine                  # Headless recording engine process

/libs
  /capture                 # Video capture abstractions and implementations
  /audio                   # WASAPI device capture, routing, resampling
  /encoding                # FFmpeg wrappers, encoder capability probing
  /mux                     # Container writing/remux logic
  /replay-buffer           # Encoded packet ring buffer and clip extraction
  /config                  # Schema, validation, migration
  /ipc                     # Named-pipe protocol and clients
  /platform-win            # Win32 helpers, COM/D3D utilities, crash support
  /logging                 # Structured logging facade
  /domain                  # State machine and command models

/plugins
  /stream-deck             # Elgato plugin project

/docs
  /architecture
  /benchmarks
  /adr
  /user

/tests
  /unit
  /integration
  /soak
  /fixtures

/scripts
  /dev
  /ci
  /packaging

/assets
  /icons
  /branding
  /stream-deck

/third_party
  /patches
  /prebuilt-notes

/tools
  /diagnostics
  /crashdump
```

### Structure rules

- App processes live under `/app`.
- Reusable core code lives under `/libs`.
- Stream Deck code is isolated under `/plugins`.
- No UI logic inside capture, audio, encoding, or replay-buffer libraries.

## 8. Configuration Schema

### Format recommendation

- **Use JSON for v1**.
- Why:
  - Easy validation, migration, logging, and plugin interoperability.
  - Familiar for Windows desktop apps and tool integrations.

### Config design rules

- Include a top-level schema version.
- Separate user preferences from runtime-discovered capabilities.
- Store stable device IDs, not only friendly names.
- Reject invalid settings atomically.

### Example `config.json`

```json
{
  "schema_version": 1,
  "app": {
    "start_minimized": true,
    "minimize_to_tray": true,
    "language": "en-US",
    "log_level": "info",
    "diagnostics_retention_days": 14
  },
  "capture": {
    "mode": "display",
    "target_display_id": "\\\\.\\DISPLAY1",
    "target_window": null,
    "capture_api": "windows_graphics_capture",
    "adapter_preference": "high_performance",
    "resolution": {
      "mode": "source"
    },
    "frame_rate": 60,
    "color_format": "nv12",
    "hdr_mode": "off"
  },
  "replay_buffer": {
    "enabled": true,
    "duration_seconds": 30,
    "memory_budget_mb": 512,
    "save_container": "mkv",
    "auto_remux_to_mp4": false,
    "clip_naming_pattern": "{date}_{time}_{game}_{duration}s_clip"
  },
  "recording": {
    "default_container": "mkv",
    "auto_remux_to_mp4": true,
    "split_files": false,
    "max_file_size_mb": 0,
    "pause_behavior": "timestamp_gap",
    "recording_naming_pattern": "{date}_{time}_{game}_recording"
  },
  "video_encoder": {
    "codec": "h264",
    "backend": "nvenc",
    "bitrate_kbps": 20000,
    "rate_control": "cbr",
    "preset": "p5",
    "profile": "high",
    "gop_seconds": 2,
    "b_frames": 2,
    "lookahead": false,
    "aq": true,
    "extra_ffmpeg_options": ""
  },
  "audio": {
    "sample_rate_hz": 48000,
    "channel_layout": "stereo",
    "tracks": [
      {
        "id": "system",
        "enabled": true,
        "source_type": "loopback",
        "device_id": "{0.0.0.00000000}.{11111111-1111-1111-1111-111111111111}",
        "codec": "aac",
        "bitrate_kbps": 192
      },
      {
        "id": "microphone",
        "enabled": true,
        "source_type": "capture",
        "device_id": "{0.0.1.00000000}.{22222222-2222-2222-2222-222222222222}",
        "codec": "aac",
        "bitrate_kbps": 160
      }
    ]
  },
  "output": {
    "clips_directory": "C:\\Users\\User\\Videos\\Clips",
    "recordings_directory": "C:\\Users\\User\\Videos\\Recordings",
    "temp_directory": "C:\\Users\\User\\AppData\\Local\\ClipApp\\Temp",
    "storage_cap_gb": 200,
    "auto_cleanup": true
  },
  "hotkeys": {
    "save_replay": "Ctrl+Shift+F8",
    "start_recording": "Ctrl+Shift+F9",
    "stop_recording": "Ctrl+Shift+F10",
    "pause_resume_recording": "Ctrl+Shift+F11"
  },
  "stream_deck": {
    "enabled": true,
    "ipc_transport": "named_pipe",
    "pipe_name": "\\\\.\\pipe\\clipapp.streamdeck",
    "request_timeout_ms": 1500,
    "allow_multiple_clients": true
  },
  "profiles": [
    {
      "match_type": "process_name",
      "match_value": "game.exe",
      "overrides": {
        "capture": {
          "frame_rate": 120
        },
        "video_encoder": {
          "bitrate_kbps": 35000
        },
        "replay_buffer": {
          "duration_seconds": 45
        }
      }
    }
  ]
}
```

### Ambiguities resolved safely

- `TASK.md` requests advanced FFmpeg options but does not define precedence rules. Safe default:
  - explicit structured fields win
  - raw `extra_ffmpeg_options` is appended last only for allowed encoder/mux options
- `TASK.md` requests multi-device/multi-channel audio but does not require fully arbitrary audio graph editing in v1. Safe default:
  - support multiple input tracks with a constrained routing model, not a full mixer UI

## 9. Development Rules for Future Coding

1. No large unreviewed rewrites. Change one subsystem boundary at a time.
2. Keep code modular. UI, IPC, capture, audio, encoding, and replay logic must remain isolated libraries.
3. Work in small implementation steps with explicit acceptance criteria per step.
4. Add structured logs around every capture, encoder, mux, and storage failure path.
5. Never block the UI/tray thread on capture, encode, disk I/O, or IPC retries.
6. Avoid unnecessary CPU/GPU copies. Treat GPU-resident frame flow as the default design constraint.
7. Prefer encoded-packet replay buffering over raw-frame buffering unless benchmarks prove otherwise.
8. Write tests where realistic:
   - pure unit tests for state/config logic
   - integration tests for packet flow and file output
   - soak tests for timing/drift and replay stability
9. Document every non-obvious Windows API choice in an ADR or nearby code comment with rationale.
10. Ask before changing architecture, process model, config schema shape, or IPC contract.
11. Introduce feature flags for risky paths such as alternative capture APIs or experimental remux behavior.
12. Do not expose encoder/UI options that capability probing cannot actually support on the current machine.
13. Treat crash recovery and file salvage as product features, not debugging afterthoughts.
14. Avoid speculative abstractions. Only generalize after the second real implementation pressure point.

## 10. Blocking Questions

No blocking questions were found for the planning phase.

### Assumptions locked for implementation planning

- Windows 11 only.
- x64 only for initial releases.
- v1 avoids invasive game injection/hooking and relies on OS-supported capture APIs.
- Default active-write container is MKV.
- Initial engine language is C++23.
- Initial Stream Deck transport is named pipes.
- Initial UI direction is WinUI 3 plus Win32 tray host, with Qt Widgets as fallback if Windows App SDK friction proves unacceptable during milestone work.

