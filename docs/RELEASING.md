# Releasing Monolith

## Distribution Model

- Code + releases: `fraa2a/Monolith` (single repo; releases are public
  assets on tag pushes — no separate releases repo).
- Published artifacts per release:
  - `MonolithSetup-X.Y.Z.exe` — full per-user installer (fresh installs).
  - `update-manifest.json` + `monolith-engine-*.zip` / `monolith-ui-*.zip` /
    `monolith-updater-*.zip` — component update payload for Updater.exe.
  - `appcast.xml` — legacy WinSparkle feed (migration only, see below).
  - `monolith-src-X.Y.Z.zip` (GPLv3 corresponding source), Stream Deck plugin.
- Install path: `%LocalAppData%\Programs\Monolith`.
- User data path: `%LocalAppData%\Monolith`.
- Install per-user, no admin needed.

Shipped payload:

- `Monolith.exe` (engine, owns the tray + recording).
- `Updater.exe` (component self-updater, `app/updater`).
- `ui\Monolith.UI.exe` (Tauri v2 interface).
- Native dependency DLLs from vcpkg + `config\default-config.json`.

No ship Node/npm. Only build-time frontend bundler (Vite) for `app/desktop-ui`
and `app/updater`.

## One-Time Setup (already done)

1. Ed25519 key pair (same pair the WinSparkle era created):

   ```powershell
   openssl genpkey -algorithm ed25519 -out monolith-ed25519-priv.pem
   openssl pkey -in monolith-ed25519-priv.pem -pubout -outform DER -out pub.der
   [Convert]::ToBase64String((Get-Content pub.der -AsByteStream)[-32..-1])
   ```

2. The base64 public key is embedded in `app/updater/src-tauri/src/download.rs`
   (`PUBLIC_KEY_B64`).

3. The private PEM lives in the repo secret `WINSPARKLE_ED_PRIVATE_KEY` and
   signs the full installer (appcast) and every component zip (manifest).

Never commit the private key. Lose it = shipped clients reject future
updates signed by a new key.

## Versioning (independent components)

Each component carries its own version — bump only what changed:

| Component | Source of truth | Used by |
|---|---|---|
| engine | `project(monolith VERSION ...)` in root `CMakeLists.txt` | `Monolith.exe` FileVersion, update-manifest |
| ui | `version` in `app/desktop-ui/src-tauri/tauri.conf.json` | `Monolith.UI.exe` FileVersion, update-manifest |
| updater | `version` in `app/updater/src-tauri/tauri.conf.json` | `Updater.exe` FileVersion, update-manifest |

The git tag `vX.Y.Z` only names the release and versions the full installer
(`MonolithSetup-X.Y.Z.exe`). A UI-only release bumps just the ui version —
clients never re-download the engine. CI warns if the tag sorts below any
component version it ships.

## Update flow (clients)

1. Engine startup (if `update.auto_check`) and tray "Check for Updates…"
   launch `Updater.exe` (`--auto` at startup: silent unless an update exists).
2. Updater.exe fetches
   `https://github.com/fraa2a/Monolith/releases/latest/download/update-manifest.json`,
   compares per-component versions against the installed FileVersions
   (fallback: `components.json`), and downloads only what changed.
3. Each zip is verified (sha256 + Ed25519 over the raw bytes) before apply.
4. Apply order: ui (engine closes the UI process via `update_close_ui`) →
   engine (`update_engine_exit`, swap, relaunch) → updater itself last
   (rename-to-`.old` self-swap). `*.old` files are swept on next launch.
5. Settings → About → "Check now" opens the same updater window.

### Legacy migration (WinSparkle installs)

Installs still on WinSparkle read `appcast.xml` (still generated every
release, pointing at the full installer). They update once through the old
full-installer path and land on the component-updater build; from there
`update-manifest.json` takes over. Keep generating the appcast indefinitely —
it costs nothing and there is no cutoff to coordinate.

## Release Command

```powershell
git tag vX.Y.Z
git push origin vX.Y.Z
```

CI then:

1. Extract the version from the tag (installer version only).
2. Configure CMake with pinned vcpkg baseline (no version injection — the
   engine version comes from `project(VERSION)`).
3. Build `Monolith.exe` + `ui\Monolith.UI.exe` + `Updater.exe`.
4. Compile `installer/monolith.iss` into `MonolithSetup-X.Y.Z.exe`.
5. Read the three component versions, package the component zips, sign them
   and emit `update-manifest.json` (`scripts/generate-update-manifest.ps1`).
6. Sign the installer + emit `appcast.xml` (`scripts/generate-appcast.ps1`).
7. Make the GPLv3 source archive from `git archive`.
8. Publish everything to the GitHub release for the tag.

## Local Installer Build

```powershell
cmake --build build --config Release --parallel
& "$env:LocalAppData\Programs\Inno Setup 6\ISCC.exe" /DMonolithVersion=X.Y.Z installer\monolith.iss
```

Use numeric `X.Y.Z` version for `VersionInfoVersion`.

## Testing the updater locally (no release needed)

- Point the updater at a local manifest:
  `$env:MONOLITH_UPDATE_MANIFEST = "http://127.0.0.1:PORT/update-manifest.json"`
  (serve the manifest + zips with any static server; URLs in the manifest
  point at the same server).
- `Updater.exe --force` reinstalls even when versions match — the way to
  exercise the download/apply path without publishing anything.
- `MONOLITH_APP_DIR` overrides the app directory the updater operates on.

## Verification Checklist

- Release build makes `Monolith.exe`, `ui\Monolith.UI.exe`, `Updater.exe`.
- Installer compile; installer run per-user, no admin.
- Fresh install start, show tray icon; UI open from tray.
- Save replay write media + catalog row; manual recording start/stop ditto.
- Settings save update `settings.db`, engine reload.
- Manifest URL resolves publicly; component zip signatures verify.
- UI-only release: existing install downloads only the ui zip, engine
  version untouched, no engine restart.
- Engine release: updater closes UI + engine, swaps, relaunches Monolith.
- Legacy appcast still updates an old WinSparkle install once.
- User data survives update + uninstall.

## Dependency Notes

Deps pinned in `vcpkg.json` via `builtin-baseline`. Bump baseline deliberate,
verify release build after any dependency change.
