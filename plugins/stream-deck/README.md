# Stream Deck Plugin

Elgato Stream Deck plugin for controlling Monolith over local JSON-RPC.

The plugin is a remote controller only. It contains no recording logic and never
writes clip catalogs or settings.

## Status

Implemented scaffold:

- TypeScript plugin entrypoint.
- Shared TCP JSON-RPC client.
- Save Replay action.
- Toggle Recording action.
- Pause/Resume action.
- 5s status polling for key title updates.
- Package script for `.streamDeckPlugin` output.

Still needs runtime verification with Stream Deck software and final image
assets.

## Build

```powershell
npm install
npm run build
```

Package:

```powershell
npm run package
```

Link during local development:

```powershell
npm run link
```

## IPC

Host: `127.0.0.1`

Port: `45991`

Transport: newline-delimited JSON-RPC over TCP.

Methods used:

- `save_replay`
- `recording_start`
- `recording_stop`
- `pause_resume`
- `get_status`

The plugin reconnects every 2s when the engine is unavailable. Requests time out
after 3s.

## Actions

- `top.fraa2a.monolith.save-replay`
- `top.fraa2a.monolith.recording-toggle`
- `top.fraa2a.monolith.pause-resume`

## Boundaries

- Do not add capture/recording code here.
- Do not write `settings.db`, `clips.db`, or `recs.db` from the plugin.
- New commands must be added to the engine IPC contract first.
