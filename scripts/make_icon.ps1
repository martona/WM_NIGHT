# Generate a multi-resolution Windows .ico from a PNG, using only built-in GDI+
# (System.Drawing) -- no ImageMagick / Python / Pillow needed. Pads the source to
# a square (transparent) so non-square art isn't distorted, high-quality-downscales
# to each size, and writes a .ico whose frames are PNG-compressed (supported by
# Windows Vista+; our target is Win10+).
#
# Run with Windows PowerShell (System.Drawing is built in there):
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\make_icon.ps1
[CmdletBinding()]
param(
    [string]$InputPng,
    [string]$OutputIco,
    [int[]] $Sizes = @(16, 24, 32, 48, 64, 128, 256)
)

[Environment]::CurrentDirectory = $PWD.Path

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$scriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not $InputPng)  { $InputPng  = Join-Path $scriptDir "..\resources\fence-icon.png" }
if (-not $OutputIco) { $OutputIco = Join-Path $scriptDir "..\resources\fence-icon.ico" }

$InputPng  = [System.IO.Path]::GetFullPath($InputPng)
$OutputIco = [System.IO.Path]::GetFullPath($OutputIco)
[void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($OutputIco))

function New-ScaledPng {
    param([System.Drawing.Image]$Image, [int]$Size)
    $bmp = New-Object System.Drawing.Bitmap $Size, $Size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try {
        $g.Clear([System.Drawing.Color]::Transparent)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $g.DrawImage($Image, 0, 0, $Size, $Size)
    } finally { $g.Dispose() }
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $bytes = $ms.ToArray()
    $ms.Dispose()
    return ,$bytes
}

$src = [System.Drawing.Image]::FromFile($InputPng)
$square = $null
$frames = @()
try {
    Write-Host "Source: $InputPng  ($($src.Width)x$($src.Height))"

    # Pad to a centered transparent square so the aspect ratio is preserved.
    $side = [Math]::Max($src.Width, $src.Height)
    $square = New-Object System.Drawing.Bitmap $side, $side, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($square)
    try {
        $g.Clear([System.Drawing.Color]::Transparent)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $g.DrawImage($src, [int](($side - $src.Width) / 2), [int](($side - $src.Height) / 2), $src.Width, $src.Height)
    } finally { $g.Dispose() }

    foreach ($s in ($Sizes | Sort-Object)) {
        $frames += [pscustomobject]@{ Size = $s; Bytes = (New-ScaledPng -Image $square -Size $s) }
    }
} finally {
    if ($square) { $square.Dispose() }
    $src.Dispose()
}

# Assemble the .ico: ICONDIR (6 bytes) + ICONDIRENTRY (16 bytes each) + PNG blobs.
$out = New-Object System.IO.MemoryStream
$bw  = New-Object System.IO.BinaryWriter $out
try {
    $bw.Write([uint16]0)               # reserved
    $bw.Write([uint16]1)               # type: 1 = icon
    $bw.Write([uint16]$frames.Count)   # image count

    $offset = 6 + (16 * $frames.Count)
    foreach ($f in $frames) {
        $dim = if ($f.Size -ge 256) { 0 } else { $f.Size }   # 0 in the dir means 256
        $bw.Write([byte]$dim)                   # width
        $bw.Write([byte]$dim)                   # height
        $bw.Write([byte]0)                      # palette count (0 = none)
        $bw.Write([byte]0)                      # reserved
        $bw.Write([uint16]1)                    # color planes
        $bw.Write([uint16]32)                   # bits per pixel
        $bw.Write([uint32]$f.Bytes.Length)      # bytes of image data
        $bw.Write([uint32]$offset)              # offset of image data
        $offset += $f.Bytes.Length
    }
    foreach ($f in $frames) { $bw.Write($f.Bytes) }
    $bw.Flush()
    [System.IO.File]::WriteAllBytes($OutputIco, $out.ToArray())
} finally {
    $bw.Dispose(); $out.Dispose()
}

Write-Host "Wrote $OutputIco  ($($frames.Count) frames: $(($frames.Size) -join ', '))"
