# The numbers a renderer change has to beat.
#
# # Why this exists
#
# Every performance figure in documentation/ was taken by hand, once, with whatever camera and
# resolution was in front of somebody at the time, and then quoted for months. Two of them are
# now known to be wrong rather than merely old: documentation/19-auto-quality.md records the
# path tracer at 2.495 ms outdoors and 19.988 ms enclosed, and the pass those were read from was
# not being timed at all — the profiler had no stack, so the cloud pass nested inside the tracer
# overwrote its slot and the tracer's own dispatch fell outside every timestamp in the frame. It
# read as 0.253 ms of GPU work against 37.7 ms of wall clock and nobody had reason to look.
#
# So: a fixed grid of cameras, resolutions and modes, run by one command, written to a file, and
# diffed against the last one. A figure nobody can reproduce is not a measurement.
#
# # Running it
#
#   tools\baseline.ps1 -Out docs\baseline-r0.csv           # the whole grid
#   tools\baseline.ps1 -Views enclosed,outdoor -Sizes deck # a quick look while working
#   tools\baseline.ps1 -Compare docs\baseline-r0.csv       # what has moved, and by how much
#
# The full grid is seven views times three resolutions and takes a few minutes.
# Narrow it while iterating; run all of it before claiming a stage is done.

param(
    # Named cameras, comma separated, or "all".
    [string]$Views = "all",
    # deck (1280x800), dev (2560x1440), high (3840x2160), or "all".
    [string]$Sizes = "all",
    # Realtime only. R3d deleted the reference path tracer, so "pathtrace" here would pass a flag
    # the binary no longer parses -- and an unknown flag is IGNORED, which means those runs would
    # come back looking like perfectly good measurements of a mode that does not exist. Trap 15,
    # arriving through the harness rather than through the engine.
    [string]$Modes = "realtime",
    [string]$Clip = "",
    # Frames rendered before the shot. The first half is thrown away as warm-up by the engine
    # itself and the rest is averaged, so this is twice the averaging window.
    [int]$Frames = 120,
    [int]$Quality = 7,
    [string]$Out = "",
    [string]$Compare = "",
    [switch]$NoSpeckle,
    # "vx,vy,vz,vyaw" in metres and degrees a second: the camera moves every frame, so the shot
    # is of the moving picture rather than a settled one.
    #
    # THE case that matters, and the one every still camera flatters. src/app/main.cpp resets the
    # per-pixel accumulator whenever the camera moves at all, so a moving frame is one sample per
    # pixel however long you have been standing there. A grid of still cameras measures a
    # photograph nobody takes; this measures the game.
    [string]$Fly = "",
    # How far a number may move before it is called a regression rather than noise.
    [double]$Tolerance = 0.03
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\bin\WorldShaper.exe"
if (-not (Test-Path $exe)) { throw "no WorldShaper.exe at $exe - run build.bat first" }
. (Join-Path $PSScriptRoot "_measure.ps1")
# The cameras and resolutions, shared with tools\facecount.ps1 so the two cannot drift apart.
. (Join-Path $PSScriptRoot "_grid.ps1")
$allViews = $WsViews
$allSizes = $WsSizes

# Named apart from the parameters on purpose, and it is not a style choice. See the note above
# Select-Named in _grid.ps1: `$views` and the parameter `$Views` are one variable, and a
# `[string]` parameter keeps that constraint for life, so assigning the array back to it rejoins
# it into a single space-separated name and the harness silently runs one view instead of two.
$viewList = Select-Named $allViews $Views "view"
$sizeList = Select-Named $allSizes $Sizes "size"
$modeList = @($Modes -split "[,\s]+" | Where-Object { $_ -ne "" })

$shots = Join-Path $env:TEMP "ws-baseline"
New-Item -ItemType Directory -Force -Path $shots | Out-Null

# --- one run ---------------------------------------------------------------------------------

function Invoke-Run([string]$mode, [string]$view, [string]$size) {
    $wh = $allSizes[$size]
    $png = Join-Path $shots ("{0}-{1}-{2}.png" -f $mode, $view, $size)
    if (Test-Path $png) { Remove-Item $png }

    # Not $args, which is an automatic variable: assigning to it inside a function shadows the
    # one PowerShell maintains and is a trap for whoever edits this next.
    # --settle, always, and it is not optional for anything written to a csv.
    #
    # The scene sharpens region by region in the background and its cache is only written when the
    # last region lands, which a scripted run never reaches - so without this the window opens at
    # frame nought against however much world happened to exist by then, and a FASTER build gets
    # there sooner with LESS in front of it. Two runs of one binary differed on 52,292 pixels that
    # way. See D229-D231.
    $runArgs = @("--settle",
                 "--screenshot", $png, "--screenshot-frame", "$Frames",
                 "--width", "$($wh[0])", "--height", "$($wh[1])",
                 "--cam", $allViews[$view], "--quality", "$Quality",
                 "--no-vsync", "--no-update-check", "--no-auto-quality")
    if ($mode -eq "pathtrace") {
        throw "the reference path tracer was deleted by R3d; there is no --pathtrace to measure"
    }
    if ($Clip -ne "") { $runArgs += @("--clip-file", $Clip) }
    if ($Fly -ne "") { $runArgs += @("--fly", $Fly) }

    # Deliberately not letting a stderr line become a terminating error. Windows PowerShell wraps
    # each stderr line from a native program in an ErrorRecord, so with ErrorActionPreference at
    # Stop the harness dies on an ordinary renderer warning.
    $before = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $log = (& $exe @runArgs 2>&1 | Out-String)
    $ErrorActionPreference = $before

    $row = [ordered]@{
        mode = $mode; view = $view; size = $size
        width = $wh[0]; height = $wh[1]
        fly = $Fly
        total_ms = 0.0; worst_ms = 0.0; cpu_ms = 0.0
        dominant = ""; dominant_ms = 0.0
        nodes = 0; leaves = 0; world_chunks = 0
        # What the figures on this row were measured against. Two rows are comparable only if these
        # match; before D231 there was no way to tell, and they frequently did not.
        #
        # The CONTENT HASH is the scene's identity (D243) and the columns beside it are a
        # description of it. Until R1e's fifth slice this row recorded only the voxel count and the
        # region tally, and the regex for those wanted the ladder's "N of M regions" -- which a
        # settled world does not print. So twenty-one rows of the last baseline recorded a scene of
        # nought voxels, every comparison against them passed the "same scene?" gate by comparing
        # nought with nought, and the one row that did carry a count was the only one that ever
        # objected. Trap 10 in the harness rather than in the engine.
        content = ""; solid_voxels = 0; regions = ""
        speckle = 0.0; fireflies = 0
        frames = 0
    }

    if ($log -match "total GPU mean\s+([0-9.]+)\s+ms,\s+worst\s+([0-9.]+)\s+ms,\s+over\s+(\d+)") {
        $row.total_ms = [double]$Matches[1]
        $row.worst_ms = [double]$Matches[2]
        $row.frames = [int]$Matches[3]
    }
    if ($log -match "CPU\s+([0-9.]+)\s+ms") { $row.cpu_ms = [double]$Matches[1] }
    # What the pool holds, where this used to read what chunk residency held. The line is the
    # one the engine already prints at every screenshot; a second line saying the same thing is a
    # second thing to keep in step with the harness.
    if ($log -match "node pool:\s+(\d+)\s+nodes,\s+(\d+)\s+leaves") {
        $row.nodes = [int]$Matches[1]
        $row.leaves = [int]$Matches[2]
    }
    # Two forms: "N of M regions" while the clip ladder is still climbing, and a sentence saying
    # it has finished when it has. Both end with the content hash, which is the thing to compare.
    if ($log -match "scene: (\d+) chunks, (\d+) solid voxels, (.+?), content ([0-9a-f]+)") {
        $row.world_chunks = [int]$Matches[1]
        $row.solid_voxels = [long]$Matches[2]
        $row.regions = $Matches[3]
        $row.content = $Matches[4]
    }

    # The pass that costs the most, which is the only one worth naming in a summary. Nested
    # passes are indented in the log and are excluded here: a pass inside another is already
    # inside its parent's number, and naming a child as the dominant pass would double count.
    # The engine writes the category, four spaces, then the label — and the label is indented two
    # spaces per level of nesting. So the indent is captured rather than eaten, which is the only
    # way to tell a child pass from a sibling that happens to sort next to it.
    $passes = @()
    foreach ($line in ($log -split "`r?`n")) {
        if ($line -match "^\[[^\]]+\]\s+frame\s{4}(\s*)(\S+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s*$") {
            $passes += [PSCustomObject]@{
                Name   = $Matches[2]
                Ms     = [double]$Matches[3]
                Nested = ($Matches[1].Length -gt 0)
            }
        }
    }
    $top = $passes | Where-Object { -not $_.Nested } | Sort-Object Ms -Descending | Select-Object -First 1
    if ($top) { $row.dominant = $top.Name; $row.dominant_ms = $top.Ms }

    if ((Test-Path $png) -and (-not $NoSpeckle)) {
        $m = Measure-Speckle $png
        $row.speckle = [Math]::Round($m.Speckle, 2)
        $row.fireflies = $m.Fireflies
    } elseif (-not (Test-Path $png)) {
        Write-Host ("    no image written - the run failed") -ForegroundColor Red
        ($log -split "`r?`n" | Select-String "error|ERROR|FATAL" | Select-Object -First 3) |
            ForEach-Object { Write-Host ("      " + $_.Line.Trim()) -ForegroundColor Red }
    }
    return [PSCustomObject]$row
}

# --- the grid --------------------------------------------------------------------------------

$rows = @()
$count = $modeList.Count * $viewList.Count * $sizeList.Count
$done = 0
Write-Host ("{0} runs: {1} modes x {2} views x {3} sizes" -f $count, $modeList.Count, $viewList.Count, $sizeList.Count)
Write-Host ""
Write-Host ("  {0,-10} {1,-9} {2,-5} {3,9} {4,9} {5,9}  {6,-11} {7,8}" -f
            "mode", "view", "size", "mean ms", "worst", "cpu ms", "dominant", "speckle")

foreach ($mode in $modeList) {
    foreach ($view in $viewList) {
        foreach ($size in $sizeList) {
            $done++
            $r = Invoke-Run $mode $view $size
            $rows += $r
            Write-Host ("  {0,-10} {1,-9} {2,-5} {3,9:N3} {4,9:N3} {5,9:N3}  {6,-11} {7,8:N1}" -f
                        $r.mode, $r.view, $r.size, $r.total_ms, $r.worst_ms, $r.cpu_ms,
                        $r.dominant, $r.speckle)
        }
    }
}

# --- what it means ---------------------------------------------------------------------------

Write-Host ""
foreach ($mode in $modeList) {
    foreach ($size in $sizeList) {
        # `$outside` and not `$out`, for the reason given above `$viewList`: `$out` is the same
        # variable as the `-Out` parameter, which is `[string]`, so a row object assigned to it
        # would be stringified — and then written to a file whose name is the row.
        $inside  = $rows | Where-Object { $_.mode -eq $mode -and $_.view -eq "enclosed" -and $_.size -eq $size }
        $outside = $rows | Where-Object { $_.mode -eq $mode -and $_.view -eq "outdoor" -and $_.size -eq $size }
        if ($inside -and $outside -and $outside.total_ms -gt 0) {
            Write-Host ("{0,-10} {1,-5} enclosed / outdoor = {2:N2}x   ({3:N2} ms against {4:N2} ms)" -f
                        $mode, $size, ($inside.total_ms / $outside.total_ms), $inside.total_ms,
                        $outside.total_ms)
        }
    }
}

# How the cost moves with resolution, which is the whole claim the rewrite makes. Lighting that
# is per face does not scale with the screen; lighting that is per pixel does.
foreach ($mode in $modeList) {
    foreach ($view in $viewList) {
        $a = $rows | Where-Object { $_.mode -eq $mode -and $_.view -eq $view -and $_.size -eq "dev" }
        $b = $rows | Where-Object { $_.mode -eq $mode -and $_.view -eq $view -and $_.size -eq "high" }
        if ($a -and $b -and $a.total_ms -gt 0) {
            Write-Host ("{0,-10} {1,-9} 1440p -> 4K = {2:N2}x  (pixels are 2.25x)" -f
                        $mode, $view, ($b.total_ms / $a.total_ms))
        }
    }
}

if ($Out -ne "") {
    $dir = Split-Path -Parent $Out
    if ($dir -ne "" -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $rows | Export-Csv -Path $Out -NoTypeInformation -Encoding utf8
    Write-Host ""
    Write-Host ("written: " + $Out)
}

# --- against last time ------------------------------------------------------------------------

if ($Compare -ne "") {
    if (-not (Test-Path $Compare)) { throw "no baseline at $Compare" }
    $was = Import-Csv $Compare
    Write-Host ""
    Write-Host ("against {0}, flagging moves over {1:P0}" -f $Compare, $Tolerance)
    Write-Host ("  {0,-10} {1,-9} {2,-5} {3,9} {4,9} {5,8}" -f "mode", "view", "size", "was", "now", "change")
    $regressed = 0
    foreach ($now in $rows) {
        $then = $was | Where-Object {
            $_.mode -eq $now.mode -and $_.view -eq $now.view -and $_.size -eq $now.size
        } | Select-Object -First 1
        if (-not $then) { continue }

        # Refuse to call it a change if the two rows were measured against different worlds.
        #
        # This is the check that was missing rather than failing. A csv taken before D231 has no
        # scene columns at all, which is itself the warning: those rows were taken against whatever
        # had been sharpened by the time the run reached its screenshot frame.
        #
        # The hash first, because it IS the scene rather than a description of it, and a row with
        # no hash is refused outright rather than falling through to a voxel count that may itself
        # be nought for a reason nobody meant. A missing scene has to be louder than a matching
        # one, or a harness that has stopped reading the log reads exactly like a clean run.
        $thenContent = if ($null -ne $then.PSObject.Properties['content']) { $then.content } else { "" }
        if ([string]::IsNullOrEmpty($thenContent)) {
            Write-Host ("  {0,-10} {1,-9} {2,-5} {3}" -f $now.mode, $now.view, $now.size,
                        "the old row records no scene - not comparable (D231, D243)") -ForegroundColor Yellow
            continue
        }
        if ([string]::IsNullOrEmpty($now.content)) {
            Write-Host ("  {0,-10} {1,-9} {2,-5} {3}" -f $now.mode, $now.view, $now.size,
                        "this run printed no scene line - the harness read nothing") -ForegroundColor Red
            continue
        }
        if ($thenContent -ne $now.content) {
            Write-Host ("  {0,-10} {1,-9} {2,-5} was world {3}, now {4} - not comparable" -f
                        $now.mode, $now.view, $now.size, $thenContent, $now.content) -ForegroundColor Yellow
            continue
        }

        $old = [double]$then.total_ms
        if ($old -le 0) { continue }
        $change = ($now.total_ms - $old) / $old
        if ([Math]::Abs($change) -lt $Tolerance) { continue }
        if ($change -gt 0) { $regressed++ }
        $colour = if ($change -gt 0) { "Red" } else { "Green" }
        Write-Host ("  {0,-10} {1,-9} {2,-5} {3,9:N3} {4,9:N3} {5,8:P1}" -f
                    $now.mode, $now.view, $now.size, $old, $now.total_ms, $change) -ForegroundColor $colour
        $regressed = $regressed
    }
    if ($regressed -eq 0) { Write-Host "  nothing regressed" -ForegroundColor Green }
}
