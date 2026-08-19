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
#   tools\bake_world.ps1 -RefineAll:$false     the control arm: what ONE CAMERA reached, the old way
#
# ---------------------------------------------------------------------------------------------
# SEVEN THINGS THAT ARE NOT OPTIONAL. THE FIRST TWO WERE MEASURED HERE AND BOTH FAIL SILENTLY.
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
#
# 6. `--refine-all`, OR THE FILE IS ONE CAMERA'S WALK AND NOT THE WORLD. `--bake-world` on its own
#    saves what the refinement ladder REACHED, and the ladder is driven by what a camera can see
#    and how many pixels each node covers -- so a bake from the spawn view writes a world that is
#    complete where that camera stood and coarse everywhere else. That is the floor with no walls
#    the user photographed, and every bake before this one deserved it. `--refine-all` takes the
#    two visibility terms out of the ladder -- the behind-the-camera demotion in the sweep and the
#    `next_keen > kRefineSplitAt` gate on the split -- so every node is refined to the clip's own
#    detail regardless of where the camera is pointing, and then the file IS the world.
#
#    It costs what it sounds like it costs. On the estate it is hundreds of thousands of nodes
#    instead of the ~29,000 a two-minute spawn-view bake reaches: many minutes rather than
#    seconds, and tens of megabytes rather than three. `-RefineAll:$false` is the control arm and
#    bakes the old way.
#
# 7. ONE RUN CANNOT FINISH THE ESTATE, AND THE LIMIT IS FRAMES RATHER THAN SECONDS. `kSettleGiveUp`
#    in main.cpp is 30,000 frames: past that the run stops waiting to settle, takes its screenshot
#    and leaves, whatever `--max-seconds` says. Measured here -- a facility bake given a 1,800 s
#    deadline ended after 30,001 frames and 119 s with 26,496 of 36,427 nodes, and the same work
#    came out of two 60 s passes, so the ceiling is the cap and not the clock.
#
#    So a whole-world bake is a LOOP. Each pass resumes from the last -- the game reads the world
#    the previous pass wrote, says `cached world has N of M leaves ... carrying on from here`, and
#    sharpens more of it -- and the loop stops the moment a pass reports every node done, or when
#    a pass adds nothing, or when -Passes runs out. Measured resuming: 18,816 of 27,315 after one
#    pass, 26,496 of 36,427 after two, each from the one before it.
#
#    AND THE CAP IS ON `--settle`, NOT ON THE GAME. The give-up branch is inside
#    `if (options_.settle && !settled_seen_)`, so a run with no `--settle` never enters it and is
#    bounded by `--max-seconds` alone. That is why the first pass settles -- so a small clip stops
#    the moment it is finished -- and every pass after it does not, and is worth its whole
#    deadline instead of the ~119 s that 30,000 frames comes to on a fast card.
#
#    That is why the passes SHARE a data root and why `--no-clip-cache` comes off when there is
#    more than one of them: a pass that empties the cache path cannot resume from the pass before
#    it. What D684 asks of `--no-clip-cache` -- that the baking machine's own accumulated world
#    must not leak into the shipped one -- is still had, and had by the two things that give it
#    directly: the root is created empty for this bake and thrown away after it, and the shipped
#    `.world` is deleted before the first pass. There is nothing left to leak from.
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

    # Where the baking camera stands, `x,y,z,yaw,pitch`. With -RefineAll on -- which is the default
    # -- this decides almost nothing about what is IN the world, because every node is refined
    # whether the camera can see it or not. It still decides what the settle frame looks at and
    # therefore what the run's screenshot shows, so it stays the spawn view.
    [string]$Cam = '0,0,0,-90,0',

    # REFINE EVERY NODE, NOT THE ONES A CAMERA COULD SEE. This is the difference between shipping
    # the world and shipping one walk through it, and it is on by default because a release that
    # ships the second is the complaint this whole feature exists to answer. See 6 at the head of
    # this file. `-RefineAll:$false` is the control arm: same script, same gate, the old bake.
    [bool]$RefineAll = $true,

    # THE MOST PASSES ONE WORLD GETS, and it is a ceiling rather than a count: the loop stops the
    # moment a pass says every node is done, or the moment a pass adds nothing to the one before
    # it. A small clip finishes in one and never sees the rest. The estate cannot finish in one --
    # see 7 at the head of this file, the cap is 30,000 FRAMES and no deadline raises it -- so a
    # whole-world bake is this loop, and the number here is how long anybody is prepared to let it
    # run: passes times -Seconds is the worst case, and finishing early is the normal case.
    [int]$Passes = 40,

    # FAIL IF THE WORLD IS NOT ALL THERE. Off by default so that baking by hand is still a thing
    # somebody can do in two minutes and look at, and ON in the release workflow, where a partial
    # world is the exact defect this feature exists to remove and shipping one quietly is how it
    # got shipped the first three times. It has nothing to say about a `-RefineAll:$false` bake,
    # which is partial on purpose, so it refuses that combination rather than failing it.
    # THE WALL CLOCK THE WHOLE RUN GETS, in minutes, 0 for none. `-Passes` times `-Seconds` is a
    # worst case that leaves out how long a run takes to START, and on a machine with a software
    # Vulkan that is a minute a pass rather than five seconds -- so the arithmetic that fits inside
    # a build server's job timeout is wrong exactly where it matters. This is the same limit stated
    # in the units the job timeout is stated in. Reaching it stops the loop with what it has, which
    # `-RequireWhole` then fails on, saying which world and how far it got; being killed by the job
    # timeout instead says nothing and leaves no report behind.
    [int]$TotalMinutes = 0,

    [switch]$RequireWhole,

    # WHERE ELSE THE GATE STANDS. `x,y,z,yaw,pitch` each, and an empty string is the world's own
    # spawn view -- which is where the BAKE stood, and is therefore the one viewpoint that cannot
    # tell you whether the world is all there. The other three are chosen to be nowhere the bake
    # ever pointed: above the roof looking down, sixty metres south of the great steps, and on the
    # far side of the block at head height. The estate is `bounds -53 -2 -46 .. 72.5 35.5 64.5`
    # and the dome tops out at 18.2 m, so 45 m up is above all of it and z = -76 is sixty metres
    # clear of the steps at z = -15.7. Yaw 90 looks north, -90 south; +z is north.
    [string[]]$GateCams = @('', '0,45,0,90,-70', '0,14,-76,90,-5', '0,6,40,-90,-4'),

    # Keep the gate's pictures here instead of throwing them away with the data root. Nothing
    # depends on them -- a picture proves nothing about a world having been READ, which is D685's
    # own finding -- but a person looking at four frames can see a missing wall in a second.
    [string]$GateShots,

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

if ($RequireWhole -and -not $RefineAll) {
    throw ("-RequireWhole with -RefineAll:`$false asks for a whole world from the arm that bakes " +
           "one camera's walk on purpose. Pick one.")
}

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
    # AND HOW IT WAS BAKED, because the source is only half of what decides the bytes. The ladder
    # is driven by the camera, the window and whether visibility gates it at all, so a world baked
    # with -RefineAll:$false is a different world from the same exe and the same clips -- and the
    # warm path would otherwise hand the old, camera-shaped file straight back to a run that asked
    # for the whole thing. D673's own lesson, one layer up: the key must name the arm that built it.
    [void]$acc.Append("|refine_all=$([int][bool]$RefineAll)|cam=$Cam|view=${Width}x$Height")
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
if ($RefineAll) {
    Write-Host "bake  detail  --refine-all: EVERY node, not the ones this camera can see"
} else {
    Write-Host "bake  detail  -RefineAll:`$false -- what the ladder REACHED from this camera only"
}
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
        '--max-seconds', '600')

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

    Write-Host ("gate  $Stem read '{0}': {1} chunks, {2} solid voxels, everything ready in {3:N0} ms" `
                -f $from, $built.Groups[1].Value, $built.Groups[2].Value, $readyMs)

    # ------------------------------------------------------------------------------------------
    # AND FROM CAMERAS THE BAKE NEVER CAME FROM, which is the half a one-camera check cannot have.
    #
    # Everything above proves the file was READ. It says nothing about whether the file has the
    # far side of the building in it, because the run that proved it stood exactly where the bake
    # stood -- and a world baked without `--refine-all` is complete precisely there and coarse
    # everywhere else. That is the shape of the whole complaint: the floor is present because the
    # camera was on it, and the walls are not because the camera never looked at them.
    #
    # So the gate opens the same shelf world again from somewhere else: above the roof, sixty
    # metres outside, and on the far side at head height. Each is its own fresh data root and its
    # own fresh shelf, each has to say `opened the world shipped at` in its turn, and each has to
    # come back with SOLID VOXELS IN THE FRAME -- `--screenshot-frame 60`, because a frame counted
    # from a standing start is what a player's first second is.
    #
    # The number that matters is `nothing to sample`. If the world were partial, a camera out here
    # would find nodes it had to build and the run would say so; a run that reads and samples
    # nothing has everything this viewpoint needs already in the file.
    # ------------------------------------------------------------------------------------------
    $views = @()
    foreach ($cam in $GateCams) {
        $vroot = New-DataRoot
        $vshelf = Join-Path $vroot "WorldShaper\worlds\$Stem.wsworld"
        $vshot = Join-Path $vroot 'view.png'
        # 600 s and not 180, and the reason is measured rather than cautious: sixty frames of the
        # estate is seconds on a card and can be minutes on a software Vulkan, and a gate run on
        # this machine drew NO frame in 180 s while a bake was holding the GPU. `--max-seconds`
        # still ends the run either way -- it just stops the deadline being the thing that fails.
        $vargs = @('--world', $vshelf, '--no-title', '--screenshot', $vshot,
                   '--screenshot-frame', '60', '--no-update-check', '--no-vsync',
                   '--width', '960', '--height', '540', '--max-seconds', '600')
        if ($cam) { $vargs += @('--cam', $cam) }
        $vrun = Invoke-Game -What "view-$Stem" -Root $vroot -GameArgs $vargs

        if ($vrun.log -notmatch "opened the world shipped at") {
            Remove-Item $vroot -Recurse -Force -ErrorAction SilentlyContinue
            throw ("$Stem : the shipped world was read from the spawn view and NOT from '$cam'. " +
                   "One camera's answer is not the world's answer -- that is D685 exactly.")
        }
        $vscene = [regex]::Match($vrun.log,
            "scene: (\d+) chunks, (\d+) solid voxels, (\d+) of (\d+) nodes sharpened, content ([0-9a-f]+)")
        $vox = 0; $sharp = ''; $content = ''
        if ($vscene.Success) {
            $vox = [int64]$vscene.Groups[2].Value
            $sharp = "$($vscene.Groups[3].Value) of $($vscene.Groups[4].Value)"
            $content = $vscene.Groups[5].Value
        }
        if ($vox -le 0) {
            Remove-Item $vroot -Recurse -Force -ErrorAction SilentlyContinue
            throw "$Stem : from '$cam' the frame held no solid voxels at all"
        }
        if ($GateShots) {
            New-Item -ItemType Directory -Path $GateShots -Force | Out-Null
            $tag = if ($cam) { ($cam -replace '[^0-9A-Za-z-]', '_') } else { 'spawn' }
            Copy-Item $vshot (Join-Path $GateShots "$Stem-$tag.png") -Force -ErrorAction SilentlyContinue
        }
        Write-Host ("gate  $Stem from '{0}': {1:N0} solid voxels at frame 60, {2} nodes, content {3}" `
                    -f $(if ($cam) { $cam } else { 'the spawn view' }), $vox, $sharp, $content)
        $views += [pscustomobject]@{ cam = $cam; voxels = $vox; nodes = $sharp; content = $content }
        Remove-Item $vroot -Recurse -Force -ErrorAction SilentlyContinue
    }

    Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
    return [pscustomobject]@{
        stem     = $Stem
        from     = $from
        chunks   = [int64]$built.Groups[1].Value
        voxels   = [int64]$built.Groups[2].Value
        ready_ms = $readyMs
        views    = $views
    }
}

# ------------------------------------------------------------------------------------------------

$key = Get-BakeKey
$stamps = Read-Stamps
$rows = @()
$clock = [Diagnostics.Stopwatch]::StartNew()

foreach ($stem in $mine) {
    $world = Join-Path $clipdir "$stem.world"
    $warm = $false
    $spent = 0.0
    $nodes = ''
    $whole = $false
    $passesRun = 0

    if ($GateOnly) {
        if (-not (Test-Path $world)) { throw "$stem : -GateOnly, and there is no clips\$stem.world" }
    } else {
        # The stamp is `key|nodes|whole`, and only the key decides whether the file can be reused.
        # The other two fields are carried so that a WARM row can still say whether the world it
        # is reusing is the whole world -- a re-used partial world is still a partial world, and
        # -RequireWhole has to be able to see that without re-baking to find out.
        $known = ''
        if ($stamps.ContainsKey($stem)) { $known = $stamps[$stem] }
        $knownParts = $known -split '\|'
        if (-not $Force -and (Test-Path $world) -and $knownParts[0] -eq $key) {
            $warm = $true
            if ($knownParts.Count -ge 3) {
                $nodes = $knownParts[1]
                $whole = ($knownParts[2] -eq 'whole')
            }
            Write-Host ("bake  $stem is already baked from this exact game and clip " +
                        "({0:N2} MB); reusing it" -f ((Get-Item $world).Length / 1MB))
        }
    }

    if (-not $GateOnly -and -not $warm) {
        # Deleted first, always. A bake that fails must not leave the last one standing and be
        # mistaken for a success by the size check below. It is also half of what replaces
        # `--no-clip-cache` on a multi-pass bake: with no `.world` beside the clip, the first pass
        # has no shipped world to find and starts from nothing exactly as it did before.
        Remove-Item $world -Force -ErrorAction SilentlyContinue
        if ($stamps.ContainsKey($stem)) { $stamps.Remove($stem) }
        Write-Stamps -Stamps $stamps

        # ONE ROOT FOR THE WHOLE OF THIS WORLD'S BAKE, created empty here and thrown away below.
        # That is the other half: nothing this machine accumulated for itself is in reach of any
        # pass, which is what D684 asks `--no-clip-cache` for, and the passes can still see each
        # other -- which is what makes the second one resume rather than start again.
        $bakeRoot = New-DataRoot
        $shot = Join-Path ([IO.Path]::GetTempPath()) "ws-bake-$stem.png"
        $before = -1

        for ($pass = 1; $pass -le [math]::Max(1, $Passes); $pass++) {
            $passesRun = $pass
            if ($Passes -gt 1) {
                Write-Host "bake  $stem pass $pass of $Passes ..."
            } else {
                Write-Host "bake  $stem ..."
            }
            # `--world` and not `--clip-file`, and `--screenshot` so the run ENDS: see 1 and 2 at
            # the head of this file. The world is written at the fixed point and again on the way
            # out, so either route leaves the file behind.
            #
            # THE FIRST PASS SETTLES AND THE REST DO NOT, and that is the whole shape of a bake.
            #
            # `--settle --screenshot-frame 3` ends the run three frames after the ladder has
            # nothing left it can do, so a small clip -- a sky, a pane of glass -- finishes in
            # seconds and the loop stops after one pass. It is also what brings `kSettleGiveUp`
            # into play: with `--settle` set, frame 30,001 makes the run stop waiting and leave,
            # whatever `--max-seconds` says, which is why one run cannot finish the estate.
            #
            # So every pass after the first drops `--settle` and asks for a frame no run will
            # reach. Then `measuring` is true from frame one, the give-up branch is never entered,
            # and the only thing that ends the run is `--max-seconds` -- which makes a pass worth
            # its whole deadline of ladder work instead of the ~119 s that 30,000 frames comes to
            # on a fast card. Measured: a settle-less `sky_test` bake wrote all 60,136 of 60,136
            # nodes and left when its deadline came up.
            #
            # It logs `the picture is of frame N of the M it was asked for` on the way out. That
            # is expected and is not a fault: the screenshot is a by-product here, the thing being
            # produced is the `.world`, and the frame it was asked for is a number chosen so that
            # nothing but the clock can end the run.
            $bakeArgs = @(
                '--world', "clips\$stem.clip", '--no-title', '--bake-world',
                '--cam', $Cam, '--screenshot', $shot,
                '--max-seconds', "$Seconds", '--no-update-check', '--no-vsync',
                '--width', "$Width", '--height', "$Height")
            if ($pass -eq 1) {
                $bakeArgs += @('--settle', '--screenshot-frame', '3')
            } else {
                $bakeArgs += @('--screenshot-frame', '1000000000')
            }
            # THE WORD THAT WAS MISSING. Without it the ladder only sharpens what this one camera
            # can see, and the shipped file is complete where the baking camera stood and coarse
            # everywhere else -- which is the floor with no walls the user photographed.
            if ($RefineAll) { $bakeArgs += '--refine-all' }
            # Only on a single-pass bake, and see 7 at the head of this file for why it cannot be
            # there on a multi-pass one: it empties the very cache path the next pass resumes from.
            if ($Passes -le 1) { $bakeArgs += '--no-clip-cache' }

            $run = Invoke-Game -What "bake-$stem" -Root $bakeRoot -GameArgs $bakeArgs
            $spent += $run.seconds
            Write-Host $run.log
            Remove-Item $shot -Force -ErrorAction SilentlyContinue
            if ($run.code -ne 0) {
                Remove-Item $bakeRoot -Recurse -Force -ErrorAction SilentlyContinue
                throw "$stem : the bake run failed on pass $pass (exit $($run.code))"
            }

            # THE LOG LINE, not the exit code, because the failure this is guarding against exited
            # zero after four minutes of work and wrote nothing at all (D684).
            $wrote = [regex]::Match($run.log,
                "baked the world for shipping: '([^']+)' \((\d+) MB\), (\d+) of (\d+) nodes")
            if (-not $wrote.Success) {
                Remove-Item $bakeRoot -Recurse -Force -ErrorAction SilentlyContinue
                throw ("$stem : pass $pass finished and never baked anything. The game says " +
                       "'baked the world for shipping' when it does, and there is no such line " +
                       "above.")
            }
            if (-not (Test-Path $world)) {
                Remove-Item $bakeRoot -Recurse -Force -ErrorAction SilentlyContinue
                throw "$stem : said it baked, and there is no $world"
            }
            $done = [int64]$wrote.Groups[3].Value
            $all  = [int64]$wrote.Groups[4].Value
            $nodes = "$done of $all"
            $whole = ($done -ge $all)

            # EVERY NODE, which is the only outcome that means the file is the world. The game
            # prints the two numbers and they are equal when there is nothing left to sharpen.
            if ($whole) {
                Write-Host ("bake  $stem is WHOLE: $nodes nodes, after $pass pass(es)")
                break
            }
            # A pass that added nothing is a plateau, and running it thirty more times adds
            # nothing thirty more times. Say where it stopped rather than spending the budget.
            if ($done -le $before) {
                Write-Host ("bake  $stem stopped growing at $nodes nodes on pass $pass -- " +
                            "further passes are adding nothing, so this is where it ends.")
                break
            }
            $before = $done
            # The wall clock, checked between passes and not inside one: a pass already has its
            # own deadline, and cutting one off half way through would throw away the sharpening
            # it has done since the last write.
            if ($TotalMinutes -gt 0 -and $clock.Elapsed.TotalMinutes -ge $TotalMinutes) {
                Write-Host ("bake  $stem stopped at $nodes nodes: the run has used its " +
                            "$TotalMinutes minutes. That is a budget being reached, not a fault " +
                            "-- raise -TotalMinutes, or bake somewhere faster.")
                break
            }
            if ($run.log -match 'gave up waiting for the world to settle') {
                Write-Host ("bake  $stem pass $pass ran out its frame budget at $nodes nodes; " +
                            "the next pass resumes from here.")
            }
        }

        Remove-Item $bakeRoot -Recurse -Force -ErrorAction SilentlyContinue
        if (-not $whole) {
            Write-Host ("bake  $stem is NOT whole: $nodes nodes after $passesRun pass(es). The file " +
                        "is complete as far as it got and coarse past that -- raise -Passes or " +
                        "-Seconds to bake the rest.")
        }
    }

    $bytes = (Get-Item $world).Length
    $rows += [pscustomobject]@{
        stem    = $stem
        bytes   = $bytes
        mb      = [math]::Round($bytes / 1MB, 2)
        seconds = [math]::Round($spent, 1)
        nodes   = $nodes
        whole   = [bool]$whole
        passes  = $passesRun
        warm    = [bool]$warm
        sha256  = (Get-FileHash $world -Algorithm SHA256).Hash
        read    = $null
    }

    if (-not $GateOnly -and -not $warm) {
        $stamps[$stem] = "$key|$nodes|" + $(if ($whole) { 'whole' } else { 'partial' })
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

# AND THAT THE WORLD IS ALL OF IT, when somebody has asked for that -- before the gate, for the
# budget's reason exactly: a partial world reads perfectly well and proving that it does answers a
# question nobody asked. `-GateOnly` is exempt because it baked nothing and has nothing to say
# about how what it is proving was made; that judgement belongs to the run that made it.
if ($RequireWhole -and -not $GateOnly) {
    $short = @($rows | Where-Object { -not $_.whole })
    if ($short.Count -gt 0) {
        throw ("-RequireWhole, and " +
               (($short | ForEach-Object { "$($_.stem) got to $($_.nodes) nodes" }) -join '; ') +
               ". A world that stops where the ladder stopped is complete where the baking " +
               "camera reached and coarse past it, which is the floor with no walls this whole " +
               "feature exists to remove. Raise -Passes or -Seconds, or bake it somewhere with " +
               "a graphics card and package by hand.")
    }
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
    elseif ($row.whole) { $note = " (WHOLE, $($row.passes) pass(es))" }
    elseif ($row.passes -gt 0) { $note = " (PARTIAL, $($row.passes) pass(es))" }
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
# SAID OUT LOUD, because "it baked" and "it baked the world" are different claims and the second
# one is the whole point of --refine-all. A partial file still opens, still reads, still passes
# every gate above -- and is complete only as far as the ladder got, which is the thing the user
# photographed. Nobody should have to count nodes in a table to find that out.
$partial = @($rows | Where-Object { -not $_.whole -and -not $_.warm -and $_.passes -gt 0 })
if ($partial.Count -gt 0) {
    Write-Host ""
    Write-Host ("PARTIAL: " + (($partial | ForEach-Object { "$($_.stem) at $($_.nodes) nodes" }) -join ', '))
    Write-Host "These worlds are complete as far as the ladder reached and coarse past that."
    Write-Host "Raise -Passes or -Seconds. -RefineAll:`$false is the arm that is meant to be partial."
}

if ($Report) {
    $summary = [pscustomobject]@{
        game       = $Game
        key        = $key
        shard      = $Shard
        camera     = $Cam
        refine_all = [bool]$RefineAll
        seconds    = $Seconds
        total_mb   = $totalMB
        gated      = (-not $NoGate)
        worlds     = $rows
    }
    ($summary | ConvertTo-Json -Depth 6) | Out-File $Report -Encoding ascii
    Write-Host "wrote $Report"
}
