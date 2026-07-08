# Architecture

## Product Shape

Monolith is a Windows-first recording and clipping app. It is recorder-first,
not broadcaster-first, and remains a custom implementation rather than an OBS
fork.

The current product consists of:

- Native recorder/tray engine: `Monolith.exe`.
- Tauri/WebView2 desktop UI sidecar: `Monolith.UI.exe`.
- Stream Deck remote-control plugin.
- Per-user installer and WinSparkle updater.

## Process Model

### Current: single recording process plus UI sidecar

`app/recorder` is the single recording process. It owns:

- Win32 tray and message-only window.
- Single-instance guard.
- Global hotkeys.
- Capture, audio, encode, replay, manual recording.
- Clip catalog writes.
- Settings reload and runtime status publishing.
- Local JSON-RPC server.
- WinSparkle update checks.

`app/desktop-ui` is a sidecar UI process. It does not record. It reads clip
catalogs read-only, writes settings, serves the embedded frontend, and sends
commands/mutations to the engine over JSON-RPC.

### Future: headless engine split

The long-term target remains a separate headless recording engine process with a
UI/tray process in front of it. The split is deferred until the MVP is stable to
avoid early IPC complexity.

## Modules

- `app/recorder`: orchestrator, tray, hotkeys, app state, settings reload,
  updater, runtime status, and command dispatch.
- `app/desktop-ui`: Tauri v2/WebView2 host plus Preact frontend. Rust modules:
  `server.rs`, `media.rs`, `settings_store.rs`, `clip_catalog.rs`,
  `engine_rpc.rs`, `game_catalog.rs`, `paths.rs`.
- `libs/capture`: Windows.Graphics.Capture display capture over D3D11.
- `libs/audio`: WASAPI loopback, microphone/input device capture,
  process-loopback, active-game detection.
- `libs/encoding`: FFmpeg H.264/H.265/AAC wrappers, thumbnail generation,
  mux helpers, and `TrackMixer`.
- `libs/replay-buffer`: encoded-packet ring buffer and keyframe-safe clip save.
- `libs/recording`: manual recording state and live writer.
- `libs/storage`: SQLite clip catalogs and `settings.db` key/value storage.
- `libs/ipc`: newline-delimited JSON-RPC TCP server on `127.0.0.1:45991`.
- `plugins/stream-deck`: Elgato SDK TypeScript plugin.

## Data Flow

Video path today:

```text
Windows.Graphics.Capture frame
  -> D3D11 texture
  -> CPU-readable staging texture
  -> BGRA pacer buffer
  -> FFmpeg video encoder
  -> encoded video packets
  -> replay buffer and manual recorder
```

Audio path today:

```text
WASAPI source
  -> direct encoder route or TrackMixer
  -> AAC encoder
  -> encoded audio packets
  -> replay buffer and manual recorder
```

Data crossing replay/recording boundaries is encoded packets, not raw frames.

## Settings

Store of record: `%LocalAppData%\Monolith\settings.db`.

`config/default-config.json` is the seed schema and compiled fallback. Legacy
`config.json` is imported once on first run when the DB is empty, then renamed
to `config.json.imported.bak`.

Settings apply scopes:

- Output folders and hotkeys: live for future operations.
- Replay duration: restarts/reconfigures replay buffer.
- Replay memory budget: fixed internally at 512 MB.
- Audio/capture/encoder changes: restart pipelines only when no manual
  recording is active.
- Unsafe changes during manual recording: deferred.

The UI writes settings through its Rust host and calls `reload_settings` over
engine IPC.

## Clip Catalogs

Each output folder is self-contained:

- Replay folder: `clips.db` plus `.thumbs\`.
- Manual recording folder: `recs.db` plus `.thumbs\`.

The engine is the single writer. The UI opens catalogs read-only and sends all
mutations to the engine:

- favorite
- hashtag add/remove
- filename rename
- display title set
- thumbnail regenerate
- delete

`get_status.clip_generation` lets the UI refresh after saves without restart.

## Active Game And Audio

Default audio mode records desktop audio to track 1 and the selected microphone
to track 2 when available.

Custom mode can route desktop, input devices, process sessions, and Active Game
to up to six logical tracks. Tracks with two or more enabled sources use
`TrackMixer`; single-source tracks feed an encoder directly with gain applied.

Active Game detection is best-effort. It uses fullscreen/foreground/audio
signals plus configured process lists. The engine polls on a fixed 5s cadence
and also performs foreground-change fast scans. Process-loopback can fail closed
without breaking default recording.

## UI Architecture

The UI uses a system WebView2 window through Tauri v2. This exception applies
only to `app/desktop-ui`; the recording engine remains native and does not embed
a browser runtime.

The Tauri host runs a loopback HTTP server and navigates the WebView to
`http://127.0.0.1:<port>/`. This preserves normal frontend `fetch()`, media,
thumbnail, and SSE routes without rewriting components to Tauri `invoke()`.

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

Priority performance work:

1. GPU downscale before CPU readback.
2. Output-sized staging texture.
3. Explicit color range/matrix in CPU path.
4. D3D11/NV12 hardware encoder path.
5. AVPacket lifetime/ref-counting improvements in mux path.
