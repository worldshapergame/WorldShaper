# Carve the shipped worlds ahead of time, so a player's first sight of one is a READ.
#
# `documentation/15-releases.md` is what this is for; D684 and D685 in the decision log are why it
# is shaped the way it is. In one line: sampling the facility from cold is minutes, reading a world
# back is under a second, and somebody has to have done the sampling once, somewhere. This is that
# somewhere. `.github/workflows/bake-world.yml` is the build server calling this file; everything
# below runs by hand exactly the same way, which is the point of it being a script and not a
# workflow step.
#
#   tools\bake_world.ps1                       bake what ships, from build\bin, and prove it reads
#   tools\bake_world.ps1 -Clips *              every world the shelf seeds, not just the facility
#   tools\bake_world.ps1 -GateOnly             only prove the worlds already there are read
#   tools\bake_world.ps1 -Game C:\unpacked     against an unpacked download rather than a build
#
# ---------------------------------------------------------------------------------------------
# FIVE THINGS THAT ARE NOT OPTIONAL. THE FIRST TWO WERE MEASURED HERE AND BOTH FAIL SILENTLY.
#
# 1. `--world`, NOT `--clip-file`. `main()` sends any run carrying `--clip-file` and no
#    `--screenshot` to `run_clip_tool`, which parses, samples, prints a measurement and exits -- it
#    has no world, no ladder and no `--bake-world` in it at all. So
#    `--clip-file clips\facility.clip --no-clip-cache --bake-world --settle --max-seconds 150`
#    samples the whole building at the authored metre and writes NOTHING, with exit code 0 and no
#    error. Measured: on `sky_test` that command took 4 s, exited 0, and produced no `.world`; the
#    same run through `--world` produced 1.08 MB. On the facility it burns minutes first, which is
#    what makes it look like a bake.
#
# 2. `--settle` DOES NOT END A RUN. Only `--screenshot` does. `--settle` marks the frame the world
#    stopped changing at and then the loop keeps drawing for ever; `--max-seconds` only forces the
#    settle mark and the screenshot, and with no screenshot asked for there is nothing to force.
#    Measured: a `--settle --max-seconds 60` bake was still going at frame 94,200 when it was
#    killed. It HAD baked -- the world is written at the fixed point and again on the way out -- so
#    a run of this shape leaves a correct file behind and never returns, which on a build server is
#    a job that hits its timeout and reports failure over a bake that worked.
#
# 3. `--no-clip-cache` ON THE BAKE, and NEVER on the gate. On the bake it empties this machine's
#    own cache path so a world it accumulated for itself cannot leak into the shipped one (D684) --
#    and it is the exact combination that used to write nothing at all, silently, which is why the
#    log line is checked for below rather than the exit code. On the GATE it would be a fault: the
#    shipped-world lookup lives inside `if (!source.empty() && !options_.no_clip_cache)`, so a gate
#    that passed it would skip the very read it is there to prove. "No cache of its own" is done
#    with a fresh LOCALAPPDATA instead, which is also what makes the run a fresh INSTALL.
#
# 4. BAKE WHAT SHIPS, BY THE PATH IT SHIPS AT. `tools/package.ps1` and the release workflow both
#    stage `build\bin\clips` into the zip, so that directory is the shipping layout and this script
#    runs the game with its working directory inside it. `--bake-world` writes beside the clip --
#    `clips\facility.clip` becomes `clips\facility.world` -- and `cmake/copy_clips.cmake` excludes
#    only `.clip.world` and `.clip.load`, so a `.world` baked here travels into the zip by itself.
#
# 5. THE GATE OPENS THE SHELF, NOT THE CLIP. D685 is the whole reason this file has a gate: the
#    bake was measured with `--clip-file`, a player opens `<data root>\worlds\facility.wsworld`,
#    and the feature did nothing whatsoever on the only path anybody uses while every number
#    reported about it was real. So the gate seeds a shelf into an empty data root and opens the
#    world off that shelf, by the path the shell would pass down, and it fails unless the log says
#    `opened the world shipped at`.
# ---------------------------------------------------------------------------------------------

[CmdletBinding()]
param(
    # The directory that SHIPS: WorldShaper.exe, shaders\ and clips\ beside each other. The build
    # puts one at build\bin and the zip is a copy of it, so either works and they are the same test.
    [string]$Game,

    # Which worlds, by stem. The default is the facility and only the facility, and that is a
    # decision about the size of the download rather than an oversight: the shelf seeds every
    # top-level clip, but the other nineteen are test scenes -- a sky, a pane of glass, a row of
    # lamps -- that build in seconds from cold, so baking them adds megabytes to every download to
    # save nobody anything. `-Clips *` takes the whole shelf; `-Clips facility,sampler` a list.
    [string[]]$Clips = @('facility'),

    # `k/n`, the same shape pages.yml shards its twelve with: this runner takes every nth clip
    # starting at k. One clip per runner is the useful setting, because the wall clock of a bake is
    # the SLOWEST SINGLE CLIP and no clip needs another clip's output.
    [string]$Shard = '0/1',

    # The deadline a single bake gets, and it is a BUDGET rather than a limit that is normally hit:
    # a world that settles early is written the moment it settles. An enclosed camera never reaches
    # a fixed point at all (D684), so without this the run is bounded only by `kSettleGiveUp`.
    [double]$Seconds = 150,

    # Where the baking camera stands, `x,y,z,yaw,pitch`. What gets baked is what a camera reached,
    # by design (D684, R11d) -- so this is the spawn view, and the ladder still runs for anywhere
    # a player walks that this camera never looked at.
    [string]$Cam = '0,0,0,-90,0',

    # The window the bake runs at, which is NOT cosmetic: refinement is driven by how many pixels a
    # node covers, so a bake at 640x360 sharpens less of the world than one at 1280x720 and writes
    # a smaller file. Ship-shaped by default.
    [int]$Width = 1280,
    [int]$Height = 720,

    # The most this is allowed to add to the download, over all the worlds baked in this run. A
    # release that quietly grows by hundreds of megabytes is a decision somebody should have made
    # on purpose, so going over stops the script rather than trimming something silently.
    [int]$BudgetMB = 64,

    # Bake even when the stamp beside the game says the world already there was made from this
    # exact game and this exact clip text.
    [switch]$Force,

    # Prove what is already there reads, and bake nothing. This is what a release gate wants when
    # the worlds arrived as an artifact from the machine that baked them.
    [switch]$GateOnly,

    # Bake and do not prove it. Only for a machine with no working Vulkan; the honest thing is to
    # say the bake is unproven when this is used.
    [switch]$NoGate,

    # A JSON summary -- every world, its seconds, its bytes, its hash -- for a workflow to publish.
    [string]$Report,

    # What the executable in -Game is called. There is one reason this is a parameter and it is a
    # real one: `build.bat` tells everybody to kill any stale `WorldShaper.exe` before building,
    # because the linker cannot overwrite a running one -- so on a machine where anything else is
    # building, a bake that takes minutes is a process sitting there with exactly the name everyone
    # has been told to kill. Two facility bakes died at exit -1 that way while a 7.8 s one came
    # through untouched. A copy of the shipping layout under another name is immune, and nothing in
    # the game depends on the name: every path it resolves is relative to whatever directory the
    # executable is in.
    [string]$Exe = 'WorldShaper.exe'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Resolve-Path (Join-Path $PSScriptRoot '..')
if (-not $Game) { $Game = Join-Path $repo 'build\bin' }
if (-not (Test-Path $Game)) { throw "there is no game at $Game -- build it, or pass -Game" }
$Game = (Resolve-Path $Game).Path

$exe = Join-Path $Game $Exe
$clipdir = Join-Path $Game 'clips'
if (-not (Test-Path $exe)) { throw "no $Exe in $Game" }
if (-not (Test-Path $clipdir)) { throw "no clips\ in $Game -- this is not a shipping layout" }

# Run the game and hand back its whole log and its exit code.
#
# Start-Process with the two streams redirected to files, rather than `& $exe ... 2>&1`, and that is
# not style. Windows PowerShell wraps every stderr line a native program writes in a NativeCommandError
# when it is redirected in-band -- and the game writes its WARN lines there -- so with
# `$ErrorActionPreference = 'Stop'` at the top of this file the first warning would end the script
# with an error that names PowerShell's own plumbing and not the game. Files also mean a bake that
# is watched from another window shows its progress as it happens.
#
# EVERY RUN GETS ITS OWN DATA ROOT, bake as well as gate. `crash.cpp` builds the root out of
# LOCALAPPDATA and `ui::default_root()` returns it, so pointing it somewhere private gives a run
# with no world cache, no shelf and no settings but its own. For the gate that is the whole
# mechanism -- it is what makes the run a fresh INSTALL. For the bake it is belt and braces on
# D684's point that the baking machine's own accumulated world must not reach the shipped one, and
# it stops a bake sharing a log file with anything else on the machine.
function Invoke-Game {
    param([string[]]$GameArgs, [string]$What, [string]$Root)

    $stamp = [Guid]::NewGuid().ToString('N').Substring(0, 8)
    $outFile = Join-Path ([IO.Path]::GetTempPath()) "ws-$What-$stamp.out"
    $errFile = Join-Path ([IO.Path]::GetTempPath()) "ws-$What-$stamp.err"
    $keep = $env:LOCALAPPDATA
    $watch = [Diagnostics.Stopwatch]::StartNew()
    try {
        if ($Root) { $env:LOCALAPPDATA = $Root }
        $proc = Start-Process -FilePath $exe -WorkingDirectory $Game -ArgumentList $GameArgs `
                              -NoNewWindow -Wait -PassThru `
                              -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    } finally {
        $env:LOCALAPPDATA = $keep
    }
    $watch.Stop()
    $log = ''
    if (Test-Path $outFile) { $log += (Get-Content $outFile -Raw) }
    if (Test-Path $errFile) { $log += (Get-Content $errFile -Raw) }
    Remove-Item $outFile, $errFile -Force -ErrorAction SilentlyContinue
    return [pscustomobject]@{
        log     = $log
        code    = $proc.ExitCode
        seconds = $watch.Elapsed.TotalSeconds
    }
}

function New-DataRoot {
    $root = Join-Path ([IO.Path]::GetTempPath()) ("ws-bake-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    return $root
}

# The stamps live BESIDE the game and not inside clips\, deliberately: package.ps1 and the release
# workflow both copy `clips` wholesale into the zip, so anything left in there is something every
# player downloads. This file is build-server bookkeeping and has no business travelling.
$stampfile = Join-Path $Game 'bake-keys.json'

function Read-Stamps {
    if (-not (Test-Path $stampfile)) { return @{} }
    try {
        $out = @{}
        $json = Get-Content $stampfile -Raw | ConvertFrom-Json
        foreach ($p in $json.PSObject.Properties) { $out[$p.Name] = [string]$p.Value }
        return $out
    } catch {
        return @{}
    }
}

function Write-Stamps {
    param($Stamps)
    ($Stamps | ConvertTo-Json) | Out-File $stampfile -Encoding ascii
}

# WHAT WOULD MAKE THIS BAKE COME OUT DIFFERENT, as one string.
#
# This is the part `shipped_stamp()` cannot do and says so in D684: a world baked here is keyed on
# the world FORMAT alone, because that is the only thing an install can compute, so a world baked
# by an older SAMPLER is indistinguishable from a current one to the game that reads it. Nothing in
# the game can catch that. This can: the executable's own hash moves whenever any of src\ is
# rebuilt, and the clip tree's hash moves whenever anybody edits a fragment. A stamp that still
# matches means re-baking would produce the same bytes, and a warm run is seconds.
function Get-BakeKey {
    $acc = [System.Text.StringBuilder]::new()
    [void]$acc.Append((Get-FileHash $exe -Algorithm SHA256).Hash)
    foreach ($f in Get-ChildItem $clipdir -Recurse -File | Sort-Object FullName) {
        if ($f.Extension -eq '.world') { continue }
        [void]$acc.Append($f.FullName.Substring($clipdir.Length))
        [void]$acc.Append((Get-FileHash $f.FullName -Algorithm SHA256).Hash)
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes($acc.ToString())
    $hash = [System.Security.Cryptography.SHA256]::Create().ComputeHash($bytes)
    return ([BitConverter]::ToString($hash) -replace '-', '').Substring(0, 32)
}

# Every world the shelf would seed, in the order `Shell::seed_worlds` walks them: every top-level
# `.clip`, and nothing from the fragment folders, because a fragment is not a world.
function Get-ShelfStems {
    return (Get-ChildItem $clipdir -Filter '*.clip' -File |
            Sort-Object Name | ForEach-Object { $_.BaseName })
}

$stems = @()
if ($Clips.Count -eq 1 -and $Clips[0] -eq '*') {
    $stems = @(Get-ShelfStems)
} else {
    $stems = @($Clips)
}
foreach ($stem in $stems) {
    if (-not (Test-Path (Join-Path $clipdir "$stem.clip"))) {
        throw "there is no clips\$stem.clip in $Game"
    }
}

if ($Shard -notmatch '^([0-9]+)/([0-9]+)$') { throw "-Shard wants k/n, got '$Shard'" }
$shardIndex = [int]$Matches[1]
$shardCount = [int]$Matches[2]
if ($shardCount -lt 1 -or $shardIndex -ge $shardCount) { throw "-Shard $Shard is not a slice" }
$mine = @()
for ($i = 0; $i -lt $stems.Count; $i++) {
    if (($i % $shardCount) -eq $shardIndex) { $mine += $stems[$i] }
}

Write-Host ""
Write-Host "bake  game    $Game"
Write-Host "bake  shard   $Shard of $($stems.Count) world(s): $($mine -join ', ')"
Write-Host "bake  camera  $Cam at ${Width}x$Height, deadline $Seconds s each, budget $BudgetMB MB"
Write-Host ""

# ------------------------------------------------------------------------------------------------
# The gate. Everything above is machinery; this is the part that decides whether a release happens.
#
# A fresh LOCALAPPDATA is the whole trick. `crash.cpp` builds the data root out of that variable
# and `ui::default_root()` returns it, so pointing it at an empty directory gives a run with no
# world cache of its own AND an empty worlds shelf -- which `Shell::seed_worlds` then fills from
# the clips beside the executable, exactly as a first launch on a player's computer does. The world
# is then opened by the path the shell hands down, `<root>\worlds\<stem>.wsworld`, so what is being
# tested is the player's route and not the developer's.
function Test-BakedWorldIsRead {
    param([string]$Stem)

    $root = New-DataRoot
    $shelf = Join-Path $root "WorldShaper\worlds\$Stem.wsworld"
    $shot = Join-Path $root 'gate.png'
    # NO `--no-clip-cache` HERE, and that is not an omission -- see the head of this file.
    # `--screenshot` is what ends the run and keeps it out of the clip tool, and it is also the
    # only proof that a frame was drawn out of what was read. The shelf file does not exist yet
    # when this starts: `Shell::seed_worlds` runs before any world is opened and puts it there,
    # which is the same order a player's first launch has.
    $run = Invoke-Game -What "gate-$Stem" -Root $root -GameArgs @(
        '--world', $shelf, '--no-title', '--screenshot', $shot, '--screenshot-frame', '3',
        '--no-update-check', '--no-vsync', '--width', '640', '--height', '360',
        '--max-seconds', '180')

    Write-Host $run.log

    # THE EVIDENCE BEFORE THE EXIT CODE, and the order is the point.
    #
    # This gate asks one question -- was the shipped world read -- and the log answers it. Leading
    # with the exit code answers a different question and answers it first, so a run that read the
    # world, drew its frame and then died on the way out reports as "the gate failed", which sends
    # the next person to look at the bake. That is not hypothetical: on the machine this was written
    # on, every gate run that lived more than a few seconds under the name `WorldShaper.exe` was
    # killed at exit -1 by something else obeying build.bat's "kill any stale WorldShaper.exe
    # first", having already printed every line below. The checks still FAIL on a bad exit -- a
    # crash after the read is a real fault -- but they fail saying which half went wrong.
    #
    # And the line itself is the strongest thing available, not the weakest: a world that was
    # SAMPLED instead of read produces a perfectly good screenshot and a perfectly good voxel
    # count, which is exactly how D685 came to be reported as working. A picture proves nothing
    # here. This line proves everything.
    if ($run.log -notmatch "opened the world shipped at '([^']+)'") {
        Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
        throw ("$Stem : the shipped world was NOT read. The game opened the shelf world and " +
               "sampled it from the clip instead, which is a release shipping a file no player " +
               "ever opens. Look for 'opened the world shipped at' in the log above.")
    }
    $from = $Matches[1]

    # And that there was a world in it. A hit on a file that decoded to nothing is still a hit.
    $built = [regex]::Match($run.log,
        "loaded from cache in [\d.]+ ms[^:]*: (\d+) chunks, (\d+) solid voxels")
    if (-not $built.Success) { throw "$Stem : nothing said the world came back from a file" }
    if ([int64]$built.Groups[2].Value -le 0) { throw "$Stem : the shipped world holds no voxels" }

    $shot = Join-Path $root 'gate.png'
    if (-not (Test-Path $shot)) { throw "$Stem : the gate read the world and drew no frame" }

    # Last, so that the message can say what already went right.
    if ($run.code -ne 0) {
        throw ("$Stem : the shipped world WAS read and the frame WAS drawn, and then the run " +
               "exited $($run.code). The bake is not the suspect; look at what happened on the " +
               "way out, or at whether something else on this machine killed the process.")
    }

    $ready = [regex]::Match($run.log, "everything ready\s+\[t\+([\d.]+) ms\]")
    $readyMs = 0.0
    if ($ready.Success) { $readyMs = [double]$ready.Groups[1].Value }

    Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host ("gate  $Stem read '{0}': {1} chunks, {2} solid voxels, everything ready in {3:N0} ms" `
                -f $from, $built.Groups[1].Value, $built.Groups[2].Value, $readyMs)
    return [pscustomobject]@{
        stem     = $Stem
        from     = $from
        chunks   = [int64]$built.Groups[1].Value
        voxels   = [int64]$built.Groups[2].Value
        ready_ms = $readyMs
    }
}

# ------------------------------------------------------------------------------------------------

$key = Get-BakeKey
$stamps = Read-Stamps
$rows = @()

foreach ($stem in $mine) {
    $world = Join-Path $clipdir "$stem.world"
    $warm = $false

    if ($GateOnly) {
        if (-not (Test-Path $world)) { throw "$stem : -GateOnly, and there is no clips\$stem.world" }
    } else {
        $known = ''
        if ($stamps.ContainsKey($stem)) { $known = $stamps[$stem] }
        if (-not $Force -and (Test-Path $world) -and $known -eq $key) {
            $warm = $true
            Write-Host ("bake  $stem is already baked from this exact game and clip " +
                        "({0:N2} MB); reusing it" -f ((Get-Item $world).Length / 1MB))
        }
    }

    $spent = 0.0
    $nodes = ''
    if (-not $GateOnly -and -not $warm) {
        # Deleted first, always. A bake that fails must not leave the last one standing and be
        # mistaken for a success by the size check below.
        Remove-Item $world -Force -ErrorAction SilentlyContinue
        if ($stamps.ContainsKey($stem)) { $stamps.Remove($stem) }
        Write-Stamps -Stamps $stamps

        Write-Host "bake  $stem ..."
        $shot = Join-Path ([IO.Path]::GetTempPath()) "ws-bake-$stem.png"
        # `--world` and not `--clip-file`, and `--screenshot` so the run ENDS: see 1 and 2 at the
        # head of this file. `--settle` waits for the ladder to have nothing left it can do from
        # this camera and the shot is taken three frames after that; `--max-seconds` forces both
        # for a camera that never reaches a fixed point. The world is written at the fixed point
        # and again on the way out, so either route leaves the file behind.
        $bakeRoot = New-DataRoot
        $run = Invoke-Game -What "bake-$stem" -Root $bakeRoot -GameArgs @(
            '--world', "clips\$stem.clip", '--no-title', '--no-clip-cache', '--bake-world',
            '--cam', $Cam, '--settle', '--screenshot', $shot, '--screenshot-frame', '3',
            '--max-seconds', "$Seconds", '--no-update-check', '--no-vsync',
            '--width', "$Width", '--height', "$Height")
        $spent = $run.seconds
        Write-Host $run.log
        Remove-Item $shot -Force -ErrorAction SilentlyContinue
        Remove-Item $bakeRoot -Recurse -Force -ErrorAction SilentlyContinue
        if ($run.code -ne 0) { throw "$stem : the bake run failed (exit $($run.code))" }

        # THE LOG LINE, not the exit code, because the failure this is guarding against exited
        # zero after four minutes of work and wrote nothing at all (D684).
        $wrote = [regex]::Match($run.log,
            "baked the world for shipping: '([^']+)' \((\d+) MB\), (\d+) of (\d+) nodes")
        if (-not $wrote.Success) {
            throw ("$stem : the run finished and never baked anything. The game says " +
                   "'baked the world for shipping' when it does, and there is no such line above.")
        }
        if (-not (Test-Path $world)) { throw "$stem : said it baked, and there is no $world" }
        $nodes = "$($wrote.Groups[3].Value) of $($wrote.Groups[4].Value)"
        if ($run.log -match 'gave up waiting for the world to settle') {
            Write-Host ("bake  $stem ran out of its $Seconds s and baked what it had reached -- a " +
                        "smaller world, not a broken one. Raise -Seconds to bake more of it.")
        }
    }

    $bytes = (Get-Item $world).Length
    $rows += [pscustomobject]@{
        stem    = $stem
        bytes   = $bytes
        mb      = [math]::Round($bytes / 1MB, 2)
        seconds = [math]::Round($spent, 1)
        nodes   = $nodes
        warm    = [bool]$warm
        sha256  = (Get-FileHash $world -Algorithm SHA256).Hash
        read    = $null
    }

    if (-not $GateOnly -and -not $warm) {
        $stamps[$stem] = $key
        Write-Stamps -Stamps $stamps
    }
}

# The budget, before the gate, because a download that is too big is a decision and not a defect
# and there is no point spending three minutes proving an oversized world reads.
$totalBytes = 0
foreach ($row in $rows) { $totalBytes += $row.bytes }
$totalMB = [math]::Round($totalBytes / 1MB, 2)
if ($totalMB -gt $BudgetMB) {
    throw ("the baked worlds come to $totalMB MB and the budget is $BudgetMB MB. That is how much " +
           "bigger every download gets. Raise -BudgetMB on purpose, or bake fewer worlds, or bake " +
           "from a tighter camera -- but decide it rather than shipping it.")
}

if (-not $NoGate) {
    foreach ($row in $rows) { $row.read = Test-BakedWorldIsRead -Stem $row.stem }
}

Write-Host ""
Write-Host "world                        MB    bake s   ready ms      voxels  nodes"
foreach ($row in $rows) {
    $readyMs = 0.0
    $voxels = 0
    if ($null -ne $row.read) { $readyMs = $row.read.ready_ms; $voxels = $row.read.voxels }
    $note = ''
    if ($row.warm) { $note = ' (warm)' }
    Write-Host ("{0,-20} {1,8:N2}  {2,8:N1}  {3,9:N0}  {4,10:N0}  {5}{6}" -f `
                $row.stem, $row.mb, $row.seconds, $readyMs, $voxels, $row.nodes, $note)
}
Write-Host ("{0,-20} {1,8:N2}   of a {2} MB budget" -f 'total', $totalMB, $BudgetMB)
Write-Host ""
if ($NoGate) {
    Write-Host "NOT PROVEN: -NoGate was passed, so nothing above has been read back by the game."
} else {
    Write-Host "every world above was opened OFF THE SHELF, from an empty data root, and READ."
}

if ($Report) {
    $summary = [pscustomobject]@{
        game     = $Game
        key      = $key
        shard    = $Shard
        camera   = $Cam
        seconds  = $Seconds
        total_mb = $totalMB
        gated    = (-not $NoGate)
        worlds   = $rows
    }
    ($summary | ConvertTo-Json -Depth 6) | Out-File $Report -Encoding ascii
    Write-Host "wrote $Report"
}
