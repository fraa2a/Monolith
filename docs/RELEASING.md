# Releasing Monolith

## Distribution model

- Code repo (`fraa2a/Monolith`) is **private**.
- Installer, appcast, and GPLv3 corresponding-source zip are published to the
  **public** release repo `fraa2a/Monolith-releases` by CI.
- Installs are **per-user** (`%LocalAppData%\Programs\Monolith`, no admin) and
  the payload is **self-contained** (.NET + Windows App SDK bundled — no
  prerequisites on the target machine).
- Auto-update: WinSparkle inside `Monolith.exe` polls
  `https://github.com/fraa2a/Monolith-releases/releases/latest/download/appcast.xml`,
  verifies the EdDSA signature, downloads the new installer, and runs it
  silently (`/SILENT /SP- /NOCANCEL`). User config under
  `%LocalAppData%\Monolith` survives updates and uninstalls.

## One-time setup (before the first release)

1. **Create the public release repo** `fraa2a/Monolith-releases` (empty, with a
   README explaining it hosts Monolith binaries + GPL source archives).

2. **Generate the WinSparkle EdDSA key pair** (any machine with openssl):

   ```powershell
   openssl genpkey -algorithm ed25519 -out monolith-ed25519-priv.pem
   # Base64 raw 32-byte public key for the app:
   $der = openssl pkey -in monolith-ed25519-priv.pem -pubout -outform DER | % { $_ }
   # or, reliably:
   openssl pkey -in monolith-ed25519-priv.pem -pubout -outform DER -out pub.der
   [Convert]::ToBase64String((Get-Content pub.der -AsByteStream)[-32..-1])
   ```

   - Paste the base64 **public** key into `kEdDsaPublicKey` in
     `app/recorder/src/updater.cpp`.
   - Store the **private** PEM text as repo secret `WINSPARKLE_ED_PRIVATE_KEY`.
     The secret must contain the full file contents including the `BEGIN`/`END`
     marker lines and the line breaks between them. On Windows, copy with
     newlines preserved:
     ```powershell
     Get-Content monolith-ed25519-priv.pem -Raw | Set-Clipboard
     ```
     Then paste into the GitHub secret field. The CI script can also recover
     from a newline-stripped value, but storing the PEM verbatim is preferred.
   - Never commit the private key. Losing it means shipped clients will reject
     future updates (they pin the public key), so back it up securely.

3. **Create a fine-grained PAT** with `contents: write` on
   `fraa2a/Monolith-releases` only; store it as repo secret `RELEASES_REPO_PAT`.

## Releasing a version

```powershell
git tag vX.Y.Z
git push origin vX.Y.Z
```

CI (`.github/workflows/version-tag.yml`) then:

1. Extracts `X.Y.Z` from the tag and passes `-DMONOLITH_VERSION=X.Y.Z` to CMake
   (flows into the exe VERSIONINFO, the C# assembly version, and the installer).
2. Builds the full self-contained payload.
3. Compiles `installer/monolith.iss` → `MonolithSetup-X.Y.Z.exe`.
4. Signs the installer (Ed25519) and generates `appcast.xml`
   (`scripts/generate-appcast.ps1`).
5. Zips the source (`git archive`) for GPLv3 compliance.
6. Publishes everything as a GitHub Release on `fraa2a/Monolith-releases`.

## Local installer build (testing)

```powershell
cmake --build build --config Release --parallel
& "$env:LocalAppData\Programs\Inno Setup 6\ISCC.exe" /DMonolithVersion=0.0.0-dev installer\monolith.iss
# → installer/Output/MonolithSetup-0.0.0-dev.exe
```

Note: `/DMonolithVersion` must be numeric X.Y.Z for `VersionInfoVersion`; for a
quick smoke just use the next planned version number.

## Versioning notes

- Source of truth: git tag `vX.Y.Z` (CI) / `project(monolith VERSION ...)` in
  the root `CMakeLists.txt` (local default).
- WinSparkle compares the appcast `sparkle:version` against the running exe's
  FileVersion — keep both derived from the same tag (CI does this automatically).
- vcpkg dependencies are pinned via `builtin-baseline` in `vcpkg.json`; CI
  checks out that exact vcpkg commit. To bump dependencies, update the baseline
  SHA deliberately.
