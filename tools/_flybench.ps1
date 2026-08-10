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
param(
    [string]$Cam = "0,2,-20,90,0",
    [string]$Fly = "0,0,3,15",
    [int]$Width = 2560,
    [int]$Height = 1440,
    [int]$Frame = 400,
    [int]$Rounds = 1,
    [string]$Tag = "run"
)

$exe = Join-Path $PSScriptRoot "..\build\bin\WorldShaper.exe"
$shot = Join-Path $env:TEMP ("ws_fly_{0}.png" -f $Tag)

for ($round = 1; $round -le $Rounds; ++$round) {
    $text = & $exe --screenshot $shot --screenshot-frame $Frame --settle `
        --width $Width --height $Height --cam $Cam --fly $Fly --quality 7 `
        --no-vsync --no-update-check --no-auto-quality 2>&1
    $wanted = @("visibility", "faces", "resolve", "total GPU", "ambient on the card", "faces:")
    foreach ($line in $text) {
        $s = [string]$line
        foreach ($w in $wanted) {
            if ($s -match [regex]::Escape($w)) { Write-Output ("{0}|{1}" -f $Tag, $s.Trim()) }
        }
    }
}
