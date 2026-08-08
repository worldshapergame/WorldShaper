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
    [string]$Out = ""
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\bin\WorldShaper.exe"
if (-not (Test-Path $exe)) { throw "no WorldShaper.exe at $exe - run build.bat first" }
if ($Out -eq "") { $Out = Join-Path $env:TEMP "ws-speckle" }
New-Item -ItemType Directory -Force -Path $Out | Out-Null

function Measure-Speckle([string]$png) {
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

        # Luminance once, so the neighbourhood walk is arithmetic and not colour conversion.
        $lum = New-Object 'double[]' ($w * $h)
        for ($y = 0; $y -lt $h; $y++) {
            $row = $y * $stride
            for ($x = 0; $x -lt $w; $x++) {
                $i = $row + $x * 4
                $lum[$y * $w + $x] = 0.0722 * $bytes[$i] + 0.7152 * $bytes[$i + 1] +
                                     0.2126 * $bytes[$i + 2]
            }
        }

        $total = 0.0
        $counted = 0
        $fireflies = 0
        $neighbours = New-Object 'double[]' 8
        for ($y = 1; $y -lt $h - 1; $y++) {
            for ($x = 1; $x -lt $w - 1; $x++) {
                $k = $y * $w + $x
                $n = 0
                for ($dy = -1; $dy -le 1; $dy++) {
                    for ($dx = -1; $dx -le 1; $dx++) {
                        if ($dx -eq 0 -and $dy -eq 0) { continue }
                        $neighbours[$n] = $lum[($y + $dy) * $w + ($x + $dx)]
                        $n++
                    }
                }
                $sorted = $neighbours | Sort-Object
                $median = ($sorted[3] + $sorted[4]) * 0.5
                # Against the local level plus a floor, so a dark room does not read as all noise
                # for want of anything to divide by.
                $relative = [Math]::Abs($lum[$k] - $median) / ($median + 8.0)
                $total += $relative
                $counted++
                if ($lum[$k] -gt $median * 4.0 + 24.0) { $fireflies++ }
            }
        }
        return [PSCustomObject]@{
            Speckle   = if ($counted -gt 0) { 1000.0 * $total / $counted } else { 0.0 }
            Fireflies = $fireflies
            Pixels    = $counted
        }
    } finally {
        $bmp.Dispose()
    }
}

Write-Host ("camera {0}   {1}x{2}" -f $Cam, $Width, $Height)
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
    if ($PathTrace) { $shot += "--pathtrace" }

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
