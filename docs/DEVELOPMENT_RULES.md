# Development Rules

1. No Electron.
2. No WebView, WebView2, Chromium Embedded Framework, or browser-shell architecture.
3. No large unreviewed rewrites.
4. Keep modules isolated:
   - capture
   - audio
   - encoder
   - replay
   - recording
   - hotkeys
   - tray
   - config
   - ipc
5. Avoid blocking the UI thread under all normal capture and recording flows.
6. Avoid unnecessary CPU/GPU copies; prefer GPU-resident frame flow where possible.
7. Log every capture failure, encoder failure, muxing failure, device failure, and IPC failure.
8. Document every non-obvious Windows API decision in docs or an ADR before or during implementation.
9. Prefer small incremental commits and narrowly scoped changes.
10. Do not change architecture direction without updating `docs/DECISIONS.md` and `docs/handover/ACTIVE_HANDOVER.md`.
11. Do not introduce heavy dependencies until the specific module requiring them is being implemented.
12. Do not implement broad abstractions before a real module needs them.
13. Update `docs/handover/ACTIVE_HANDOVER.md` at the end of every session.

