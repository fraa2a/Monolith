param(
    [string] $Version = "",
    [string] $OutputDir = "dist"
)

$ErrorActionPreference = "Stop"

$pluginRoot = Split-Path -Parent $PSScriptRoot
$stagingDir = Join-Path ([System.IO.Path]::GetTempPath()) ("monolith-streamdeck-" + [System.Guid]::NewGuid().ToString("N"))
$bundleDir = Join-Path $stagingDir "top.fraa2a.monolith.sdPlugin"
$manifestTemplatePath = Join-Path $pluginRoot "manifest.json"
$manifestPath = Join-Path $bundleDir "manifest.json"
$packageJsonPath = Join-Path $pluginRoot "package.json"
$tscPath = Join-Path $pluginRoot "node_modules\.bin\tsc.cmd"
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $pluginRoot "..\.."))
$logoPath = Join-Path $repoRoot "MONOLITH.png"
$imgDir = Join-Path $bundleDir "imgs"

if (-not (Test-Path $manifestTemplatePath)) {
    throw "Stream Deck manifest template not found: $manifestTemplatePath"
}

if (-not (Test-Path $packageJsonPath)) {
    throw "package.json not found: $packageJsonPath"
}

if (-not (Test-Path $tscPath)) {
    throw "TypeScript compiler not found: $tscPath"
}

if (-not (Test-Path $logoPath)) {
    throw "Logo not found: $logoPath"
}

$packageJson = Get-Content $packageJsonPath -Raw | ConvertFrom-Json
if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = [string] $packageJson.version
}
if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    throw "Version must look like X.Y.Z, got: $Version"
}

$manifest = Get-Content $manifestTemplatePath -Raw | ConvertFrom-Json
$manifest.Version = "$Version.0"
$manifestJson = $manifest | ConvertTo-Json -Depth 10

New-Item -ItemType Directory -Force -Path $bundleDir | Out-Null
$manifestJson | Set-Content $manifestPath

New-Item -ItemType Directory -Force -Path $imgDir | Out-Null

Add-Type -AssemblyName System.Drawing
function New-ResizedPng {
    param(
        [Parameter(Mandatory)] [string] $SourcePath,
        [Parameter(Mandatory)] [string] $DestinationPath,
        [Parameter(Mandatory)] [int] $Size
    )

    $src = [System.Drawing.Image]::FromFile($SourcePath)
    try {
        $bmp = [System.Drawing.Bitmap]::new($Size, $Size)
        try {
            $graphics = [System.Drawing.Graphics]::FromImage($bmp)
            try {
                $graphics.Clear([System.Drawing.Color]::Transparent)
                $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
                $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                $graphics.DrawImage($src, 0, 0, $Size, $Size)
            } finally {
                $graphics.Dispose()
            }
            $bmp.Save($DestinationPath, [System.Drawing.Imaging.ImageFormat]::Png)
        } finally {
            $bmp.Dispose()
        }
    } finally {
        $src.Dispose()
    }
}

foreach ($icon in @(
    @{ Name = "plugin"; Size = 256 },
    @{ Name = "plugin@2x"; Size = 512 },
    @{ Name = "category"; Size = 28 },
    @{ Name = "category@2x"; Size = 56 },
    @{ Name = "save-replay"; Size = 20 },
    @{ Name = "save-replay@2x"; Size = 40 },
    @{ Name = "record-idle"; Size = 20 },
    @{ Name = "record-idle@2x"; Size = 40 },
    @{ Name = "pause"; Size = 20 },
    @{ Name = "pause@2x"; Size = 40 },
    @{ Name = "save-replay-key"; Size = 72 },
    @{ Name = "save-replay-key@2x"; Size = 144 },
    @{ Name = "record-idle-key"; Size = 72 },
    @{ Name = "record-idle-key@2x"; Size = 144 },
    @{ Name = "pause-key"; Size = 72 },
    @{ Name = "pause-key@2x"; Size = 144 }
)) {
    New-ResizedPng -SourcePath $logoPath -DestinationPath (Join-Path $imgDir "$($icon.Name).png") -Size $icon.Size
}

Push-Location $pluginRoot
try {
    & $tscPath --project tsconfig.json --outDir (Join-Path $bundleDir "bin")
    if ($LASTEXITCODE -ne 0) {
        throw "tsc build failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

$distDir = Join-Path $pluginRoot $OutputDir
New-Item -ItemType Directory -Force -Path $distDir | Out-Null
$archiveBase = "Monolith-StreamDeck-$Version"
$zipPath = Join-Path ([System.IO.Path]::GetTempPath()) ("$archiveBase-" + [System.Guid]::NewGuid().ToString("N") + ".zip")
$pluginPath = Join-Path $distDir "$archiveBase.streamDeckPlugin"

if (Test-Path $pluginPath) { Remove-Item $pluginPath -Force }

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($zipPath, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    $stagingRoot = [System.IO.Path]::GetFullPath($stagingDir).TrimEnd('\') + '\'
    foreach ($file in Get-ChildItem $bundleDir -Recurse -File) {
        $relativePath = [System.IO.Path]::GetFullPath($file.FullName).Substring($stagingRoot.Length).Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $zip,
            $file.FullName,
            $relativePath,
            [System.IO.Compression.CompressionLevel]::Optimal
        ) | Out-Null
    }
} finally {
    $zip.Dispose()
}
Copy-Item $zipPath $pluginPath -Force
Remove-Item $zipPath -Force
Remove-Item $stagingDir -Recurse -Force
Write-Host "Stream Deck package written: $pluginPath"
