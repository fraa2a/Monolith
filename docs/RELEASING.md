# Releasing Monolith

## Distribution Model

- Code repo: private `fraa2a/Monolith`.
- Public release repo: `fraa2a/Monolith-releases`.
- Published artifacts: installer, WinSparkle appcast, and GPLv3 corresponding
  source archive.
- Install path: `%LocalAppData%\Programs\Monolith`.
- User data path: `%LocalAppData%\Monolith`.
- Install is per-user and should not require admin.

The shipped payload includes:

- `Monolith.exe`.
- `ui\Monolith.UI.exe`.
- Native dependency DLLs from vcpkg.
- Installer metadata and updater support.

Deno is not shipped. It is only a build-time frontend bundler for
`app/desktop-ui`.

## One-Time Setup

1. Create public repo `fraa2a/Monolith-releases`.

2. Generate WinSparkle Ed25519 key pair:

   ```powershell
   openssl genpkey -algorithm ed25519 -out monolith-ed25519-priv.pem
   openssl pkey -in monolith-ed25519-priv.pem -pubout -outform DER -out pub.der
   [Convert]::ToBase64String((Get-Content pub.der -AsByteStream)[-32..-1])
   ```

3. Paste the base64 public key into `kEdDsaPublicKey` in
   `app/recorder/src/updater.cpp`.

4. Store the private PEM as repo secret `WINSPARKLE_ED_PRIVATE_KEY`.

   ```powershell
   Get-Content monolith-ed25519-priv.pem -Raw | Set-Clipboard
   ```

5. Create a fine-grained PAT with `contents: write` on
   `fraa2a/Monolith-releases` only. Store it as `RELEASES_REPO_PAT`.

Never commit the private key. Losing it means already-shipped clients reject
future updates signed by a new key.

## Versioning

Source of truth:

- CI release: git tag `vX.Y.Z`.
- Local default: root `CMakeLists.txt` `project(monolith VERSION ...)`.

CI passes `-DMONOLITH_VERSION=X.Y.Z` to CMake. That version flows into the
generated `version.h`, the Windows VERSIONINFO resource, installer metadata, and
appcast version.

## Release Command

```powershell
git tag vX.Y.Z
git push origin vX.Y.Z
```

CI then:

1. Extracts version from tag.
2. Configures CMake with the pinned vcpkg baseline.
3. Builds `Monolith.exe` and `Monolith.UI.exe`.
4. Compiles `installer/monolith.iss` into `MonolithSetup-X.Y.Z.exe`.
5. Signs installer metadata and generates `appcast.xml` via
   `scripts/generate-appcast.ps1`.
6. Creates GPLv3 source archive from `git archive`.
7. Publishes all artifacts to `fraa2a/Monolith-releases`.

## Local Installer Build

```powershell
cmake --build build --config Release --parallel
& "$env:LocalAppData\Programs\Inno Setup 6\ISCC.exe" /DMonolithVersion=1.4.1 installer\monolith.iss
```

Use a numeric `X.Y.Z` version for `VersionInfoVersion`.

## Verification Checklist

- Release build produces `Monolith.exe`.
- UI build produces `ui\Monolith.UI.exe`.
- Installer compiles.
- Installer runs per-user without admin.
- Fresh install starts and shows tray icon.
- UI opens from tray.
- Save replay writes media and catalog row.
- Manual recording start/stop writes media and catalog row.
- Settings save updates `settings.db` and engine reloads.
- Appcast URL resolves publicly.
- WinSparkle verifies signature and applies update.
- User data survives update and uninstall.

## Dependency Notes

Dependencies are pinned in `vcpkg.json` via `builtin-baseline`. Bump the baseline
deliberately and verify release build after any dependency change.
