# Development Rules

1. No Electron.
2. No browser shell in the capture/recording engine. `app/desktop-ui` may use
   Tauri v2/WebView2 as the UI sidecar only. `app/recorder` and `libs/*` must
   stay native.
3. No large unreviewed rewrites.
4. Keep modules isolated: capture, audio, encoding, replay, recording, storage,
   config/settings, hotkeys, tray, IPC, UI, plugin.
5. Do not block the tray/UI message loop on capture, encode, mux, disk I/O,
   thumbnail generation, update checks, settings I/O, or IPC.
6. Prefer encoded-packet buffering over raw-frame buffering.
7. Avoid unnecessary CPU/GPU copies. GPU-resident frame flow is the long-term
   target.
8. Log capture, device, encoder, mux, settings, catalog, updater, and IPC
   failures with enough context to diagnose user machines.
9. Document non-obvious Windows API choices in an ADR or nearby source comment.
10. Do not change process model, settings schema, IPC contract, catalog schema,
    or updater/release flow without updating `docs/DECISIONS.md` and
    `docs/handover/ACTIVE_HANDOVER.md`.
11. Prefer small, narrowly scoped commits.
12. Do not introduce heavy dependencies until the owning module has a concrete
    need.
13. Do not introduce broad abstractions before a real module needs them.
14. The UI may read clip catalogs, but clip mutations must go through the engine
    so the engine remains the single writer.
15. `settings.db` is the settings store of record. `config/default-config.json`
    is only the seed/fallback; legacy `config.json` is migration input only.
16. Update `docs/handover/ACTIVE_HANDOVER.md` at the end of every substantial
    session.
