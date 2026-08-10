# The moving case, which is the one the face pass is judged on.
#
# The settled grid discards transients by construction -- that is what `--settle` is for -- so it
# cannot see what a face pass costs while the store is churning. Trap 14 in the handover is this
# exact shape: the overlay named the culprit and the grid could not. This drives the camera along a
# fixed path at a fixed step and reports the pass table, so two builds can be compared on the state
# that actually costs.
#
# One run at a time. D367: a previous run still shutting down holds the GPU and the next one reads
# high.
# `-Chisel "8,16"` adds the other half of the worst case: a player carving and filling in front of
# the camera WHILE it flies, one edit every eight frames. The flight alone measures a camera
# revealing faces and `--edit` alone measures one change to a settled picture; a player does both
# at once, and the two costs land on the same frames -- an edit reopens every face within
# kEditShadowReach of it, and those are exactly the faces the moving camera is also claiming.
param(
    [string]$Cam = "0,2,-20,90,0",
    [string]$Fly = "0,0,3,15",
    [int]$Width = 2560,
    [int]$Height = 1440,
    [int]$Frame = 400,
    [int]$Rounds = 1,
    [string]$Tag = "run",
    [string]$Chisel = "",
    # Extra arguments, which is how the two arms of an A/B are run from one build:
    #   -Extra "--no-face-gate"        light everything in the store again, seen or not
    #   -Extra "--no-face-worklist"    dispatch over every live slot again
    [string]$Extra = ""
)

$exe = Join-Path $PSScriptRoot "..\build\bin\WorldShaper.exe"
$shot = Join-Path $env:TEMP ("ws_fly_{0}.png" -f $Tag)

for ($round = 1; $round -le $Rounds; ++$round) {
    # NOT `$extra`. PowerShell variable names are case-insensitive, so a local called `$extra`
    # IS the `[string]$Extra` parameter -- and a parameter declared [string] keeps that constraint
    # for life, so assigning an array to it silently joins it into one space-separated string and
    # the whole lot arrives at the exe as a single argument it ignores. That is trap 4 in
    # documentation/22-rewrite-handover.md, and it cost a full interleaved A/B here: eight runs in
    # which neither --chisel nor the arm flags were passed at all, and the two arms came out equal
    # because they were the same run.
    $runArgs = @()
    if ($Chisel -ne "") { $runArgs = @("--chisel", $Chisel) }
    if ($Extra -ne "") { $runArgs += @($Extra -split "\s+" | Where-Object { $_ -ne "" }) }
    $text = & $exe --screenshot $shot --screenshot-frame $Frame --settle `
        --width $Width --height $Height --cam $Cam --fly $Fly --quality 7 `
        --no-vsync --no-update-check --no-auto-quality @runArgs 2>&1
    $wanted = @("visibility", "faces", "resolve", "total GPU", "ambient on the card", "faces:",
                "chisel", "CPU ", "seen on the card")
    foreach ($line in $text) {
        $s = [string]$line
        foreach ($w in $wanted) {
            if ($s -match [regex]::Escape($w)) { Write-Output ("{0}|{1}" -f $Tag, $s.Trim()) }
        }
    }
}
