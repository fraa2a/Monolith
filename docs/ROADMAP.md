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

- Goal:
  - Prove the rolling-buffer design and save-last-X-seconds behavior.
- Deliverables:
  - encoded-packet or temporary packet ring buffer prototype
  - save replay trigger path
  - clip write flow prototype
- Acceptance criteria:
  - triggering replay save produces a valid test output path or artifact
  - memory behavior is measured and logged
  - basic timing boundaries are understood
- Next action:
  - decide whether the ring buffer stores encoded packets only or needs a temporary hybrid path during prototyping

