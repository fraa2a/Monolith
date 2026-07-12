# Releasing Monolith

## Distribution Model

- Code repo: private `fraa2a/Monolith`.
- Public release repo: `fraa2a/Monolith-releases`.
- Published artifacts: installer, WinSparkle appcast, GPLv3 corresponding source archive.
- Install path: `%LocalAppData%\Programs\Monolith`.
- User data path: `%LocalAppData%\Monolith`.
- Install per-user, no admin needed.

Shipped payload got:

- `Monolith.exe`.
- `ui\Monolith.UI.exe`.
- Native dependency DLLs from vcpkg.
- Installer metadata + updater support.

No ship Node/npm. Only build-time frontend bundler (Vite) for `app/desktop-ui`.

## One-Time Setup

1. Make public repo `fraa2a/Monolith-releases`.

2. Make WinSparkle Ed25519 key pair:

   ```powershell
   openssl genpkey -algorithm ed25519 -out monolith-ed25519-priv.pem
   openssl pkey -in monolith-ed25519-priv.pem -pubout -outform DER -out pub.der
   [Convert]::ToBase64String((Get-Content pub.der -AsByteStream)[-32..-1])
   ```

3. Paste base64 public key into `kEdDsaPublicKey` in `app/recorder/src/updater.cpp`.

4. Store private PEM as repo secret `WINSPARKLE_ED_PRIVATE_KEY`.

   ```powershell
   Get-Content monolith-ed25519-priv.pem -Raw | Set-Clipboard
   ```

5. Make fine-grained PAT with `contents: write` on `fraa2a/Monolith-releases` only. Store as `RELEASES_REPO_PAT`.

Never commit private key. Lose it = shipped clients reject future updates signed by new key.

## Versioning

Source of truth:

- CI release: git tag `vX.Y.Z`.
- Local default: root `CMakeLists.txt` `project(monolith VERSION ...)`.

CI pass `-DMONOLITH_VERSION=X.Y.Z` to CMake. Version flow into generated `version.h`, Windows VERSIONINFO resource, installer metadata, appcast version.

## Release Command

```powershell
git tag vX.Y.Z
git push origin vX.Y.Z
```

CI then:

1. Extract version from tag.
2. Configure CMake with pinned vcpkg baseline.
3. Build `Monolith.exe` + `Monolith.UI.exe`.
4. Compile `installer/monolith.iss` into `MonolithSetup-X.Y.Z.exe`.
5. Sign installer metadata + make `appcast.xml` via `scripts/generate-appcast.ps1`.
6. Make GPLv3 source archive from `git archive`.
7. Publish all artifacts to `fraa2a/Monolith-releases`.

## Local Installer Build

```powershell
cmake --build build --config Release --parallel
& "$env:LocalAppData\Programs\Inno Setup 6\ISCC.exe" /DMonolithVersion=X.Y.Z installer\monolith.iss
```

Use numeric `X.Y.Z` version for `VersionInfoVersion`.

## Verification Checklist

- Release build make `Monolith.exe`.
- UI build make `ui\Monolith.UI.exe`.
- Installer compile.
- Installer run per-user, no admin.
- Fresh install start, show tray icon.
- UI open from tray.
- Save replay write media + catalog row.
- Manual recording start/stop write media + catalog row.
- Settings save update `settings.db`, engine reload.
- Appcast URL resolve publicly.
- WinSparkle verify signature, apply update.
- User data survive update + uninstall.

## Dependency Notes

Deps pinned in `vcpkg.json` via `builtin-baseline`. Bump baseline deliberate, verify release build after any dependency change.