# Manifest Notes

## Role

The Stream Deck plugin controls the native Monolith engine through local IPC.
It does not record, encode, inspect media files, or mutate databases directly.

## Transport

- Protocol: newline-delimited JSON-RPC.
- Host: `127.0.0.1`.
- Port: `45991`.
- Engine owner: `libs/ipc`.

Named pipes are still a deferred alternative in ADR-0007.

## Plugin Characteristics

- Technology: Node.js + TypeScript.
- SDK: official Elgato Stream Deck SDK v2.
- Node runtime: 20, per manifest.
- Minimum Stream Deck software: 6.4.
- Platform: Windows 10+.

## Actions

| Action | UUID | IPC |
|---|---|---|
| Save Replay | `top.fraa2a.monolith.save-replay` | `save_replay` |
| Toggle Recording | `top.fraa2a.monolith.recording-toggle` | `get_status`, then `recording_start` or `recording_stop` |
| Pause / Resume | `top.fraa2a.monolith.pause-resume` | `pause_resume`, then `get_status` |

## Status Poll

`plugin.ts` polls `get_status` every 5s and updates key titles for recording and
pause state. This is intentionally lightweight and eventually corrected by the
next poll after optimistic updates.

## Asset Expectations

The manifest references plugin/category/action images under `imgs/`. See
`top.fraa2a.monolith.sdPlugin/imgs/README.md`.

## TODO

- Verify packaged plugin in Stream Deck software.
- Replace placeholder/missing key images with final PNG assets.
- Add a visible disconnected state if UX testing shows alerts are insufficient.
