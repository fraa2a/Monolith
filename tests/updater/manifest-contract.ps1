$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ([guid]::NewGuid().ToString())
New-Item -ItemType Directory $root | Out-Null
try {
    $key = Join-Path $root 'private.pem'
    $pub = Join-Path $root 'public.der'
    & openssl genpkey -algorithm ED25519 -out $key
    if ($LASTEXITCODE -ne 0) { throw 'test key generation failed' }
    & openssl pkey -in $key -pubout -outform DER -out $pub
    if ($LASTEXITCODE -ne 0) { throw 'test public key extraction failed' }
    $bytes = [IO.File]::ReadAllBytes($pub)
    $public = [Convert]::ToBase64String($bytes[($bytes.Length-32)..($bytes.Length-1)])
    $zip = Join-Path $root 'payload.zip'
    $payload = Join-Path $root 'payload'
    New-Item -ItemType Directory $payload | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $payload 'sample.bin'), [byte[]](1,2,3,4))
    [IO.Compression.ZipFile]::CreateFromDirectory($payload, $zip)
    $output = Join-Path $root 'manifest.json'
    $args = @{ EngineVersion='1.2.3'; UiVersion='2.3.4'; UpdaterVersion='3.4.5'; EngineZip=$zip; UiZip=$zip; UpdaterZip=$zip; BaseUrl='https://example.com/releases'; OutputPath=$output }
    $failed = $false
    try { & "$PSScriptRoot/../../scripts/generate-update-manifest.ps1" @args -PrivateKeyPem '' } catch { $failed = $true }
    if (-not $failed -or (Test-Path $output)) { throw 'missing signing key published a manifest' }
    $failed = $false
    try { & "$PSScriptRoot/../../scripts/generate-update-manifest.ps1" @args -PrivateKeyPem 'invalid private key' } catch { $failed = $true }
    if (-not $failed -or (Test-Path $output)) { throw 'invalid signing key published a manifest' }
    $pem = [IO.File]::ReadAllText($key)
    $failed = $false
    try { & "$PSScriptRoot/../../scripts/generate-update-manifest.ps1" @args -PrivateKeyPem $pem } catch { $failed = $true }
    if (-not $failed -or (Test-Path $output)) { throw 'wrong signing key published a manifest' }
    & "$PSScriptRoot/../../scripts/generate-update-manifest.ps1" @args -PrivateKeyPem $pem -PublicKeyBase64 $public
    $env:MONOLITH_GENERATED_MANIFEST = $output
    $env:MONOLITH_TEST_PUBLIC_KEY = $public
    $env:MONOLITH_TEST_PAYLOAD = $zip
    cargo test --locked --no-default-features --manifest-path "$PSScriptRoot/../rust/Cargo.toml" -- --ignored --exact manifest::tests::generated_release_contract
    if ($LASTEXITCODE -ne 0) { throw 'generated manifest rejected by production parser' }
} finally {
    Remove-Item $root -Recurse -Force
    Remove-Item Env:MONOLITH_GENERATED_MANIFEST, Env:MONOLITH_TEST_PUBLIC_KEY, Env:MONOLITH_TEST_PAYLOAD -ErrorAction SilentlyContinue
}
