# Manifest Notes

Planned plugin characteristics:

- Technology: Node.js + TypeScript
- SDK: official Elgato Stream Deck SDK
- Transport to native app: local IPC
- Preferred initial IPC shape: localhost TCP for config draft compatibility, with named pipes still the preferred native target to evaluate during implementation

Planned actions:

- `save_replay`
- `start_recording`
- `stop_recording`
- `pause_resume_recording`

TODO:

- Finalize action UUIDs
- Define IPC request/response schema
- Create plugin scaffold only after native IPC contract is stable

