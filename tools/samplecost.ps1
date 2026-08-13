# What one NODE costs to sample, level by level, and whether a node sampled alone is the same
# node.
#
# # Why this measurement exists
#
# documentation/21-renderer-rewrite.md R11 moves the world's SOURCE onto the rule the renderer has
# run on since R1: what gets built is the node a ray asked for, at the resolution that node's own
# level implies -- 256 / 2^level, which is 32 voxels a metre at the level-3 leaf and 1 at an
# eight-metre node. Three of R11's sub-steps are trades against one number, what one node costs,
# and nothing in this project had ever measured it: every sampler figure it holds is
# whole-building or whole-region.
#
# It matters because the cost of a sample is NOT the cost of its voxels. A sample allocates a clip,
# wakes workers and descends from the root of a field that describes an entire building however
# small the box is -- so a node's price is mostly fixed, and the fixed part is what decides whether
# a node is a sensible unit at all. An empty node pays it and nothing else, which is what makes the
# two separable by measurement rather than by argument.
#
# # And the half that can fail
#
# Beside every timing the tool compares that node against THE SAME NODE read out of the biggest box
# that will fit, voxel for voxel and mask for mask. That is not a formality: the sampler settles
# paint rules over regions, and a rule keyed on a pattern has almost no box to settle against when
# the box is eight voxels wide. The first run of this found the sampler disagreeing with itself at
# one and two voxels a metre -- 17 of 32 nodes, 87 cells of matter present in one arm and absent in
# the other -- which is D613 and is fixed. The check stays because the next resolution ladder will
# find the next one.
#
#   tools\samplecost.ps1                      the facility, every level, recorded as the baseline
#   tools\samplecost.ps1 -Levels 3,5          the fine end only, quicker
#   tools\samplecost.ps1 -Clip clips\sampler.clip -Out ""    somewhere else, recording nothing

param(
    [string]$Clip = "clips\facility.clip",
    [string]$Levels = "3,8",
    [int]$Nodes = 24,
    [int]$Boxes = 3,
    # Where the row-per-level goes. Empty writes nothing, which is what a one-off run wants.
    [string]$Out = "documentation\baselines\r11a-sample-cost.csv"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\bin\WorldShaper.exe"
if (-not (Test-Path $exe)) { throw "no WorldShaper.exe at $exe - run build.bat first" }

$csv = ""
if ($Out -ne "") {
    $csv = Join-Path $root $Out
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $csv) | Out-Null
}

# The previous run, read BEFORE this one overwrites it. A baseline nobody compares against is a
# file, not a baseline.
$before = $null
if ($csv -ne "" -and (Test-Path $csv)) { $before = Import-Csv $csv }

$args = @("--clip-file", (Join-Path $root $Clip), "--sample-cost",
          "--sample-cost-levels", $Levels, "--sample-cost-nodes", $Nodes,
          "--sample-cost-boxes", $Boxes)
if ($csv -ne "") { $args += @("--sample-cost-csv", $csv) }

Write-Host "sampling $Clip node by node, levels $Levels ..." -ForegroundColor Cyan
$began = Get-Date
# PowerShell wraps every stderr line from a native program in an ErrorRecord, and the clip's own
# warnings go to stderr -- so with ErrorActionPreference at Stop the first warning ends the run.
# Same dance as tools\baseline.ps1, and for the same reason.
$was = $ErrorActionPreference
$ErrorActionPreference = "Continue"
$log = (& $exe @args 2>&1 | Out-String)
$failed = ($LASTEXITCODE -ne 0)
$ErrorActionPreference = $was
Write-Host $log
$took = ((Get-Date) - $began).TotalSeconds
Write-Host ("`nthe run took {0:N1} s" -f $took)

# The comparison, and only of the timings: the agreement is a pass or a fail and the executable
# has already said which, in words, with the first disagreeing cell named.
if ($before -ne $null -and $csv -ne "" -and (Test-Path $csv)) {
    $after = Import-Csv $csv
    Write-Host "`nagainst the last recorded run" -ForegroundColor Cyan
    Write-Host ("{0,-6} {1,>12} {2,>12} {3,>9}" -f "level", "was ms", "now ms", "change")
    foreach ($row in $after) {
        $was = $before | Where-Object { $_.level -eq $row.level }
        if ($was -eq $null) { continue }
        $old = [double]$was.one_ms
        $new = [double]$row.one_ms
        $move = if ($old -gt 0) { 100.0 * ($new - $old) / $old } else { 0.0 }
        # Five per cent, because a single sample of a few dozen nodes is not tighter than that --
        # the worst node at every level is several times the median, so which nodes the spread
        # picked matters. Anything larger is worth looking at rather than worth believing.
        $colour = if ([math]::Abs($move) -gt 5.0) { "Yellow" } else { "Gray" }
        Write-Host ("{0,-6} {1,12:N3} {2,12:N3} {3,8:N1}%" -f $row.level, $old, $new, $move) `
            -ForegroundColor $colour
    }
}

if ($failed) {
    Write-Host "`nA NODE SAMPLED ALONE IS NOT THE SAME NODE. Read the agreement block above." `
        -ForegroundColor Red
    exit 1
}
