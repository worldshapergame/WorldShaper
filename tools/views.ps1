# Look at a clip from every side, with the real renderer, framed on whatever you are looking at.
#
# A clip is a building, and a building is not a number. Every check the forge does — is it
# joined together, can you walk on it, is anything nearly-but-not-quite aligned — answers a
# question you already knew to ask. Looking at it answers the ones you did not.
#
# So this builds a clip and photographs it through the actual path tracer, to actual PNG files.
# The first run builds the world and caches it; every view after that loads the cache in under
# half a second, so a dozen angles costs about as much as one.
#
# # Framing
#
# The focus is measured, not guessed. The tool asks the engine where the matter actually is —
# `worldbox` in the report — and puts the camera at the distance that makes that box fill the
# frame, at the game's own field of view. Nothing here changes the lens; a photograph taken
# through a different lens than the game uses is a photograph of a different game.
#
# The focus can be the whole clip, one named part, or a box given by hand:
#
#   tools\views.ps1 -Clip clips\facility.clip -Out renders\all
#   tools\views.ps1 -Clip clips\facility.clip -Part portico -Out renders\portico
#   tools\views.ps1 -Clip clips\facility.clip -Focus "-6,0,-4, 6,7,4" -Out renders\rotunda -Views inside
#
# -Part frames that part *and builds only that part*, so it is seen on its own.
# -Focus frames a box while building the whole clip, which is how an interior is photographed:
#  the room is the focus, the building around it is still there.
#
# # Views
#
# -Views ring     eight compass elevations and a plan               (the default)
# -Views quick    front, corner and plan, for a fast look
# -Views close    a tight orbit at head height, for detail
# -Views inside   standing in the middle of the focus, looking each way and up
# -Views all      everything above
#
# The contact sheet at the end is one image with every view in it, which is the thing to
# actually look at: a fault that is invisible head-on is obvious the moment it is next to the
# same wall seen from the side. Each tile is labelled with its name and how much of the frame
# the focus filled, so "too far away" is a number rather than an impression.

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Part = "",
    [string]$Focus = "",
    [int]$Metre = 16,
    [int]$Width = 720,
    [int]$Height = 460,
    [int]$Frames = 40,
    [switch]$PathTrace,
    [string]$Views = "ring",
    [double]$Fill = 0.88,
    [string]$Only = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\bin\WorldShaper.exe"
if (-not (Test-Path $exe)) { throw "no WorldShaper.exe at $exe - run build.bat first" }
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Force -Path $Out | Out-Null }

# The game's own field of view. Kept here as a number to compute with, never as a number to
# change: framing is done by moving the camera, because moving the lens photographs a building
# nobody will ever stand in front of.
$FovDegrees = 90.0

# --- where is the thing --------------------------------------------------------------------

$low = @(0.0, 0.0, 0.0)
$high = @(1.0, 1.0, 1.0)

if ($Focus -ne "") {
    $n = ($Focus -split "[,\s]+" | Where-Object { $_ -ne "" }) | ForEach-Object { [double]$_ }
    if ($n.Count -lt 6) { throw "-Focus needs six numbers: x0,y0,z0,x1,y1,z1" }
    $low = @($n[0], $n[1], $n[2])
    $high = @($n[3], $n[4], $n[5])
    Write-Host ("focus given: {0} .. {1}" -f ($low -join ","), ($high -join ","))
} else {
    $ask = @("--clip-file", $Clip, "--clip-metre", "6")
    if ($Part -ne "") { $ask += @("--clip-part", $Part) }
    $report = & $exe @ask 2>&1 | Out-String
    $box = ($report -split "`n" | Select-String "^worldbox" | Select-Object -First 1)
    if (-not $box) {
        Write-Host $report
        throw "the clip did not report a worldbox - it may have failed to build"
    }
    $n = ($box.Line -split "\s+" | Where-Object { $_ -match "^-?[0-9.]+$" }) | ForEach-Object { [double]$_ }
    $low = @($n[0], $n[1], $n[2])
    $high = @($n[3], $n[4], $n[5])
    Write-Host ("focus measured: {0:0.##},{1:0.##},{2:0.##} .. {3:0.##},{4:0.##},{5:0.##} m" -f $low[0], $low[1], $low[2], $high[0], $high[1], $high[2])
}

# A clip sampled at fewer voxels to the metre lands in the world *smaller*, because its cells are
# written one to a world voxel however coarsely they were cut. So a preview built at eight to the
# metre is a quarter-scale model, and a camera aimed at where the full-size building would be is
# aimed at empty sky. The focus box is in the clip's own metres; this puts it where the world
# actually has it.
$worldScale = $Metre / 32.0
if ($worldScale -ne 1.0) {
    for ($i = 0; $i -lt 3; $i++) { $low[$i] = $low[$i] * $worldScale; $high[$i] = $high[$i] * $worldScale }
    Write-Host ("  built at {0}/m, so it stands at {1:P0} size: {2:0.##},{3:0.##},{4:0.##} .. {5:0.##},{6:0.##},{7:0.##} m" -f $Metre, $worldScale, $low[0], $low[1], $low[2], $high[0], $high[1], $high[2])
}

$centre = @((($low[0] + $high[0]) * 0.5), (($low[1] + $high[1]) * 0.5), (($low[2] + $high[2]) * 0.5))
$half = @((($high[0] - $low[0]) * 0.5), (($high[1] - $low[1]) * 0.5), (($high[2] - $low[2]) * 0.5))

# --- how much of the frame does a box fill, from here ----------------------------------------
#
# Projected properly: every corner of the box is put through the camera and the screen rectangle
# they land in is measured. Fitting a sphere round the box instead is easier and always frames
# too far out, which is the exact complaint this exists to answer.
function Get-Coverage([double[]]$eye, [double]$yawDeg, [double]$pitchDeg) {
    $yaw = $yawDeg * [Math]::PI / 180.0
    $pitch = $pitchDeg * [Math]::PI / 180.0
    $fx = [Math]::Cos($yaw) * [Math]::Cos($pitch); $fy = [Math]::Sin($pitch); $fz = [Math]::Sin($yaw) * [Math]::Cos($pitch)
    $rx = -[Math]::Sin($yaw); $ry = 0.0; $rz = [Math]::Cos($yaw)
    $ux = $fy * $rz - $fz * $ry; $uy = $fz * $rx - $fx * $rz; $uz = $fx * $ry - $fy * $rx

    $tan = [Math]::Tan($FovDegrees * 0.5 * [Math]::PI / 180.0)
    $aspect = $Width / $Height
    $minU = 9e9; $maxU = -9e9; $minV = 9e9; $maxV = -9e9; $anyFront = $false
    foreach ($cx in @($low[0], $high[0])) {
        foreach ($cy in @($low[1], $high[1])) {
            foreach ($cz in @($low[2], $high[2])) {
                $dx = $cx - $eye[0]; $dy = $cy - $eye[1]; $dz = $cz - $eye[2]
                $depth = $dx * $fx + $dy * $fy + $dz * $fz
                if ($depth -le 0.05) { continue }
                $anyFront = $true
                $su = ($dx * $rx + $dy * $ry + $dz * $rz) / ($depth * $tan * $aspect)
                $sv = ($dx * $ux + $dy * $uy + $dz * $uz) / ($depth * $tan)
                if ($su -lt $minU) { $minU = $su }; if ($su -gt $maxU) { $maxU = $su }
                if ($sv -lt $minV) { $minV = $sv }; if ($sv -gt $maxV) { $maxV = $sv }
            }
        }
    }
    if (-not $anyFront) { return 0.0 }
    # Clipped to the frame, so something half off the edge does not read as well framed.
    $w = [Math]::Min($maxU, 1.0) - [Math]::Max($minU, -1.0)
    $h = [Math]::Min($maxV, 1.0) - [Math]::Max($minV, -1.0)
    if ($w -le 0 -or $h -le 0) { return 0.0 }
    return ($w * 0.5) * ($h * 0.5)
}

# The distance along a direction at which the box just fills `Fill` of the frame. Solved rather
# than searched: the box's own half-extents give the angle it subtends, and the distance follows.
function Get-Distance([double]$yawDeg, [double]$pitchDeg) {
    $yaw = $yawDeg * [Math]::PI / 180.0
    $pitch = $pitchDeg * [Math]::PI / 180.0
    $fx = [Math]::Cos($yaw) * [Math]::Cos($pitch); $fy = [Math]::Sin($pitch); $fz = [Math]::Sin($yaw) * [Math]::Cos($pitch)
    $rx = -[Math]::Sin($yaw); $ry = 0.0; $rz = [Math]::Cos($yaw)
    $ux = $fy * $rz - $fz * $ry; $uy = $fz * $rx - $fx * $rz; $uz = $fx * $ry - $fy * $rx

    # How far the box reaches across the screen axes and along the view, whatever way it is turned.
    $reachR = [Math]::Abs($half[0] * $rx) + [Math]::Abs($half[1] * $ry) + [Math]::Abs($half[2] * $rz)
    $reachU = [Math]::Abs($half[0] * $ux) + [Math]::Abs($half[1] * $uy) + [Math]::Abs($half[2] * $uz)
    $reachF = [Math]::Abs($half[0] * $fx) + [Math]::Abs($half[1] * $fy) + [Math]::Abs($half[2] * $fz)

    $tan = [Math]::Tan($FovDegrees * 0.5 * [Math]::PI / 180.0)
    $aspect = $Width / $Height
    # Fill is a fraction of the frame's area; each axis gets its square root.
    $edge = [Math]::Sqrt($Fill)
    $needR = $reachR / ($edge * $tan * $aspect)
    $needU = $reachU / ($edge * $tan)
    return ([Math]::Max($needR, $needU) + $reachF)
}

# A view standing on a compass bearing and an elevation, at whatever distance frames the focus.
function New-Orbit([string]$name, [double]$bearing, [double]$rise, [double]$pull = 1.0) {
    $b = $bearing * [Math]::PI / 180.0
    $e = $rise * [Math]::PI / 180.0
    # Where the camera looks, from where it stands: the opposite of the direction it stands in.
    $yaw = ($bearing + 180.0)
    $pitch = -$rise
    $d = (Get-Distance $yaw $pitch) * $pull
    $ex = $centre[0] + [Math]::Cos($b) * [Math]::Cos($e) * $d
    $ey = $centre[1] + [Math]::Sin($e) * $d
    $ez = $centre[2] + [Math]::Sin($b) * [Math]::Cos($e) * $d
    $cover = Get-Coverage @($ex, $ey, $ez) $yaw $pitch
    [PSCustomObject]@{
        Name = $name
        Cam = ("{0:0.###},{1:0.###},{2:0.###},{3:0.###},{4:0.###}" -f $ex, $ey, $ez, $yaw, $pitch)
        Cover = $cover
    }
}

# A view from inside the focus, which cannot be framed by backing away because there is a wall
# behind you. It stands in the middle and turns.
function New-Inside([string]$name, [double]$yaw, [double]$pitch, [double]$offX = 0.0, [double]$offZ = 0.0) {
    $ex = $centre[0] + $offX * $half[0]
    $ey = $low[1] + 1.7 * $worldScale
    if ($ey -gt ($high[1] - 0.2 * $worldScale)) { $ey = $centre[1] }
    $ez = $centre[2] + $offZ * $half[2]
    [PSCustomObject]@{
        Name = $name
        Cam = ("{0:0.###},{1:0.###},{2:0.###},{3:0.###},{4:0.###}" -f $ex, $ey, $ez, $yaw, $pitch)
        Cover = (Get-Coverage @($ex, $ey, $ez) $yaw $pitch)
    }
}

$ring = @()
$bearings = @(@{n="south";b=270.0}, @{n="southwest";b=225.0}, @{n="west";b=180.0},
              @{n="northwest";b=135.0}, @{n="north";b=90.0}, @{n="northeast";b=45.0},
              @{n="east";b=0.0}, @{n="southeast";b=315.0})
foreach ($p in $bearings) { $ring += (New-Orbit $p.n $p.b 18.0) }
$ring += (New-Orbit "plan" 270.0 88.0)

$quick = @((New-Orbit "south" 270.0 14.0), (New-Orbit "southwest" 225.0 22.0), (New-Orbit "plan" 270.0 88.0))

# Close views: same bearings, pulled in so the focus overflows the frame and detail is readable.
$close = @()
foreach ($p in @(@{n="close-south";b=270.0}, @{n="close-southwest";b=225.0},
                 @{n="close-west";b=180.0}, @{n="close-northeast";b=45.0})) {
    $close += (New-Orbit $p.n $p.b 6.0 0.42)
}
$close += (New-Orbit "close-up-at-it" 270.0 -8.0 0.38)

$inside = @(
    (New-Inside "in-east"  0.0   0.0),
    (New-Inside "in-north" 90.0  0.0),
    (New-Inside "in-west"  180.0 0.0),
    (New-Inside "in-south" 270.0 0.0),
    (New-Inside "in-up"    90.0  55.0),
    (New-Inside "in-from-door" 90.0 6.0 0.0 -0.85)
)

switch ($Views) {
    "ring"   { $chosen = $ring }
    "quick"  { $chosen = $quick }
    "close"  { $chosen = $close }
    "inside" { $chosen = $inside }
    "all"    { $chosen = $ring + $close + $inside }
    default  { $chosen = $ring }
}
if ($Only -ne "") {
    $wanted = $Only -split ","
    $chosen = $chosen | Where-Object { $wanted -contains $_.Name }
}

$common = @("--clip-file", $Clip, "--clip-metre", "$Metre", "--no-vsync", "--no-update-check",
            "--width", "$Width", "--height", "$Height", "--screenshot-frame", "$Frames")
if ($Part -ne "") { $common += @("--clip-part", $Part) }
if ($PathTrace) { $common += "--pathtrace" }

$made = @()
$labels = @()
foreach ($view in $chosen) {
    $png = Join-Path $Out ($view.Name + ".png")
    $shot = $common + @("--screenshot", $png, "--cam", $view.Cam)
    $log = & $exe @shot 2>&1 | Out-String
    if (Test-Path $png) {
        $made += $png
        $labels += ("{0}  {1:P0} of frame" -f $view.Name, $view.Cover)
        Write-Host ("  {0,-18} {1,-40} fills {2,5:P0}" -f $view.Name, $view.Cam, $view.Cover)
    } else {
        Write-Host ("  {0,-18} FAILED" -f $view.Name)
        ($log -split "`n" | Select-String "error|ERROR" | Select-Object -First 4) | ForEach-Object { Write-Host ("      " + $_.Line.Trim()) }
    }
}

# One sheet with every view on it. Reading a dozen separate images is a dozen separate
# impressions; reading them side by side is a judgement.
if ($made.Count -gt 0) {
    Add-Type -AssemblyName System.Drawing
    $columns = [int][Math]::Ceiling([Math]::Sqrt($made.Count))
    $rows = [int][Math]::Ceiling($made.Count / $columns)
    $label = 18
    $sheet = [System.Drawing.Bitmap]::new([int]($columns * $Width), [int]($rows * ($Height + $label)))
    $canvas = [System.Drawing.Graphics]::FromImage($sheet)
    $canvas.Clear([System.Drawing.Color]::FromArgb(24, 24, 26))
    $font = [System.Drawing.Font]::new("Consolas", [single]11)
    $ink = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(230, 230, 235))
    for ($i = 0; $i -lt $made.Count; $i++) {
        $tile = [System.Drawing.Image]::FromFile($made[$i])
        $cx = [int](($i % $columns) * $Width)
        $cy = [int]([Math]::Floor($i / $columns) * ($Height + $label))
        $canvas.DrawImage($tile, $cx, [int]($cy + $label), [int]$Width, [int]$Height)
        $canvas.DrawString($labels[$i], $font, $ink, [single]$cx, [single]$cy)
        $tile.Dispose()
    }
    $canvas.Dispose()
    $sheetPath = Join-Path $Out "contact-sheet.png"
    $sheet.Save($sheetPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $sheet.Dispose()
    Write-Host ""
    Write-Host ("contact sheet: " + $sheetPath)
}
