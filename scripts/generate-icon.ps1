# Generates app/assets/Monolith.ico (multi-size, PNG-compressed entries).
# Placeholder artwork: dark rounded square with a monolith slab. Replace the
# .ico with final artwork at any time; all projects reference this one file.
param(
    [string]$OutPath = (Join-Path $PSScriptRoot "..\app\assets\Monolith.ico")
)

Add-Type -AssemblyName System.Drawing

$sizes = 16, 24, 32, 48, 64, 128, 256
$pngs = @()

foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias

    # Background: dark rounded square
    $bgRect = New-Object System.Drawing.Rectangle(0, 0, $s, $s)
    $bgBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $bgRect,
        [System.Drawing.Color]::FromArgb(255, 30, 32, 38),
        [System.Drawing.Color]::FromArgb(255, 12, 13, 16),
        90.0)
    $radius = [Math]::Max(2, [int]($s * 0.18))
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $radius * 2
    $path.AddArc(0, 0, $d, $d, 180, 90)
    $path.AddArc($s - $d - 1, 0, $d, $d, 270, 90)
    $path.AddArc($s - $d - 1, $s - $d - 1, $d, $d, 0, 90)
    $path.AddArc(0, $s - $d - 1, $d, $d, 90, 90)
    $path.CloseFigure()
    $g.FillPath($bgBrush, $path)

    # Monolith slab: 1:4:9 proportions scaled to fit, centered
    $slabH = [int]($s * 0.62)
    $slabW = [Math]::Max(2, [int]($slabH * 4.0 / 9.0))
    $slabX = [int](($s - $slabW) / 2)
    $slabY = [int](($s - $slabH) / 2)
    $slabRect = New-Object System.Drawing.Rectangle($slabX, $slabY, $slabW, $slabH)
    $slabBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $slabRect,
        [System.Drawing.Color]::FromArgb(255, 10, 10, 12),
        [System.Drawing.Color]::FromArgb(255, 2, 2, 3),
        65.0)
    $g.FillRectangle($slabBrush, $slabRect)

    # Edge highlight on slab left side
    if ($s -ge 24) {
        $edgePen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(200, 120, 200, 255), [Math]::Max(1, [int]($s / 64)))
        $g.DrawLine($edgePen, $slabX, $slabY, $slabX, $slabY + $slabH)
        $edgePen.Dispose()
    }
    # Outline so slab reads against dark bg
    $outline = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 70, 75, 90), 1)
    $g.DrawRectangle($outline, $slabRect)
    $outline.Dispose()

    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $pngs += ,@($s, $ms.ToArray())
}

# Pack PNG entries into ICO container
$outDir = Split-Path -Parent $OutPath
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Force $outDir | Out-Null }
$fs = [System.IO.File]::Create($OutPath)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([uint16]0)               # reserved
$bw.Write([uint16]1)               # type: icon
$bw.Write([uint16]$pngs.Count)     # image count
$offset = 6 + 16 * $pngs.Count
foreach ($entry in $pngs) {
    $s = $entry[0]; $bytes = $entry[1]
    $dim = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$dim)          # width
    $bw.Write([byte]$dim)          # height
    $bw.Write([byte]0)             # palette
    $bw.Write([byte]0)             # reserved
    $bw.Write([uint16]1)           # planes
    $bw.Write([uint16]32)          # bpp
    $bw.Write([uint32]$bytes.Length)
    $bw.Write([uint32]$offset)
    $offset += $bytes.Length
}
foreach ($entry in $pngs) { $bw.Write($entry[1]) }
$bw.Flush(); $bw.Close()
Write-Host "Wrote $OutPath ($((Get-Item $OutPath).Length) bytes)"
