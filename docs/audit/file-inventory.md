# File-by-file audit inventory

Snapshot after technical changes and before the comment-only branch. 239 files are accounted for. SHA256 prefixes identify reviewed content; they change with subsequent comments. Inventory inclusion is not a claim that every path or failure mode has been executed. Binary assets, generated schemas and dependency locks receive metadata review. The inventory file itself is excluded to avoid a self-referential hash.

See [audit findings, validation and unresolved work](2026-10-08.md).

| File | Review type | Bytes | SHA256 prefix | Observation |
| --- | --- | ---: | --- | --- |
| `.cargo/config.toml` | Configuration review | 81 | `53329a3ce3b8` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `.github/workflows/core-tests.yml` | Configuration review | 1842 | `5b4f706ea6a8` | Portable native/Rust/plugin/frontend regressions on all PR branches. |
| `.github/workflows/version-tag.yml` | Configuration review | 14250 | `28f8a39ab2c6` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `.github/workflows/windows-ci.yml` | Configuration review | 6462 | `b922c37fa08c` | Runtime/text verifier plus complete Windows build on all PR branches. |
| `.gitignore` | Configuration review | 1175 | `a01da3edebe1` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `AGENTS.md` | Documentation | 907 | `7db4d1094db6` | Requirements/history/architecture cross-check; historical claims are not re-certified by this audit. |
| `CLAUDE.md` | Documentation | 5889 | `186f370617a9` | Requirements/history/architecture cross-check; historical claims are not re-certified by this audit. |
| `CMakeLists.txt` | Configuration review | 2077 | `69c9cf59f18f` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `LICENSE` | Configuration review | 35148 | `1c14cc78a086` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `app/assets/exeicon.ico` | Binary metadata | 36959 | `9b6a8ad2ef0f` | Tracked asset, size/type/reference inventory; no source or image-content assurance. |
| `app/assets/exeicon.png` | Binary metadata | 3240110 | `010b821deb5e` | Tracked asset, size/type/reference inventory; no source or image-content assurance. |
| `app/assets/icon.ico` | Binary metadata | 6758 | `86a350fea7a5` | Tracked asset, size/type/reference inventory; no source or image-content assurance. |
| `app/assets/icon.png` | Binary metadata | 263787 | `366936932c73` | Tracked asset, size/type/reference inventory; no source or image-content assurance. |
| `app/desktop-ui/.gitignore` | Configuration review | 245 | `ac0cac065088` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `app/desktop-ui/CMakeLists.txt` | Configuration review | 2588 | `9449856f9aee` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `app/desktop-ui/PRODUCT.md` | Documentation | 2475 | `9690174b036a` | Requirements/history/architecture cross-check; historical claims are not re-certified by this audit. |
| `app/desktop-ui/README.md` | Documentation | 3477 | `3edd2d82dc55` | Requirements/history/architecture cross-check; historical claims are not re-certified by this audit. |
| `app/desktop-ui/package-lock.json` | Dependency metadata | 78331 | `1c588c6ea009` | Lockfile/version consistency and locked-build use checked; no exhaustive dependency CVE audit. |
| `app/desktop-ui/package.json` | Configuration review | 663 | `8e46a5a148d9` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `app/desktop-ui/src-tauri/Cargo.lock` | Dependency metadata | 129289 | `4a0bf62b9410` | Lockfile/version consistency and locked-build use checked; no exhaustive dependency CVE audit. |
| `app/desktop-ui/src-tauri/Cargo.toml` | Configuration review | 731 | `de1624dba27d` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `app/desktop-ui/src-tauri/build.rs` | Source review | 40 | `6aeeab41bc35` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/desktop-ui/src-tauri/capabilities/default.json` | Configuration review | 463 | `0d91e0486d5c` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `app/desktop-ui/src-tauri/gen/schemas/acl-manifests.json` | Generated metadata | 65934 | `b8665c2471bd` | Generated Tauri schema; source capability/config is authoritative; not independently edited. |
| `app/desktop-ui/src-tauri/gen/schemas/capabilities.json` | Generated metadata | 308 | `22b043a59cea` | Generated Tauri schema; source capability/config is authoritative; not independently edited. |
| `app/desktop-ui/src-tauri/gen/schemas/desktop-schema.json` | Generated metadata | 116049 | `f68a9c570ecf` | Generated Tauri schema; source capability/config is authoritative; not independently edited. |
| `app/desktop-ui/src-tauri/gen/schemas/linux-schema.json` | Generated metadata | 116049 | `f68a9c570ecf` | Generated Tauri schema; source capability/config is authoritative; not independently edited. |
| `app/desktop-ui/src-tauri/gen/schemas/windows-schema.json` | Generated metadata | 116049 | `f68a9c570ecf` | Generated Tauri schema; source capability/config is authoritative; not independently edited. |
| `app/desktop-ui/src-tauri/icons/icon.png` | Binary metadata | 263787 | `366936932c73` | Tracked asset, size/type/reference inventory; no source or image-content assurance. |
| `app/desktop-ui/src-tauri/src/asset_scope.rs` | Source review | 721 | `fb5bd9ddc252` | Directory permission changes inspected; obsolete roots are retained, O06. |
| `app/desktop-ui/src-tauri/src/clip_catalog.rs` | Source review | 22189 | `cede95166fdd` | A14-A15: batched hydration, failures and transaction sequence allocation; direct writes remain O04. |
| `app/desktop-ui/src-tauri/src/collections.rs` | Source review | 8356 | `bc168cabc087` | A14-A15: preserve membership on catalog failure and transactional pruning/cascade. |
| `app/desktop-ui/src-tauri/src/commands.rs` | Source review | 20049 | `d602ed7721b3` | Delete/rename routed to engine; remaining direct metadata/thumbnail writes violate O04. |
| `app/desktop-ui/src-tauri/src/engine_rpc.rs` | Source review | 4760 | `5ca8b4102d28` | A18: bounded response, request IDs, envelope checks, per-operation timeouts; O09 remains. |
| `app/desktop-ui/src-tauri/src/exe_icon.rs` | Source review | 5604 | `80432c69002c` | Win32 icon/GDI handle cleanup and conversion inspected; actual extraction untested. |
| `app/desktop-ui/src-tauri/src/game_catalog.rs` | Source review | 13772 | `80317b1202c7` | A16: migration/index order, writable refresh and persistence errors. |
| `app/desktop-ui/src-tauri/src/main.rs` | Source review | 4690 | `faff379e17de` | Native command registration, event watcher and background catalog startup inspected; UI host runtime requires Windows. |
| `app/desktop-ui/src-tauri/src/paths.rs` | Source review | 835 | `ed715cc2127b` | Default/dev/override path resolution inspected; environment/installed-layout runtime untested. |
| `app/desktop-ui/src-tauri/src/settings_store.rs` | Source review | 4256 | `2f306fd2559d` | A17: serialized directory cache invalidation including WAL changes. |
| `app/desktop-ui/src-tauri/tauri.conf.json` | Configuration review | 780 | `67dc8f7c5e1b` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `app/desktop-ui/tsconfig.json` | Configuration review | 301 | `c3d2136219f5` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `app/desktop-ui/ui/index.html` | Configuration review | 637 | `aaebe08b00df` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `app/desktop-ui/ui/src/app.tsx` | Source review | 11526 | `0808e1d4322b` | A19: load generations and visible deletion failures; frontend build passes. |
| `app/desktop-ui/ui/src/home/clip-card.tsx` | Source review | 10907 | `4a52652ad21b` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/desktop-ui/ui/src/home/collection-detail.tsx` | Source review | 10864 | `53e355e9b914` | A19: stale response/unmount guard; some action feedback remains O12. |
| `app/desktop-ui/ui/src/home/collection-picker.tsx` | Source review | 6168 | `4c334160788e` | Membership lookup loads whole collections, O12; loading/action state inspected. |
| `app/desktop-ui/ui/src/home/collections-view.tsx` | Source review | 7551 | `acc31f4d7231` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/desktop-ui/ui/src/home/confirm-dialog.tsx` | Source review | 1253 | `50d68fc9814f` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/desktop-ui/ui/src/home/context-menu.tsx` | Source review | 2319 | `616ca42c1e24` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/desktop-ui/ui/src/home/detail-view.tsx` | Source review | 27093 | `916dd2611daa` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/desktop-ui/ui/src/home/filter-menu.tsx` | Source review | 3423 | `6efa33ded081` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/desktop-ui/ui/src/home/filters.tsx` | Source review | 1486 | `6819c2bf02a9` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/desktop-ui/ui/src/home/fullscreen.tsx` | Source review | 8305 | `ecc24ec3acaa` | Native capabilities corrected in packaging PR; prior maximized state restoration remains O12. |
| `app/desktop-ui/ui/src/home/hashtag-dialog.tsx` | Source review | 3661 | `42d8ed3b26bb` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/desktop-ui/ui/src/lib/api.ts` | Source review | 10045 | `aa8872eda63c` | A21: bounded/retryable icon requests and visible subscription errors; IPC names inspected. |
| `app/desktop-ui/ui/src/lib/format.ts` | Source review | 3764 | `5210dd2ee6b6` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/desktop-ui/ui/src/lib/multitrack.ts` | Source review | 6378 | `09118860d11f` | A20: initial state/events/cleanup; actual WebView2 multitrack playback gate. |
| `app/desktop-ui/ui/src/lib/player.ts` | Source review | 725 | `af0b0e289df1` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/desktop-ui/ui/src/lib/settings-api.ts` | Source review | 2754 | `7e4fca860455` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/desktop-ui/ui/src/lib/window.ts` | Source review | 582 | `3167c98ae86c` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/desktop-ui/ui/src/main.tsx` | Source review | 145 | `b35076b88cca` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/desktop-ui/ui/src/settings/audio-settings.tsx` | Source review | 9579 | `a95c6d862554` | Track/source baseline and UI-to-config routing inspected; hardware execution gate. |
| `app/desktop-ui/ui/src/settings/controls.tsx` | Source review | 8176 | `1e15fea7f1b6` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/desktop-ui/ui/src/settings/settings-popup.tsx` | Source review | 26010 | `7aac5c84cd90` | Typed frontend build; debounced-close/concurrent save and completion-state issues remain O07. |
| `app/desktop-ui/ui/src/shell/icons.tsx` | Source review | 8014 | `371f22d80b14` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/desktop-ui/ui/src/shell/sidebar.tsx` | Source review | 1690 | `885158a4e44d` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/desktop-ui/ui/src/shell/titlebar.tsx` | Source review | 14560 | `8739564b9bea` | A19: guarded saves and rollback/error feedback; whole-config concurrency remains O07. |
| `app/desktop-ui/ui/styles.css` | Configuration review | 53496 | `df381d8a2813` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `app/desktop-ui/vite.config.ts` | Source review | 478 | `8e3962f4204a` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/engine/README.md` | Documentation | 231 | `dd91ae9a32e0` | Requirements/history/architecture cross-check; historical claims are not re-certified by this audit. |
| `app/recorder/CMakeLists.txt` | Configuration review | 2467 | `9e27109cff73` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `app/recorder/app/.gitkeep` | Scaffold | 1 | `01ba4719c80b` | Empty placeholder; retained pending cohesive module cleanup, O13. |
| `app/recorder/hotkeys/.gitkeep` | Scaffold | 1 | `01ba4719c80b` | Empty placeholder; retained pending cohesive module cleanup, O13. |
| `app/recorder/include/.gitkeep` | Scaffold | 1 | `01ba4719c80b` | Empty placeholder; retained pending cohesive module cleanup, O13. |
| `app/recorder/monolith.rc` | Configuration review | 1315 | `67832b2d5b18` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `app/recorder/recording/.gitkeep` | Scaffold | 1 | `01ba4719c80b` | Empty placeholder; retained pending cohesive module cleanup, O13. |
| `app/recorder/resources/.gitkeep` | Scaffold | 1 | `01ba4719c80b` | Empty placeholder; retained pending cohesive module cleanup, O13. |
| `app/recorder/src/feedback_sounds.h` | Source review | 4039 | `43eee00bf6eb` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/recorder/src/main.cpp` | Source review | 149309 | `1f249266d20e` | A11: pacer/capture settings/status writes; tray control/bookmarks/catalog boundaries inspected; owned workers and writer backpressure remain O03. |
| `app/recorder/src/replay_disk_settings.h` | Source review | 851 | `2ce813fb1844` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/recorder/src/resource.h` | Source review | 106 | `fe5859ceed1b` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/recorder/src/settings_config.cpp` | Source review | 32027 | `66511cb12ecf` | Defaults, SQLite config and input validation inspected; UI save concurrency remains O07. |
| `app/recorder/src/settings_config.h` | Source review | 7918 | `648663a40775` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/recorder/src/settings_window.cpp` | Source review | 6856 | `5698353ed86e` | A12: serialized sidecar handle replacement; process lifetime requires Windows execution. |
| `app/recorder/src/settings_window.h` | Source review | 267 | `68d29976a04a` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/recorder/src/updater.cpp` | Source review | 3930 | `c939ad50c846` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/recorder/src/updater.h` | Source review | 1409 | `7e3891d026c0` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/recorder/src/version.h.in` | Configuration review | 588 | `fbe1e2014d94` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `app/recorder/tray/.gitkeep` | Scaffold | 1 | `01ba4719c80b` | Empty placeholder; retained pending cohesive module cleanup, O13. |
| `app/updater/.gitignore` | Configuration review | 150 | `d25b24d3691b` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `app/updater/CMakeLists.txt` | Configuration review | 2485 | `a6d60f9b73cd` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `app/updater/package-lock.json` | Dependency metadata | 79107 | `f4d8ed5c8d32` | Lockfile/version consistency and locked-build use checked; no exhaustive dependency CVE audit. |
| `app/updater/package.json` | Configuration review | 666 | `f138f7b3f7c9` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `app/updater/src-tauri/Cargo.lock` | Dependency metadata | 127724 | `18463253a34c` | Lockfile/version consistency and locked-build use checked; no exhaustive dependency CVE audit. |
| `app/updater/src-tauri/Cargo.toml` | Configuration review | 799 | `37d074ee5a2e` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `app/updater/src-tauri/build.rs` | Source review | 40 | `6aeeab41bc35` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/updater/src-tauri/capabilities/default.json` | Configuration review | 323 | `0f532b070a4c` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `app/updater/src-tauri/gen/schemas/acl-manifests.json` | Generated metadata | 69499 | `4d93885b4645` | Generated Tauri schema; source capability/config is authoritative; not independently edited. |
| `app/updater/src-tauri/gen/schemas/capabilities.json` | Generated metadata | 251 | `da5f373eddbc` | Generated Tauri schema; source capability/config is authoritative; not independently edited. |
| `app/updater/src-tauri/gen/schemas/desktop-schema.json` | Generated metadata | 129129 | `623c82e1cf0b` | Generated Tauri schema; source capability/config is authoritative; not independently edited. |
| `app/updater/src-tauri/gen/schemas/windows-schema.json` | Generated metadata | 129129 | `623c82e1cf0b` | Generated Tauri schema; source capability/config is authoritative; not independently edited. |
| `app/updater/src-tauri/icons/icon.png` | Binary metadata | 263787 | `366936932c73` | Tracked asset, size/type/reference inventory; no source or image-content assurance. |
| `app/updater/src-tauri/src/apply.rs` | Source review | 4331 | `dd1f6e414f71` | A25: per-file predecessor restoration; component rollback and Windows fault matrix remain O01. |
| `app/updater/src-tauri/src/archive.rs` | Source review | 2496 | `fe574bc97240` | A25: traversal/aliases/links/limits with production ZIP extraction regression. |
| `app/updater/src-tauri/src/download.rs` | Source review | 3985 | `24156f81c285` | A23: mandatory integrity/signatures and forged-signature regression; memory remains O08. |
| `app/updater/src-tauri/src/engine_rpc.rs` | Source review | 3783 | `a4966e54abed` | A18: bounded response, request IDs, envelope checks, per-operation timeouts; O09 remains. |
| `app/updater/src-tauri/src/http.rs` | Source review | 7912 | `c94ac6448f6b` | A24: WinHTTP string termination, query, HTTPS and size bounds; Windows-target check only. |
| `app/updater/src-tauri/src/main.rs` | Source review | 15856 | `e92ff99a2fe6` | Download/apply/cancel/restart/version flow inspected; O01/O02 remain, Windows Tauri runtime gate. |
| `app/updater/src-tauri/src/manifest.rs` | Source review | 3000 | `047a6d85a6f2` | A23: schema/key/version/size/HTTPS/hash/signature validation with invalid-input tests. |
| `app/updater/src-tauri/src/paths.rs` | Source review | 1430 | `71b25316f94c` | Default/dev/override path resolution inspected; environment/installed-layout runtime untested. |
| `app/updater/src-tauri/src/state.rs` | Source review | 3686 | `19e151cc74b7` | Serialized updater state/enums and public frontend shape inspected. |
| `app/updater/src-tauri/src/versions.rs` | Source review | 3678 | `221c5c65f478` | VERSIONINFO size/signature inspected and Windows-target checked; persistence flow remains O02. |
| `app/updater/src-tauri/tauri.conf.json` | Configuration review | 625 | `ccda47035cc2` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `app/updater/tsconfig.json` | Configuration review | 301 | `c3d2136219f5` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `app/updater/ui/index.html` | Configuration review | 575 | `4f622288b4ca` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `app/updater/ui/src/main.tsx` | Source review | 157 | `c5213a964838` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `app/updater/ui/src/updater.tsx` | Source review | 15531 | `4fa003a038a3` | Updater phases, download progress and native command/event flow inspected; restart truthfulness depends on O02. |
| `app/updater/ui/styles.css` | Configuration review | 11272 | `fc83ea77b541` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `app/updater/vite.config.ts` | Source review | 478 | `8260f4b94697` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `build.bat` | Configuration review | 1214 | `4f803e309313` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `config/default-config.json` | Configuration review | 1723 | `4c1a7d38fd71` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `config/default_config.h.in` | Configuration review | 343 | `379fab68a27d` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `docs/ARCHITECTURE.md` | Documentation | 5112 | `e4b52a663b6a` | Requirements/history/architecture cross-check; historical claims are not re-certified by this audit. |
| `docs/DECISIONS.md` | Documentation | 15980 | `74be1a60967b` | Requirements/history/architecture cross-check; historical claims are not re-certified by this audit. |
| `docs/DEVELOPMENT_RULES.md` | Documentation | 1498 | `db94e7965221` | Requirements/history/architecture cross-check; historical claims are not re-certified by this audit. |
| `docs/RELEASING.md` | Documentation | 6037 | `b94e0a8cf5ad` | Requirements/history/architecture cross-check; historical claims are not re-certified by this audit. |
| `docs/audit/2026-10-08.md` | Documentation | 16691 | `82057887b493` | Requirements/history/architecture cross-check; historical claims are not re-certified by this audit. |
| `docs/handover/ACTIVE_HANDOVER.md` | Documentation | 36695 | `dd4a6ebc4504` | Requirements/history/architecture cross-check; historical claims are not re-certified by this audit. |
| `global.json` | Configuration review | 80 | `1fdeb1b73953` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `installer/monolith.iss` | Configuration review | 3919 | `2d1a8e8f0c2f` | Installer payload/prerequisite/launch configuration inspected; clean install remains O14. |
| `libs/audio/CMakeLists.txt` | Configuration review | 488 | `531d48fc2e3a` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `libs/audio/audio.cpp` | Source review | 33382 | `e535e149e9fe` | A02: asynchronous activation lifetime, error cleanup and packet-buffer release; Windows runtime gate. |
| `libs/audio/audio.h` | Source review | 6210 | `2a69c1e89b32` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `libs/capture/CMakeLists.txt` | Configuration review | 455 | `33d942d9dbac` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `libs/capture/capture.cpp` | Source review | 21931 | `aa1b9cc940f5` | A01: session ownership, callback revocation, partial startup; Windows runtime gate. |
| `libs/capture/capture.h` | Source review | 2924 | `13eaa48601cb` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `libs/config/README.md` | Documentation | 306 | `2ccd33018471` | Requirements/history/architecture cross-check; historical claims are not re-certified by this audit. |
| `libs/disk-segments/CMakeLists.txt` | Configuration review | 602 | `7533c8b2572b` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `libs/disk-segments/disk_budget.h` | Source review | 601 | `7a5d3918e480` | Budget arithmetic and config boundary fixtures inspected. |
| `libs/disk-segments/disk_segments.cpp` | Source review | 16938 | `2dfc43965615` | Segment/key/pin/save/budget transitions covered by phase2; Windows sharing semantics remain untested. |
| `libs/disk-segments/disk_segments.h` | Source review | 3041 | `486f596848c9` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `libs/encoding/.gitkeep` | Scaffold | 1 | `01ba4719c80b` | Empty placeholder; retained pending cohesive module cleanup, O13. |
| `libs/encoding/CMakeLists.txt` | Configuration review | 720 | `6b844fe4c9cc` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `libs/encoding/encoding.cpp` | Source review | 42009 | `0ad4c6ac54ab` | A03-A06: PCM conversion, bounds, codec family, bounded mixer and worker teardown. |
| `libs/encoding/encoding.h` | Source review | 10311 | `4ef977fa4ae5` | Stream defaults, codec selection and caller contracts checked. |
| `libs/encoding/mux_common.cpp` | Source review | 6415 | `b3b47136a268` | Stream indexes, timebases, AVIO ownership inspected; synchronous I/O remains O03. |
| `libs/encoding/mux_common.h` | Source review | 3593 | `54fc50328bfe` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `libs/encoding/pcm_input.h` | Source review | 1531 | `82807585d990` | A03: shared packed-PCM conversion; production helper regression. |
| `libs/encoding/thumbnail.cpp` | Source review | 6290 | `a8db2bacf99d` | FFmpeg RAII, dimensions, scaling and file writes inspected; decoder runtime not independently stressed. |
| `libs/encoding/trim.cpp` | Source review | 26046 | `c35410aacb23` | Lossless/re-encode/concat timeline and error handling covered by media fixtures; atomic replacement is limited. |
| `libs/encoding/trim.h` | Source review | 2217 | `66f0535fcd97` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `libs/gamelist/CMakeLists.txt` | Configuration review | 696 | `6a437c33e202` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `libs/gamelist/gamelist.cpp` | Source review | 17450 | `149127aa45d8` | Database-gated selection, scan cadence, Windows process/window handling inspected; live detection untested. |
| `libs/gamelist/gamelist.h` | Source review | 2924 | `51a10e492ef7` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `libs/ipc/.gitkeep` | Scaffold | 1 | `01ba4719c80b` | Empty placeholder; retained pending cohesive module cleanup, O13. |
| `libs/ipc/CMakeLists.txt` | Configuration review | 445 | `8dcd53d1f067` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `libs/ipc/ipc_limits.h` | Source review | 988 | `3cf6f4aa5d08` | Shared protocol limits inspected against client/test bounds. |
| `libs/ipc/ipc_server.cpp` | Source review | 18701 | `a113a6357f64` | Framing, limits, connection shutdown and real socket regression; authentication/deadlines remain O09. |
| `libs/ipc/ipc_server.h` | Source review | 4344 | `28af0a2763c9` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `libs/logging/CMakeLists.txt` | Configuration review | 373 | `ce2e45ee854e` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `libs/logging/logging.cpp` | Source review | 3354 | `c713b056d266` | Locking, error visibility and rotation inspected; open-only rotation/synchronous flush remain O11. |
| `libs/logging/logging.h` | Source review | 1200 | `e1fce496df37` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `libs/platform-win/CMakeLists.txt` | Configuration review | 398 | `196d898006d6` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `libs/platform-win/platform_win.cpp` | Source review | 2857 | `957fc63e16b9` | UTF conversion, process/window handle use inspected; path-size and Windows execution limits remain. |
| `libs/platform-win/platform_win.h` | Source review | 1553 | `87a00380e837` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `libs/recording/CMakeLists.txt` | Configuration review | 574 | `cc76c7e6d5af` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `libs/recording/recording.cpp` | Source review | 9207 | `1b5fbc88bb15` | A07-A09: paths, B-frame timestamps and failed output; O03/O10 remain. |
| `libs/recording/recording.h` | Source review | 1082 | `ad79285258c2` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `libs/replay-buffer/.gitkeep` | Scaffold | 1 | `01ba4719c80b` | Empty placeholder; retained pending cohesive module cleanup, O13. |
| `libs/replay-buffer/CMakeLists.txt` | Configuration review | 624 | `2a53e279ab75` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `libs/replay-buffer/packet_ring.h` | Source review | 5918 | `59b2f5687831` | Retention, key selection, timestamps and skew covered by phase1/phase2. |
| `libs/replay-buffer/replay_buffer.cpp` | Source review | 12509 | `2f7551804d16` | A10: worker/callback lifetime and completion failure; full backpressure remains O03. |
| `libs/replay-buffer/replay_buffer.h` | Source review | 2736 | `153e73ebee8a` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `libs/storage/CMakeLists.txt` | Configuration review | 610 | `c931581ffa9f` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `libs/storage/storage.cpp` | Source review | 46820 | `5dc12a91d8fd` | A13: mutation names and abandoned mutex; journal fixtures pass; O05 remains. |
| `libs/storage/storage.h` | Source review | 8223 | `3afb54526991` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `plugins/stream-deck/.gitignore` | Configuration review | 36 | `d932426cfacc` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `plugins/stream-deck/README.md` | Documentation | 1328 | `d2674fb6e455` | Requirements/history/architecture cross-check; historical claims are not re-certified by this audit. |
| `plugins/stream-deck/manifest-notes.md` | Documentation | 1463 | `bb90f7b84660` | Requirements/history/architecture cross-check; historical claims are not re-certified by this audit. |
| `plugins/stream-deck/manifest.json` | Configuration review | 1698 | `afe74b70adc4` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `plugins/stream-deck/package-lock.json` | Dependency metadata | 3609 | `c7567ccf9859` | Lockfile/version consistency and locked-build use checked; no exhaustive dependency CVE audit. |
| `plugins/stream-deck/package.json` | Configuration review | 464 | `e033d71423da` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `plugins/stream-deck/scripts/package-plugin.ps1` | Configuration review | 6306 | `080ae6ad9a3b` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `plugins/stream-deck/src/actions/pause-resume.ts` | Source review | 828 | `6b09a19a00e4` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `plugins/stream-deck/src/actions/recording-toggle.ts` | Source review | 1043 | `0b16ee0f933b` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `plugins/stream-deck/src/actions/save-replay.ts` | Source review | 640 | `e6208220f7c1` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `plugins/stream-deck/src/ipc-client.ts` | Source review | 4600 | `ed3c7c4e7487` | A22: bounded framing/requests and single-socket teardown; real loopback integration passes. |
| `plugins/stream-deck/src/plugin.ts` | Source review | 1445 | `5cd3fd835e0e` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `plugins/stream-deck/top.fraa2a.monolith.sdPlugin/bin/actions/pause-resume.js` | Source review | 3849 | `1020742bfeed` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `plugins/stream-deck/top.fraa2a.monolith.sdPlugin/bin/actions/recording-toggle.js` | Source review | 4096 | `0c789ed02da3` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `plugins/stream-deck/top.fraa2a.monolith.sdPlugin/bin/actions/save-replay.js` | Source review | 3641 | `85c5f6b8589b` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `plugins/stream-deck/top.fraa2a.monolith.sdPlugin/bin/ipc-client.js` | Source review | 4167 | `d0f2ed456482` | Generated counterpart checked against TypeScript build; A22. |
| `plugins/stream-deck/top.fraa2a.monolith.sdPlugin/bin/plugin.js` | Source review | 1479 | `88b6d479f8ec` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `plugins/stream-deck/top.fraa2a.monolith.sdPlugin/imgs/README.md` | Documentation | 852 | `6d17f48fd24a` | Requirements/history/architecture cross-check; historical claims are not re-certified by this audit. |
| `plugins/stream-deck/tsconfig.json` | Configuration review | 299 | `72812f6b4ff6` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `ports/README.md` | Documentation | 671 | `69c1f777ebda` | Requirements/history/architecture cross-check; historical claims are not re-certified by this audit. |
| `ports/x264/LICENSE.vcpkg` | Configuration review | 1073 | `1ee376fc340e` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `ports/x264/allow-clang-cl.patch` | Configuration review | 863 | `9411a951a9f6` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `ports/x264/configure.patch` | Configuration review | 1328 | `2c7032f904de` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `ports/x264/parallel-install.patch` | Configuration review | 359 | `d8a4dd8d2642` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `ports/x264/portfile.cmake` | Configuration review | 4514 | `761c1edce09d` | Pinned x264 Git transport overlay; upstream build/patches retained. |
| `ports/x264/uwp-cflags.patch` | Configuration review | 577 | `4f90d00aac77` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `ports/x264/vcpkg.json` | Configuration review | 1034 | `311d617109f0` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `ports/x264/version.diff.in` | Configuration review | 449 | `5902c75a38f4` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `report.md` | Documentation | 64423 | `d6023eaac2fe` | Requirements/history/architecture cross-check; historical claims are not re-certified by this audit. |
| `research_codebase/findings_codebase.md` | Documentation | 1739 | `168d8908617f` | Requirements/history/architecture cross-check; historical claims are not re-certified by this audit. |
| `research_codebase/research_plan.md` | Documentation | 1164 | `3b2ab4d5a31d` | Requirements/history/architecture cross-check; historical claims are not re-certified by this audit. |
| `research_codebase/research_report.md` | Documentation | 1864 | `314f47b7e55c` | Requirements/history/architecture cross-check; historical claims are not re-certified by this audit. |
| `scripts/README.md` | Documentation | 869 | `8866574dd0e8` | Requirements/history/architecture cross-check; historical claims are not re-certified by this audit. |
| `scripts/check-text.py` | Configuration review | 820 | `2074fe998117` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `scripts/generate-appcast.ps1` | Configuration review | 6129 | `5dbff57583e3` | Legacy installer metadata/signature flow inspected; no release published. |
| `scripts/generate-update-manifest.ps1` | Configuration review | 6066 | `d028f1e2f1a2` | Component staging/runtime verification and signing metadata inspected. |
| `scripts/verify-runtime-dlls.ps1` | Configuration review | 3084 | `d73bafbfe42c` | Recursive per-directory dependency closure and eight verifier tests. |
| `tests/packaging/verify-runtime-dlls.tests.ps1` | Test support | 2668 | `05ceb975934b` | Fixture/build/shim checked against production modules; platform substitutions are explicit. |
| `tests/phase1/CMakeLists.txt` | Test support | 1193 | `0ad01309775c` | Fixture/build/shim checked against production modules; platform substitutions are explicit. |
| `tests/phase1/README.md` | Documentation | 2036 | `1e187d922517` | Requirements/history/architecture cross-check; historical claims are not re-certified by this audit. |
| `tests/phase1/ipc_server_test.cpp` | Source review | 6304 | `1c3fd3cd94dc` | Production regression/fixture inspected and executed where listed in the validation table; shims do not certify Windows behavior. |
| `tests/phase1/pcm_input_test.cpp` | Source review | 1012 | `26440f87aaeb` | Production regression/fixture inspected and executed where listed in the validation table; shims do not certify Windows behavior. |
| `tests/phase1/replay_packet_ring_test.cpp` | Source review | 4291 | `3b7d57ed6081` | Production regression/fixture inspected and executed where listed in the validation table; shims do not certify Windows behavior. |
| `tests/phase1/win32-shim/windows.h` | Source review | 455 | `b5370ba28757` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `tests/phase1/win32-shim/winsock2.h` | Source review | 962 | `c7dd69e95407` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `tests/phase1/win32-shim/ws2tcpip.h` | Source review | 35 | `482846ae880f` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `tests/phase2/CMakeLists.txt` | Test support | 2096 | `35591467821b` | Fixture/build/shim checked against production modules; platform substitutions are explicit. |
| `tests/phase2/README.md` | Documentation | 4508 | `c67162b4b719` | Requirements/history/architecture cross-check; historical claims are not re-certified by this audit. |
| `tests/phase2/encoding_test.cpp` | Source review | 2631 | `6b6dae23a07c` | Production regression/fixture inspected and executed where listed in the validation table; shims do not certify Windows behavior. |
| `tests/phase2/media_test.cpp` | Source review | 23486 | `53405d39622c` | Production regression/fixture inspected and executed where listed in the validation table; shims do not certify Windows behavior. |
| `tests/phase2/verify_timeline.py` | Test support | 4405 | `605f50dec108` | Production regression/fixture inspected and executed where listed in the validation table; shims do not certify Windows behavior. |
| `tests/phase2/win32-shim/windows.h` | Source review | 1996 | `ffc1984a7a75` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `tests/rust/.gitignore` | Test support | 9 | `306fd52e74fc` | Fixture/build/shim checked against production modules; platform substitutions are explicit. |
| `tests/rust/Cargo.lock` | Dependency metadata | 30080 | `a461455cbe65` | Lockfile/version consistency and locked-build use checked; no exhaustive dependency CVE audit. |
| `tests/rust/Cargo.toml` | Test support | 754 | `80ede3bfcf0e` | Fixture/build/shim checked against production modules; platform substitutions are explicit. |
| `tests/rust/src/lib.rs` | Source review | 1798 | `d9636b1a7938` | Production regression/fixture inspected and executed where listed in the validation table; shims do not certify Windows behavior. |
| `tests/rust/src/tests.rs` | Source review | 7980 | `b3953a2adb5a` | Production regression/fixture inspected and executed where listed in the validation table; shims do not certify Windows behavior. |
| `tests/stream-deck/ipc-client.test.mjs` | Test support | 2851 | `79911695d86e` | Production regression/fixture inspected and executed where listed in the validation table; shims do not certify Windows behavior. |
| `tests/u6/CMakeLists.txt` | Test support | 601 | `a7444450817e` | Fixture/build/shim checked against production modules; platform substitutions are explicit. |
| `tests/u6/storage_mutation_test.cpp` | Source review | 5263 | `e33d780a4397` | Production regression/fixture inspected and executed where listed in the validation table; shims do not certify Windows behavior. |
| `tests/u6/win32-shim/windows.h` | Source review | 2063 | `4f095a1fddba` | Static logic, lifetime/input/action flow and source/build references inspected; applicable module findings and platform limits are in the report. |
| `vcpkg-configuration.json` | Configuration review | 41 | `8282438f3650` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
| `vcpkg.json` | Configuration review | 384 | `c959fc3111a7` | Build/package/config/resource/style syntax, references and defaults inspected; no runtime proof implied. |
