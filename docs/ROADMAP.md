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

## Milestone 6: Audio V2

Status: foundation implemented; mixer/runtime validation remains.

- Goal:
  - Add reliable default system+microphone output and a custom audio-routing model for up to six tracks.
- Deliverables:
  - Default mode: system audio on track 1, selected primary microphone on track 2. Implemented.
  - Custom mode: add/configure/disable/remove sources and assign each source to tracks 1-6. Implemented in Settings/config.
  - Multiple microphone/input devices as independent configurable sources. Implemented.
  - Per-process/application session listing with readable names and saved routing assignments. Implemented with Windows process-loopback best effort.
  - Best-effort Active Game source that never breaks recording if no game is detected. Implemented as foreground-process best effort.
  - Encoder/muxer support for the required audio tracks, with resampling/mixing as needed. Implemented for independent tracks; same-track multi-source mixing remains pending.
- Acceptance criteria:
  - replay save, manual recording, Settings launch, and output paths still work. Build and Settings launch smoke verified; runtime audio tests still needed.
  - missing devices/processes do not crash recording or corrupt config. Implemented by fail-closed source startup and saved-source availability display.
  - assignments outside tracks 1-6 are rejected or sanitized. Implemented.
  - current limitations of per-app capture, active-game detection, multi-track muxing, and A/V sync are documented. Implemented in handover/architecture.
