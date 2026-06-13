<#
.SYNOPSIS
Generate the WinSparkle appcast.xml for a Monolith release and (optionally)
sign the installer with the EdDSA (Ed25519) private key.

.DESCRIPTION
Produces a Sparkle-format RSS appcast consumed by the recorder's WinSparkle
integration (app/recorder/src/updater.cpp). The appcast and the installer are
published as assets on the default public repo release page, so
`releases/latest/download/appcast.xml` is a stable unauthenticated URL.

Signing uses openssl (pure Ed25519 over the installer bytes, Sparkle format).
One-time key generation:
    openssl genpkey -algorithm ed25519 -out monolith-ed25519-priv.pem
    # Base64 raw public key for win_sparkle_set_eddsa_public_key():
    openssl pkey -in monolith-ed25519-priv.pem -pubout -outform DER |
        ForEach-Object -Begin { $b = [System.IO.MemoryStream]::new() } `
            -Process { $b.WriteByte($_) } `
            -End { [Convert]::ToBase64String($b.ToArray()[-32..-1]) }
Keep the private PEM only in the CI secret WINSPARKLE_ED_PRIVATE_KEY.

.EXAMPLE
./generate-appcast.ps1 -Version 0.4.0 `
    -InstallerPath installer/Output/MonolithSetup-0.4.0.exe `
    -InstallerUrl  https://github.com/fraa2a/Monolith/releases/download/v0.4.0/MonolithSetup-0.4.0.exe `
    -PrivateKeyPem $env:WINSPARKLE_ED_PRIVATE_KEY `
    -OutputPath appcast.xml
#>
param(
    [Parameter(Mandatory)] [string] $Version,
    [Parameter(Mandatory)] [string] $InstallerPath,
    [Parameter(Mandatory)] [string] $InstallerUrl,
    # PEM text of the Ed25519 private key (not a path); empty = unsigned appcast.
    [string] $PrivateKeyPem = "",
    [string] $ReleaseNotesUrl = "",
    [string] $OutputPath = "appcast.xml"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $InstallerPath)) {
    throw "Installer not found: $InstallerPath"
}
$installerItem = Get-Item $InstallerPath
$length = $installerItem.Length

# ── EdDSA signature (Sparkle format: base64 of raw 64-byte Ed25519 sig) ──────
$edSignature = ""
if ($PrivateKeyPem -ne "") {
    $openssl = Get-Command openssl -ErrorAction SilentlyContinue
    if (-not $openssl) {
        # Git for Windows bundles openssl; CI runners have it on PATH.
        $gitOpenssl = "$env:ProgramFiles\Git\usr\bin\openssl.exe"
        if (Test-Path $gitOpenssl) { $openssl = $gitOpenssl }
        else { throw "openssl not found; required to sign the installer." }
    } else {
        $openssl = $openssl.Source
    }

    $keyFile = New-TemporaryFile
    $sigFile = New-TemporaryFile
    try {
        # PEM must end with a newline or openssl rejects it.
        [System.IO.File]::WriteAllText($keyFile.FullName, $PrivateKeyPem.TrimEnd() + "`n")
        & $openssl pkeyutl -sign -inkey $keyFile.FullName -rawin `
            -in $installerItem.FullName -out $sigFile.FullName
        if ($LASTEXITCODE -ne 0) { throw "openssl signing failed (exit $LASTEXITCODE)" }
        $sigBytes = [System.IO.File]::ReadAllBytes($sigFile.FullName)
        if ($sigBytes.Length -ne 64) {
            throw "Unexpected Ed25519 signature length: $($sigBytes.Length) (want 64)"
        }
        $edSignature = [Convert]::ToBase64String($sigBytes)
        Write-Host "Installer signed (edSignature: $($edSignature.Substring(0,16))...)"
    } finally {
        Remove-Item $keyFile.FullName, $sigFile.FullName -Force -ErrorAction SilentlyContinue
    }
} else {
    Write-Warning "No private key supplied - emitting UNSIGNED appcast. WinSparkle clients with a public key configured will reject this update."
}

# ── Appcast XML ───────────────────────────────────────────────────────────────
$pubDate = (Get-Date).ToUniversalTime().ToString("ddd, dd MMM yyyy HH:mm:ss 'GMT'", [System.Globalization.CultureInfo]::InvariantCulture)
$sigAttr = if ($edSignature) { " sparkle:edSignature=`"$edSignature`"" } else { "" }
$notes = if ($ReleaseNotesUrl) {
    "      <sparkle:releaseNotesLink>$ReleaseNotesUrl</sparkle:releaseNotesLink>`n"
} else { "" }

$xml = @"
<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>Monolith</title>
    <link>https://github.com/fraa2a/Monolith</link>
    <description>Monolith release feed</description>
    <language>en</language>
    <item>
      <title>Version $Version</title>
$notes      <pubDate>$pubDate</pubDate>
      <enclosure
        url="$InstallerUrl"
        sparkle:version="$Version"
        sparkle:os="windows-x64"
        length="$length"
        type="application/octet-stream"
        sparkle:installerArguments="/SILENT /SP- /NOCANCEL"$sigAttr />
    </item>
  </channel>
</rss>
"@

$outFull = [System.IO.Path]::GetFullPath(
    [System.IO.Path]::Combine((Get-Location).Path, $OutputPath))
[System.IO.File]::WriteAllText($outFull, $xml, [System.Text.UTF8Encoding]::new($false))
Write-Host "Appcast written: $outFull (installer length=$length)"
