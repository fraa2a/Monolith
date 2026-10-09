# Audit corrections implementation plan

**Goal:** Implement the user-approved modifications from the complete Monolith audit in one English pull request.

**Baseline:** main 2e8c4ec1616215344b968a5f09a004cf9df6d494.

**Design:** Preserve the native engine, Tauri sidecars, screenshot-based frontend and module boundaries. Keep media mutations in the engine. Use persistent catalog/clip identities, bounded owned workers, session-owned bookmarks, recoverable file mutations and authenticated updater metadata. Do not add unrelated features or heavy dependencies.

## Tasks

- [ ] Engine M01-M08, S04, F04: start order, common clock, WGC target, deferred reload, owned asynchronous writing/lifecycle, device recovery, disk session cleanup, visible failures and session bookmarks.
- [ ] Storage S01-S03, S06-S07, S10: validated paths, serialized recoverable trim, conservative reconciliation, active-writer exclusion and first-request deadline.
- [ ] Host S05, S08-S09, Q02-Q03: immutable membership identities, nondestructive reads, engine-only media writes, current-root checked media access, lightweight membership query.
- [ ] Frontend F01-F03, F05-F12: serialized settings and close flush, identity-based selection, editable-field shortcut guards, thumbnail/collection/favorite refresh, bounded probes, accessibility and fullscreen state. Expose audio-track API limits truthfully.
- [ ] Updater U01-U10: producer/consumer schema compatibility, signed component/version/digest/size, streaming verification, real process exit/readiness, stable recovery entry, cancellation and state ordering, build quoting and strict Stream Deck responses.
- [ ] Q01, Q04-Q05: bounded asynchronous runtime-rotated logs, focused responsibility extraction, current architecture/test documentation.
- [ ] Integration: native suites, Rust suite and Windows cross-check, frontend builds/browser tests, Stream Deck tests, PowerShell producer/consumer, text/whitespace checks.
- [ ] Independent review: fix concrete issues and rerun affected tests.
- [ ] Publish: durable branch checkpoints, one PR, verify CI. Keep Windows hardware/WebView2/clean-install gaps explicit.

## Review focus

Slow disk and shutdown; interrupted trim and competing mutations; settings save failures and out-of-order responses; catalog/root/clip identity changes; optional audio recovery; first upgrade from the old updater. Do not equate a portable passing suite with Windows runtime certification.
