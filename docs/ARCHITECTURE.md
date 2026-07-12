# Architecture

## Product Shape

Monolith = Windows-first record+clip app. Recorder-first, not broadcaster-first. Custom build, not OBS fork.

Product now got:

- Native recorder/tray engine: `Monolith.exe`.
- Tauri/WebView2 desktop UI sidecar: `Monolith.UI.exe`.
- Stream Deck remote-control plugin.
- Per-user installer + WinSparkle updater.

## Process Model

### Current: single recording process plus UI sidecar

`app/recorder` = single record process. Own:

- Win32 tray + message-only window.
- Single-instance guard.
- Global hotkeys.
- Capture, audio, encode, replay, manual record.
- Clip catalog writes.
- Settings reload + runtime status publish.
- Local JSON-RPC server.
- WinSparkle update checks.

`app/desktop-ui` = sidecar UI process. No record. Read clip catalogs read-only, write settings, serve embedded frontend, send commands/mutations to engine over JSON-RPC.

### Future: headless engine split

Long-term target = separate headless record engine process + UI/tray process in front. Split deferred til MVP stable, dodge early IPC mess.

## Modules

- `app/recorder`: orchestrator, tray, hotkeys, app state, settings reload, updater, runtime status, command dispatch.
- `app/desktop-ui`: Tauri v2/WebView2 host + Preact frontend. Rust modules:
  `server.rs`, `media.rs`, `settings_store.rs`, `clip_catalog.rs`,
  `engine_rpc.rs`, `game_catalog.rs`, `paths.rs`.
- `libs/capture`: Windows.Graphics.Capture display capture over D3D11.
- `libs/audio`: WASAPI loopback, mic/input device capture, process-loopback, active-game detect.
- `libs/encoding`: FFmpeg H.264/H.265/AAC wrappers, thumbnail gen, mux helpers, `TrackMixer`.
- `libs/replay-buffer`: encoded-packet ring buffer + keyframe-safe clip save.
- `libs/recording`: manual record state + live writer.
- `libs/storage`: SQLite clip catalogs + `settings.db` key/value store.
- `libs/ipc`: newline-delimited JSON-RPC TCP server on `127.0.0.1:45991`.
- `plugins/stream-deck`: Elgato SDK TypeScript plugin.

## Data Flow

Video path now:

```text
Windows.Graphics.Capture frame
  -> D3D11 texture
  -> CPU-readable staging texture
  -> BGRA pacer buffer
  -> FFmpeg video encoder
  -> encoded video packets
  -> replay buffer and manual recorder
```

Audio path now:

```text
WASAPI source
  -> direct encoder route or TrackMixer
  -> AAC encoder
  -> encoded audio packets
  -> replay buffer and manual recorder
```

Data cross replay/record boundary = encoded packets, not raw frames.

## Settings

Store of record: `%LocalAppData%\Monolith\settings.db`.

`config/default-config.json` = seed schema + compiled fallback. Legacy `config.json` imported once on first run when DB empty, then renamed to `config.json.imported.bak`.

Settings apply scopes:

- Output folders + hotkeys: live for future ops.
- Replay duration: restart/reconfig replay buffer.
- Replay memory budget: fixed internal at 512 MB.
- Audio/capture/encoder change: restart pipelines only when no manual record active.
- Unsafe change during manual record: deferred.

UI write settings thru its Rust host, call `reload_settings` over engine IPC.

## Clip Catalogs

Each output folder self-contained:

- Replay folder: `clips.db` + `.thumbs\`.
- Manual record folder: `recs.db` + `.thumbs\`.

Engine = single writer. UI open catalogs read-only, send all mutations to engine:

- favorite
- hashtag add/remove
- filename rename
- display title set
- thumbnail regenerate
- delete

`get_status.clip_generation` let UI refresh after save, no restart.

## Active Game And Audio

Default audio mode: record desktop audio to track 1, selected mic to track 2 when got.

Custom mode can route desktop, input devices, process sessions, Active Game to up to six logical tracks. Tracks with 2+ enabled sources use `TrackMixer`; single-source tracks feed encoder direct with gain applied.

Active Game detect = best-effort. Use fullscreen/foreground/audio signals + configured process lists. Engine poll on fixed 5s cadence + also do foreground-change fast scans. Process-loopback can fail closed, no break default record.

## UI Architecture

UI use system WebView2 window thru Tauri v2. This exception apply only to `app/desktop-ui`; record engine stay native, no embed browser runtime.

Tauri host run loopback HTTP server, navigate WebView to `http://127.0.0.1:<port>/`. Keep normal frontend `fetch()`, media, thumbnail, SSE routes, no rewrite components to Tauri `invoke()`.

## Runtime Paths

- Videos: `Videos\Monolith\Clips`, `Videos\Monolith\Recordings`.
- App data: `%LocalAppData%\Monolith`.
- Settings: `%LocalAppData%\Monolith\settings.db`.
- Runtime status: `%LocalAppData%\Monolith\runtime-status.json`.
- Logs: `%LocalAppData%\Monolith\monolith.log`.
- Install path: `%LocalAppData%\Programs\Monolith`.

## Performance Direction

Current high-cost area:

```text
GPU texture -> CPU staging -> BGRA vector -> sws_scale/color conversion
```

Priority perf work:

1. GPU downscale before CPU readback.
2. Output-sized staging texture.
3. Explicit color range/matrix in CPU path.
4. D3D11/NV12 hardware encoder path.
5. AVPacket lifetime/ref-counting improve in mux path.