# Development Rules

1. No Electron.
2. No browser shell in capture/recording engine. `app/desktop-ui` may use
   Tauri v2/WebView2 as UI sidecar only. `app/recorder` and `libs/*` stay
   native.
3. No big unreviewed rewrites.
4. Keep modules isolated: capture, audio, encoding, replay, recording, storage,
   config/settings, hotkeys, tray, IPC, UI, plugin.
5. No block tray/UI message loop on capture, encode, mux, disk I/O,
   thumbnail gen, update checks, settings I/O, IPC.
6. Prefer encoded-packet buffering over raw-frame buffering.
7. Avoid needless CPU/GPU copies. GPU-resident frame flow = long-term goal.
8. Log capture, device, encoder, mux, settings, catalog, updater, IPC
   failures with enough context to diagnose user machines.
9. Document non-obvious Windows API choices in ADR or nearby source comment.
10. No change to process model, settings schema, IPC contract, catalog schema,
    or updater/release flow without updating `docs/DECISIONS.md` and
    `docs/handover/ACTIVE_HANDOVER.md`.
11. Prefer small, narrow commits.
12. No heavy deps until owning module has concrete need.
13. No broad abstractions before real module needs them.
14. UI may read clip catalogs, but clip mutations must go through engine
    so engine stays single writer.
15. `settings.db` = settings store of record. `config/default-config.json`
    = seed/fallback only; legacy `config.json` = migration input only.
16. Update `docs/handover/ACTIVE_HANDOVER.md` at end of every substantial
    session.