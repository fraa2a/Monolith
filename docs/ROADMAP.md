# Roadmap

## Milestone 0: Repo Foundation and Documentation

- Goal:
  - Establish the repo structure, baseline docs, and first config draft.
- Deliverables:
  - initial folder layout
  - handover document
  - architecture/rules/decisions docs
  - default config draft
- Acceptance criteria:
  - next session can begin implementation without re-planning the entire repo
  - no production capture/encoding code added yet
- Next action:
  - implement the minimal tray + hotkey prototype shell

## Milestone 1: Minimal Tray + Hotkey Prototype

- Goal:
  - Prove background app lifecycle, hidden window/message loop, tray icon ownership, and global hotkey registration.
- Deliverables:
  - hidden Win32 app shell
  - tray icon
  - context menu with placeholder actions
  - configurable hotkey parsing and registration stub
- Acceptance criteria:
  - app runs in tray
  - at least one hotkey triggers a logged placeholder action
  - no UI thread blocking
- Next action:
  - define the internal app state and command dispatch boundary

## Milestone 2: Video/Audio Capture Prototype

- Goal:
  - Prove Windows-native capture ingress for video and audio.
- Deliverables:
  - WGC display capture spike
  - WASAPI loopback spike
  - WASAPI microphone spike
  - timestamp/logging scaffolding
- Acceptance criteria:
  - capture callbacks run reliably
  - logs show stable frame/audio ingress over a short session
  - no encoding or replay buffer logic required yet
- Next action:
  - choose the first stable internal media packet shape

## Milestone 3: Replay Buffer Proof of Concept

Status: complete.

- Goal:
  - Prove the rolling-buffer design and save-last-X-seconds behavior.
- Deliverables:
  - encoded-packet ring buffer prototype
  - save replay trigger path
  - Matroska clip write flow prototype
- Acceptance criteria:
  - triggering replay save produces a valid MKV artifact
  - memory behavior is measured and logged
  - basic timing boundaries are understood
- Next action:
  - add replay-buffer test harness and begin runtime hardening / next product milestone

## Milestone 4: Runtime Hardening and Manual Recording Foundation

Status: complete for MVP; longer soak testing remains open.

- Goal:
  - Convert the replay-buffer proof into a safer recorder core and prepare manual recording.
- Deliverables:
  - recording state machine skeleton (implemented in the current app shell)
  - manual recording start/stop/pause/resume from tray and hotkeys
  - graceful shutdown path for capture, encoders, and pending clip saves
  - clearer encoder/capture failure states in logs
  - basic replay-buffer test harness
- Acceptance criteria:
  - repeated save hotkey presses do not corrupt output or deadlock
  - app exits cleanly through tray without killing worker threads
  - recording commands have explicit accepted/rejected states (implemented)
- Next action:
  - continue runtime hardening and Audio V2 mixer/runtime validation

## Milestone 5: Settings Foundation

Status: complete.

- Goal:
  - Add the first persistent per-user settings path without redesigning replay or recording.
- Deliverables:
  - `AppData\Local\Monolith\config.json` load/save path
  - default config merge from `config/default-config.json`
  - WinUI 3 settings sidecar opened from the tray
  - editable replay clip folder, manual recording folder, replay duration, replay memory budget, and manual recording format
  - read-only hotkey display
- Acceptance criteria:
  - settings opens from the tray (manually tested)
  - settings save and persist across restarts (manually tested)
  - missing or invalid user config does not crash startup
  - output folder changes are saved and used for replay/manual recording
  - replay duration and memory budget are saved and applied to the replay buffer
  - visible settings are wired, read-only, or honestly marked with their apply scope
  - Settings cold start avoids app-side runtime enumeration until the relevant page is opened
- Live-applied settings:
  - replay clip output folder
  - manual recording output folder
  - hotkeys
- Replay-buffer restart/reconfigure:
  - replay duration
  - replay memory budget
- Applies to next recording/clip:
  - manual recording format
- Capture/encoder restart when no manual recording is active:
  - capture monitor
  - output resolution
  - capture border
  - encoder backend
  - video bitrate
  - advanced FFmpeg options
- Audio pipeline restart when no manual recording is active:
  - audio mode
  - primary microphone
  - custom source routing/add/disable/remove
- Full app restart:
  - no currently exposed setting should require a full app restart by default
- Not implemented:
  - Stream Deck settings

## Milestone 6: Audio V2 (v0.5.1 update)

Status: foundation + dynamic Active Game detection implemented; mixer/runtime validation remains.

- Goal:
  - Add reliable default system+microphone output and a custom audio-routing model for up to six tracks.
- Deliverables:
  - Default mode: system audio on track 1, selected primary microphone on track 2. Implemented.
  - Custom mode: add/configure/disable/remove sources and assign each source to tracks 1-6. Implemented in Settings/config.
  - Multiple microphone/input devices as independent configurable sources. Implemented.
  - Per-process/application session listing with readable names and saved routing assignments. Implemented with Windows process-loopback best effort.
  - Best-effort Active Game source that never breaks recording if no game is detected. Implemented: dynamic detection with 30 s poll + foreground-change fast scan, debounce-based switching, blacklist/whitelist/manual-game config, process-loopback with fallback reporting.
  - Encoder/muxer support for the required audio tracks, with resampling/mixing as needed. Implemented for independent tracks; same-track multi-source mixing remains pending.
- v0.5.1 additions:
  - Active Game detection re-evaluates every poll tick instead of locking onto the first process.
  - Default poll interval changed to 30 s; foreground-change WinEvent hook provides sub-1-s detection latency.
  - Debounce (3 s default) prevents spurious switches on alt-tab.
  - Audio Mode labels simplified; Custom Sources hidden in Default mode.
  - Default → Custom confirmation dialog added.
  - Active Game runtime status panel shows detected game, confidence, signals, capture mode.
  - Runtime-status.json extended with confidence/reason/capture_mode/process_loopback_available/last_switch_time/poll_interval_ms/fast_scan_enabled.
- Acceptance criteria:
  - replay save, manual recording, Settings launch, and output paths still work. Build and Settings launch smoke verified; runtime audio tests still needed.
  - missing devices/processes do not crash recording or corrupt config. Implemented by fail-closed source startup and saved-source availability display.
  - assignments outside tracks 1-6 are rejected or sanitized. Implemented.
  - current limitations of per-app capture, active-game detection, multi-track muxing, and A/V sync are documented. Implemented in handover/architecture.

## Milestone 7: Distribution — Installer + Auto-Update

Status: implemented and verified locally; one-time release setup and first public release pending.

- Goal:
  - Ship Monolith to general users as a single per-user installer with zero prerequisites and working auto-update, while the code repo stays private.
- Deliverables:
  - version source of truth: git tag → CMake `MONOLITH_VERSION` → generated `version.h` (exe VERSIONINFO + updater) + csproj `Version`. Implemented.
  - self-contained Settings sidecar (.NET 8 + Windows App SDK bundled, no trimming). Implemented.
  - WinSparkle auto-update: `updater` module, tray "Check for Updates…", `update.auto_check` setting with Settings toggle, EdDSA-signed appcast. Implemented (public key pending one-time keygen).
  - per-user Inno Setup installer (`installer/monolith.iss`): no UAC, stable AppId, GPLv3 license page, full payload, user config preserved on uninstall/update. Implemented and compiled locally.
  - appcast generation script (`scripts/generate-appcast.ps1`) with Ed25519 signing via openssl. Implemented and tested.
  - release CI (`version-tag.yml`): tag → versioned build → installer → signed appcast → GPLv3 source zip → publish to public `fraa2a/Monolith-releases`. Implemented.
  - one-time setup + release process documented in `docs/RELEASING.md`. Implemented.
- Acceptance criteria:
  - pushing tag `vX.Y.Z` produces a public release with installer, appcast, and source zip downloadable without authentication. Pending (needs one-time setup).
  - installer works per-user without admin and the app runs on a machine without .NET/WinAppSDK. Local install verified; clean-VM test pending.
  - an older installed version detects, downloads, verifies, and applies the new release; user config survives. Pending first public release.
  - vcpkg dependencies pinned for reproducible release builds. Implemented (`builtin-baseline`).
- Next action:
  - complete the one-time setup in `docs/RELEASING.md`, then publish and verify the first public release.
