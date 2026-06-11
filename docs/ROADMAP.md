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

Status: in progress.

- Goal:
  - Convert the replay-buffer proof into a safer recorder core and prepare manual recording.
- Deliverables:
  - recording state machine skeleton (implemented in the current app shell)
  - manual recording start/stop/pause/resume from tray and hotkeys (implemented; needs runtime verification)
  - graceful shutdown path for capture, encoders, and pending clip saves
  - clearer encoder/capture failure states in logs
  - basic replay-buffer test harness
- Acceptance criteria:
  - repeated save hotkey presses do not corrupt output or deadlock
  - app exits cleanly through tray without killing worker threads
  - recording commands have explicit accepted/rejected states (implemented)
- Next action:
  - verify the Settings foundation via CI build and runtime smoke test

## Milestone 5: Settings Foundation

Status: implemented in code; build/runtime verification pending.

- Goal:
  - Add the first persistent per-user settings path without redesigning replay or recording.
- Deliverables:
  - `AppData\Local\Monolith\config.json` load/save path
  - default config merge from `config/default-config.json`
  - minimal Win32 settings window opened from the tray
  - editable replay clip folder, manual recording folder, replay duration, and replay memory budget
  - read-only hotkey display
- Acceptance criteria:
  - settings opens from the tray
  - missing or invalid user config does not crash startup
  - output folder changes are saved and used for replay/manual recording
  - replay duration and memory budget are saved and applied to the replay buffer
  - unsupported hotkey rebinding is not presented as working
- Live-applied settings:
  - replay clip output folder
  - manual recording output folder
  - replay duration
  - replay memory budget
- Restart-required settings:
  - none exposed in this pass
- Not implemented:
  - hotkey rebinding
  - encoder/capture/audio device settings UI
  - Stream Deck settings
  - WinUI 3 settings shell
