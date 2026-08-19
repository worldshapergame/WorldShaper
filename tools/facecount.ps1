# How many distinct voxel faces a frame actually resolves, and how that moves with resolution.
#
# # Why this is the measurement that matters
#
# documentation/21-renderer-rewrite.md is built on one claim: that lighting computed per face
# stops scaling with the screen, because the number of distinct faces in view grows far more
# slowly than the number of pixels. The face pass is budgeted in faces, so if face count turns
# out to track pixels one for one, the budget has to grow with resolution and the whole argument
# for the rewrite collapses.
#
# Nothing in the engine has ever measured it. The arithmetic says it should hold — at a 90-degree
# lens and 1440 lines a voxel already covers a full pixel at 22.5 m, so everything nearer than
# that is at level 0 and gains nothing from more pixels, and only the 22.5-33.8 m band gains at
# 4K. But that is a prediction about a real building, and the building has railings and mouldings
# and a dome.
#
# So: debug view 11 writes each pixel's face key into the image, exactly, and this counts the
# distinct ones. Zero means sky or a summary block — no face, and nothing the face pass would
# shade.
#
#   tools\facecount.ps1
#   tools\facecount.ps1 -Views enclosed,close -Sizes dev,high

param(
    [string]$Views = "all",
    [string]$Sizes = "all",
    [int]$Frames = 90,
    [int]$Quality = 7,
    [string]$Out = ""
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\bin\WorldShaper.exe"
if (-not (Test-Path $exe)) { throw "no WorldShaper.exe at $exe - run build.bat first" }
. (Join-Path $PSScriptRoot "_grid.ps1")

$viewList = Select-Named $WsViews $Views "view"
$sizeList = Select-Named $WsSizes $Sizes "size"

$shots = Join-Path $env:TEMP "ws-facecount"
New-Item -ItemType Directory -Force -Path $shots | Out-Null

# Exact, and it has to be. A cardinality cannot be sampled: a subset of the pixels says nothing
# about how many distinct keys the whole frame holds. So every pixel is examined — and the inner
# loop is compiled rather than interpreted, because PowerShell walks a few million simple
# operations a second and a 4K frame is 8.3 million pixels of several operations each. Measured
# in PowerShell it took longer to count one image than to render the ninety frames behind it.
Add-Type -TypeDefinition @'
using System.Collections.Generic;

public static class WsFaceKeys {
    // The image arrives as 32bppArgb, which is B,G,R,A in memory on a little-endian machine.
    // The shader wrote the four bytes of the key to r,g,b,a with the low byte first, so the key
    // is reassembled from those positions rather than read as a word.
    public static long[] Count(byte[] bytes, int width, int height, int stride) {
        HashSet<uint> seen = new HashSet<uint>();
        long surface = 0;
        for (int y = 0; y < height; y++) {
            int row = y * stride;
            for (int x = 0; x < width; x++) {
                int i = row + x * 4;
                uint key = (uint)bytes[i + 2]
                         | ((uint)bytes[i + 1] << 8)
                         | ((uint)bytes[i] << 16)
                         | ((uint)bytes[i + 3] << 24);
                if (key == 0) continue;   // sky, or a summary block: no face
                surface++;
                seen.Add(key);
            }
        }
        return new long[] { seen.Count, surface };
    }
}
'@

function Measure-FaceKeys([string]$png) {
    $bmp = [System.Drawing.Bitmap]::FromFile($png)
    try {
        $w = $bmp.Width; $h = $bmp.Height
        $data = $bmp.LockBits(
            (New-Object System.Drawing.Rectangle 0, 0, $w, $h),
            [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
            [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $stride = $data.Stride
        $bytes = New-Object byte[] ($stride * $h)
        [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
        $bmp.UnlockBits($data)

        $r = [WsFaceKeys]::Count($bytes, $w, $h, $stride)
        return [PSCustomObject]@{
            Faces = $r[0]
            SurfacePixels = $r[1]
            Pixels = [long]$w * [long]$h
        }
    } finally {
        $bmp.Dispose()
    }
}

Write-Host ("  {0,-9} {1,-5} {2,10} {3,12} {4,12} {5,10}" -f
            "view", "size", "faces", "surface px", "px / face", "of frame")

$rows = @()
foreach ($view in $viewList) {
    $previous = $null
    foreach ($size in $sizeList) {
        $wh = $WsSizes[$size]
        $png = Join-Path $shots ("{0}-{1}.png" -f $view, $size)
        if (Test-Path $png) { Remove-Item $png }

        # R3d deleted the reference path tracer -- shaders/pathtrace.comp, pt_normals.glsl, the
        # --pathtrace flag, the F4 toggle and the branch itself (D517, D518). The binary WARNS about an
        # unknown argument and then IGNORES it, so a run that still passes --pathtrace comes back
        # looking like a perfectly good measurement of a mode that does not exist. That is trap 15, and
        # tools/baseline.ps1 already refuses it by name for the same reason.
        $runArgs = @("--debug-mode", "11",
                     "--screenshot", $png, "--screenshot-frame", "$Frames",
                     "--width", "$($wh[0])", "--height", "$($wh[1])",
                     "--cam", $WsViews[$view], "--quality", "$Quality",
                     "--no-vsync", "--no-update-check", "--no-auto-quality")

        $before = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        & $exe @runArgs | Out-Null
        $ErrorActionPreference = $before

        if (-not (Test-Path $png)) {
            Write-Host ("  {0,-9} {1,-5} FAILED" -f $view, $size) -ForegroundColor Red
            continue
        }
        $m = Measure-FaceKeys $png
        $perFace = if ($m.Faces -gt 0) { $m.SurfacePixels / $m.Faces } else { 0.0 }
        Write-Host ("  {0,-9} {1,-5} {2,10:N0} {3,12:N0} {4,12:N2} {5,10:P0}" -f
                    $view, $size, $m.Faces, $m.SurfacePixels, $perFace,
                    ($m.SurfacePixels / $m.Pixels))
        $rows += [PSCustomObject]@{
            view = $view; size = $size; width = $wh[0]; height = $wh[1]
            faces = $m.Faces; surface_px = $m.SurfacePixels; pixels = $m.Pixels
            px_per_face = [Math]::Round($perFace, 3)
        }
    }
}

# The number the rewrite lives or dies by: what happens to face count when the pixels multiply.
Write-Host ""
Write-Host "how face count moves with resolution (pixels move by the figure in brackets)"
foreach ($view in $viewList) {
    $r = @($rows | Where-Object { $_.view -eq $view })
    for ($i = 1; $i -lt $r.Count; $i++) {
        $a = $r[$i - 1]; $b = $r[$i]
        if ($a.faces -le 0) { continue }
        $pixelRatio = ($b.pixels / $a.pixels)
        $faceRatio = ($b.faces / $a.faces)
        Write-Host ("  {0,-9} {1,-4} -> {2,-5} faces x{3:N2}   (pixels x{4:N2})" -f
                    $view, $a.size, $b.size, $faceRatio, $pixelRatio)
    }
}

if ($Out -ne "") {
    $dir = Split-Path -Parent $Out
    if ($dir -ne "" -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $rows | Export-Csv -Path $Out -NoTypeInformation -Encoding utf8
    Write-Host ""
    Write-Host ("written: " + $Out)
}
