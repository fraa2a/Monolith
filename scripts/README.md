# Scripts

This folder contains release and maintenance helpers. Review scripts before
running them; do not auto-run helper files from an untrusted branch.

## Current Scripts

### `generate-appcast.ps1`

Generates the WinSparkle `appcast.xml` for a built installer and can attach an
Ed25519 signature when `WINSPARKLE_ED_PRIVATE_KEY` is available.

Used by release CI after `installer/monolith.iss` produces
`MonolithSetup-X.Y.Z.exe`.

Typical CI flow:

```powershell
.\scripts\generate-appcast.ps1 -InstallerPath installer\Output\MonolithSetup-X.Y.Z.exe -Version X.Y.Z
```

See `docs/RELEASING.md` for the full release process and one-time key setup.

## Rules

- Keep scripts idempotent where possible.
- Keep release scripts Windows-friendly.
- Do not commit private signing keys or generated installer secrets.
- Update this README when adding a helper script.
