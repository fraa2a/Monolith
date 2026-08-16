<#
.SYNOPSIS
Generate update-manifest.json for a Monolith release: per-component zips
(engine / ui / updater) with size, sha256 and EdDSA signature.

.DESCRIPTION
The component updater (Updater.exe, app/updater) fetches
releases/latest/download/update-manifest.json and downloads ONLY the
components whose version is higher than the installed one — a UI-only
release never re-downloads the engine. Each component is versioned
independently (CMakeLists project() for the engine, the two tauri.conf.json
files for ui/updater); the git tag only names the release and versions the
full installer.

Signing uses the same Ed25519 key pair WinSparkle used (CI secret
WINSPARKLE_ED_PRIVATE_KEY, pure Ed25519 over the zip bytes — openssl
pkeyutl -sign -rawin; the public half is embedded in Updater.exe).

.EXAMPLE
./generate-update-manifest.ps1 `
    -EngineVersion 1.6.0 -UiVersion 1.7.0 -UpdaterVersion 1.0.0 `
    -EngineZip  monolith-engine-1.6.0.zip `
    -UiZip      monolith-ui-1.7.0.zip `
    -UpdaterZip monolith-updater-1.0.0.zip `
    -BaseUrl    https://github.com/fraa2a/Monolith/releases/download/v1.7.0 `
    -Tag        v1.7.0 `
    -NotesUrl   https://github.com/fraa2a/Monolith/releases/tag/v1.7.0 `
    -PrivateKeyPem $env:WINSPARKLE_ED_PRIVATE_KEY `
    -OutputPath update-manifest.json
#>
param(
    [Parameter(Mandatory)] [string] $EngineVersion,
    [Parameter(Mandatory)] [string] $UiVersion,
    [Parameter(Mandatory)] [string] $UpdaterVersion,
    [Parameter(Mandatory)] [string] $EngineZip,
    [Parameter(Mandatory)] [string] $UiZip,
    [Parameter(Mandatory)] [string] $UpdaterZip,
    # Release download base; canonical asset names are appended per component.
    [Parameter(Mandatory)] [string] $BaseUrl,
    [string] $Tag = "",
    [string] $NotesUrl = "",
    # PEM text of the Ed25519 private key (not a path); empty = unsigned manifest.
    [string] $PrivateKeyPem = "",
    [string] $OutputPath = "update-manifest.json"
)

$ErrorActionPreference = "Stop"

# Reconstruct canonical PEM from a potentially whitespace-mangled value.
# GitHub secrets pasted as a single line lose their newlines; openssl's PEM
# parser requires the BEGIN/END markers each on their own line.
function Format-Pem([string] $raw) {
    $t = $raw.Trim()
    if ($t -match '(?s)-----BEGIN ([A-Z0-9 ]+?)-----(.*?)-----END \1-----') {
        $label = $matches[1].Trim()
        $body  = ($matches[2] -replace '\s', '')
        $lines = [regex]::Matches($body, '.{1,64}') | ForEach-Object { $_.Value }
        return "-----BEGIN $label-----`n" + ($lines -join "`n") + "`n-----END $label-----`n"
    }
    return $t + "`n"
}

function Resolve-Openssl {
    $openssl = Get-Command openssl -ErrorAction SilentlyContinue
    if ($openssl) { return $openssl.Source }
    # Git for Windows bundles openssl; CI runners have it on PATH.
    $gitOpenssl = "$env:ProgramFiles\Git\usr\bin\openssl.exe"
    if (Test-Path $gitOpenssl) { return $gitOpenssl }
    throw "openssl not found; required to sign the component zips."
}

# Sparkle-format EdDSA signature: base64 of the raw 64-byte Ed25519 signature
# over the file bytes.
function Get-EdSignature([string] $FilePath, [string] $openssl) {
    $keyFile = New-TemporaryFile
    $sigFile = New-TemporaryFile
    try {
        [System.IO.File]::WriteAllText($keyFile.FullName, (Format-Pem $PrivateKeyPem))
        & $openssl pkey -in $keyFile.FullName -noout 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "WINSPARKLE_ED_PRIVATE_KEY is not a valid PEM private key. " +
                  "Store the full PEM text (BEGIN line + base64 body + END line) as the secret."
        }
        & $openssl pkeyutl -sign -inkey $keyFile.FullName -rawin `
            -in $FilePath -out $sigFile.FullName
        if ($LASTEXITCODE -ne 0) { throw "openssl signing failed (exit $LASTEXITCODE)" }
        $sigBytes = [System.IO.File]::ReadAllBytes($sigFile.FullName)
        if ($sigBytes.Length -ne 64) {
            throw "Unexpected Ed25519 signature length: $($sigBytes.Length) (want 64)"
        }
        return [Convert]::ToBase64String($sigBytes)
    } finally {
        Remove-Item $keyFile.FullName, $sigFile.FullName -Force -ErrorAction SilentlyContinue
    }
}

function Get-Component([string] $Key, [string] $Version, [string] $Zip, [string] $openssl) {
    if (-not (Test-Path $Zip)) { throw "Component zip not found: $Zip" }
    $item = Get-Item $Zip
    $sha256 = (Get-FileHash -Algorithm SHA256 $Zip).Hash.ToLowerInvariant()
    $edSignature = ""
    if ($PrivateKeyPem -ne "") {
        $edSignature = Get-EdSignature $Zip $openssl
        Write-Host "$Key signed (edSignature: $($edSignature.Substring(0,16))...)"
    } else {
        Write-Warning "No private key supplied - emitting UNSIGNED manifest entry for $Key. Updater.exe clients with the public key configured will reject it."
    }
    return [ordered] @{
        version     = $Version
        url         = "$BaseUrl/$(Split-Path $Zip -Leaf)"
        size        = $item.Length
        sha256      = $sha256
        edSignature = $edSignature
    }
}

$openssl = $null
if ($PrivateKeyPem -ne "") { $openssl = Resolve-Openssl }

$manifest = [ordered] @{
    schema       = 1
    publishedAt  = (Get-Date).ToUniversalTime().ToString("yyyy-MM-dd'T'HH:mm:ss'Z'",
                    [System.Globalization.CultureInfo]::InvariantCulture)
    release      = [ordered] @{
        tag      = $Tag
        notesUrl = $NotesUrl
    }
    components   = [ordered] @{
        engine  = Get-Component "engine"  $EngineVersion  $EngineZip  $openssl
        ui      = Get-Component "ui"      $UiVersion      $UiZip      $openssl
        updater = Get-Component "updater" $UpdaterVersion $UpdaterZip $openssl
    }
}

$outFull = [System.IO.Path]::GetFullPath(
    [System.IO.Path]::Combine((Get-Location).Path, $OutputPath))
[System.IO.File]::WriteAllText($outFull,
    ($manifest | ConvertTo-Json -Depth 5), [System.Text.UTF8Encoding]::new($false))
Write-Host "Manifest written: $outFull (engine $EngineVersion / ui $UiVersion / updater $UpdaterVersion)"
