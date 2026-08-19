# How speckled a render is, as a number.
#
# Speckle is the one rendering fault that argument cannot settle. It is obvious on screen and
# invisible in any average — a hundred blown pixels in a million move the mean by nothing — so the
# constants that control it have been tuned by looking, which means every tuning is an opinion and
# no two of them are comparable. The path tracer's own comments quote figures like "0.934 to
# 0.976" from some earlier harness that is not in the tree, and there is no way to reproduce them.
#
# So: a fixed camera, a fixed frame count, a number.
#
# WHAT IT MEASURES. For every pixel, how far its luminance sits from the median of its eight
# neighbours, relative to the local level. A smooth surface scores near nought whether it is bright
# or dark; a lone bright pixel on a smooth wall scores near one however dim the room. Taking the
# median rather than the mean is what makes it a speckle metric rather than a sharpness metric — a
# real edge moves the mean of its neighbours and leaves the median alone, so an image full of
# mouldings and joints does not read as noise.
#
# Reported as the mean over the frame, times a thousand so that the numbers worth comparing are
# whole ones, plus the count of pixels that are outright fireflies: more than four times their
# neighbourhood. Those are the ones the eye actually catches.
#
#   tools\speckle.ps1 -Cam "12,3,-6,0.5,0"                  # one camera, current build
#   tools\speckle.ps1 -Cam "..." -Frames 8,32,128           # how fast it converges
#
# A speckle that falls steeply with frame count is variance and will average out; one that does not
# is a wrong value that has been cached, and no amount of waiting will fix it. Those are different
# faults with different repairs, and the frame sweep is what tells them apart.

param(
    [string]$Cam = "0,6,-24,0.02,0",
    [string]$Clip = "clips/facility.clip",
    # A string, not an int array. Passing `-Frames 16,64` to a script run with -File hands the
    # parameter one token, and PowerShell binds "16,64" to an int array as the single number 1664
    # without complaining about it. Splitting it here means the argument reads the same however
    # the script is invoked.
    [string]$Frames = "16,64",
    [int]$Metre = 0,
    [int]$Width = 640,
    [int]$Height = 360,
    [int]$Quality = 7,
    [switch]$PathTrace,
    # "vx,vy,vz,vyaw" in metres and degrees a second. THE case that matters, and the one a
    # screenshot cannot show.
    #
    # A still camera accumulates hundreds of samples a pixel and every measurement taken that way
    # says the picture is clean. A moving one resets the accumulator every frame and shows what a
    # player actually looks at. `Options::fly` in src/app/main.cpp makes the same point and is the
    # reason it exists; measuring only the still case is how a renderer ends up tuned for a
    # photograph nobody takes.
    [string]$Fly = "",
    [string]$Out = ""
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\bin\WorldShaper.exe"
if (-not (Test-Path $exe)) { throw "no WorldShaper.exe at $exe - run build.bat first" }
if ($Out -eq "") { $Out = Join-Path $env:TEMP "ws-speckle" }
New-Item -ItemType Directory -Force -Path $Out | Out-Null

# The metric itself, shared with tools\baseline.ps1 so that the two cannot drift apart.
. (Join-Path $PSScriptRoot "_measure.ps1")

Write-Host ("camera {0}   {1}x{2}{3}" -f $Cam, $Width, $Height,
            $(if ($Fly -ne "") { "   flying " + $Fly } else { "   still" }))
Write-Host ""
Write-Host ("  {0,7}  {1,9}  {2,10}  {3}" -f "frames", "speckle", "fireflies", "image")

foreach ($f in ($Frames -split "," | ForEach-Object { [int]$_.Trim() })) {
    $png = Join-Path $Out ("f{0}.png" -f $f)
    if (Test-Path $png) { Remove-Item $png }
    # Detail PINNED, and this is the difference between a metric and a number.
    #
    # The quality controller raises the render scale over the first seconds, so a shot at sixteen
    # frames is a small image stretched to fit — which is smooth for reasons that have nothing to
    # do with the path tracer. Left to itself the sweep read 60, then 140, then 66, and the shape
    # of that curve was the controller's, not the renderer's.
    $shot = @("--clip-file", $Clip, "--no-vsync", "--no-update-check", "--quality", "$Quality",
              "--width", "$Width", "--height", "$Height",
              "--screenshot-frame", "$f", "--screenshot", $png, "--cam", $Cam)
    if ($Metre -gt 0) { $shot += @("--clip-metre", "$Metre") }
    # R3d deleted the reference path tracer (D517, D518). Refused by name rather than silently
    # ignored, which is what tools/baseline.ps1 does and for the same reason: the binary warns
    # about an unknown argument and then carries on, so a run that still asks for the tracer
    # returns a clean-looking measurement of the real-time path instead. Trap 15.
    if ($PathTrace) {
        throw "the reference path tracer was deleted by R3d; there is no --pathtrace to measure"
    }
    if ($Fly -ne "") { $shot += @("--fly", $Fly) }

    # Deliberately not `2>&1`. Windows PowerShell wraps each stderr line from a native program in
    # an ErrorRecord, so redirecting it turns the renderer's ordinary warnings into terminating
    # errors and the tool dies on a frame that merely took a while.
    $before = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $exe @shot | Out-Null
    $ErrorActionPreference = $before

    if (-not (Test-Path $png)) {
        Write-Host ("  {0,7}  FAILED - no image written" -f $f)
        continue
    }
    $m = Measure-Speckle $png
    Write-Host ("  {0,7}  {1,9:N2}  {2,10}  {3}" -f $f, $m.Speckle, $m.Fireflies, $png)
}
