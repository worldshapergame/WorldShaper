# Builds assets/icon.ico from assets/icon.png.
#
# Kept in the repository rather than run once and forgotten, so that changing the logo is
# editing one PNG and running this — not hunting for whatever tool made the .ico last time.
#
# The .ico holds every size Windows asks for. Explorer picks 16 for the details view, 32 for
# the desktop, 48 for the taskbar and 256 for the large-icon view; leaving a size out makes
# Windows scale a neighbouring one, which on a logo with thin strokes is what turns it to
# mush at small sizes.
#
# Entries are PNG-compressed. That is the modern icon format, supported since Vista, and it
# keeps the file to a few kilobytes instead of the hundreds a 256x256 uncompressed entry
# would cost.

param(
    [string]$Source = (Join-Path $PSScriptRoot "..\assets\icon.png"),
    [string]$Output = (Join-Path $PSScriptRoot "..\assets\icon.ico")
)

Add-Type -AssemblyName System.Drawing

$sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)

# Not named $source: PowerShell is case-insensitive, so it would collide with $Source.
$logo = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Source))
$images = @()
foreach ($size in $sizes) {
    $bitmap = New-Object System.Drawing.Bitmap($size, $size)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.Clear([System.Drawing.Color]::Transparent)
    $graphics.DrawImage($logo, 0, 0, $size, $size)
    $graphics.Dispose()

    $stream = New-Object System.IO.MemoryStream
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $images += ,@{ Size = $size; Bytes = $stream.ToArray() }
    $stream.Dispose()
    $bitmap.Dispose()
}
$logo.Dispose()

# ICONDIR: reserved, type 1 (icon), count. Then one ICONDIRENTRY each, then the data.
$out = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($out)
$writer.Write([uint16]0)
$writer.Write([uint16]1)
$writer.Write([uint16]$images.Count)

$offset = 6 + (16 * $images.Count)
foreach ($image in $images) {
    # 256 is written as 0: the field is a single byte and 256 does not fit in one.
    $dimension = if ($image.Size -ge 256) { 0 } else { $image.Size }
    $writer.Write([byte]$dimension)      # width
    $writer.Write([byte]$dimension)      # height
    $writer.Write([byte]0)               # palette entries; 0 for truecolour
    $writer.Write([byte]0)               # reserved
    $writer.Write([uint16]1)             # colour planes
    $writer.Write([uint16]32)            # bits per pixel
    $writer.Write([uint32]$image.Bytes.Length)
    $writer.Write([uint32]$offset)
    $offset += $image.Bytes.Length
}
foreach ($image in $images) { $writer.Write($image.Bytes) }

$writer.Flush()
[System.IO.File]::WriteAllBytes((Join-Path (Split-Path -Parent $Output) (Split-Path -Leaf $Output)), $out.ToArray())
$writer.Dispose()
$out.Dispose()

Write-Host ("wrote {0} ({1:N0} bytes, {2} sizes)" -f $Output, (Get-Item $Output).Length, $images.Count)
