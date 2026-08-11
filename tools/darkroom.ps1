# The gate for one clause: **a surface that receives no light is black, and there is no minimum.**
#
# It is a clause that is easy to assert, easy to lose, and invisible in every other test. An ambient
# constant added to a lit scene reads as taste; two of them lived in `resolve.comp` for the whole of
# this rewrite (`kIndirectFloor`, `kGroundBounce`) and nothing that measured the facility could have
# named either, because the facility is lit. A sealed box with nothing in it can.
#
# `clips/sealed_dark.clip` is that box: four metres of air with two metres of stone on every side,
# no opening, no emitter, no sky. Every term is nought by construction, so any pixel that comes back
# above nought is the renderer adding light it was not given.
#
# Run it with `-Fog` as well, because the air is the second way in and it is worse than the first:
# fog scatters the sun and the sky whether or not either reaches it, and the glow lands in FRONT of
# the walls, so no amount of black paint removes it.
#
#   .\tools\darkroom.ps1
#   .\tools\darkroom.ps1 -Fog
param(
    [switch]$Fog,
    [int]$Frame = 600,
    [int]$Width = 640,
    [int]$Height = 400,
    # Extra arguments, so the two arms of an A/B are one build. `-Extra "--no-bounce"` is the useful
    # one: with the bounce off, a room that receives nothing must still be black.
    [string]$Extra = ""
)

$exe = Join-Path $PSScriptRoot "..\build\bin\WorldShaper.exe"
$clip = Join-Path $PSScriptRoot "..\clips\sealed_dark.clip"
$shot = Join-Path $env:TEMP "ws_darkroom.png"

$runArgs = @("--clip-file", $clip, "--screenshot", $shot, "--screenshot-frame", "$Frame",
             "--settle", "--width", "$Width", "--height", "$Height", "--cam", "0,0,0,90,0",
             "--quality", "7", "--no-vsync", "--no-update-check", "--no-auto-quality",
             "--max-seconds", "300")
if ($Fog) { $runArgs += @("--fog", "0.08,0.9,0.6,40,0") }
if ($Extra -ne "") { $runArgs += @($Extra -split "\s+" | Where-Object { $_ -ne "" }) }

& $exe @runArgs | Out-Null

Add-Type -AssemblyName System.Drawing
$bmp = [System.Drawing.Bitmap]::FromFile($shot)
try {
    $brightest = 0
    $lit = 0
    for ($y = 0; $y -lt $bmp.Height; $y++) {
        for ($x = 0; $x -lt $bmp.Width; $x++) {
            # The developer overlay is drawn over the world and is not the world: three lines of
            # text at the top left, and the cursor's own marker in the middle. Everything else is
            # the picture, and the picture is what this measures.
            if ($y -lt 60 -and $x -lt 220) { continue }
            if ([math]::Abs($x - [int]($bmp.Width / 2)) -lt 6 -and
                [math]::Abs($y - [int]($bmp.Height / 2)) -lt 6) { continue }
            $p = $bmp.GetPixel($x, $y)
            $v = [math]::Max($p.R, [math]::Max($p.G, $p.B))
            if ($v -gt 0) { $lit++ }
            if ($v -gt $brightest) { $brightest = $v }
        }
    }
} finally {
    $bmp.Dispose()
}

$where = if ($Fog) { "with fog" } else { "clear" }
if ($brightest -eq 0) {
    Write-Output ("dark room, {0}{1}: BLACK - brightest channel 0 of 255, every pixel" -f
                  $where, $(if ($Extra) { " $Extra" } else { "" }))
} else {
    Write-Output ("dark room, {0}{1}: FAILED - brightest channel {2} of 255, {3} pixels above nought" -f
                  $where, $(if ($Extra) { " $Extra" } else { "" }), $brightest, $lit)
}
