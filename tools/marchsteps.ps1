# How far a ray actually walks, and therefore whether hardware ray tracing would buy anything.
#
# # The question
#
# RT cores accelerate one half of a ray: *which volume did I hit*. They do it by descending a BVH
# instead of stepping a grid, and against triangle soup that is an enormous win because the
# alternative is testing every triangle.
#
# This renderer's alternative is not that. `node_march` in shaders/node.glsl walks an octree, and
# when a cell is empty it does NOT step over it -- `skip_level` asks the descent how big the empty
# block is and jumps to its far side in one step, at whatever size that turns out to be. That is
# hierarchical empty-space skipping, which is the same service a BVH provides, out of the structure
# the world is already stored in.
#
# So the whole case for a BVH rests on a number: how many outer steps does a ray really spend? If it
# is thirty, there is a lot to win. If it is four, there is nothing there whatever the hardware can
# do, and the cost -- a second geometry representation, a rebuild on every chisel, a mesh per LOD
# level -- is paid for nothing.
#
# Nothing in this engine measured it until D849.
#
# # Two instruments, deliberately, because they read different sources
#
#   `--debug-mode 12` writes the visibility word out as four exact bytes; byte 2 is the primary
#   ray's step count. Per pixel, exact, and PRIMARY RAYS ONLY -- it is an image, and an image has
#   no pixel for a sun ray.
#
#   `--march-stats` counts every `node_march` in the frame with two atomics on the way out: the
#   primary rays, the face pass's sun rays, its gathering rays, and the refraction marches. It has
#   no distribution, only a mean.
#
# The first gives the shape, the second gives the coverage. They are not two views of one counter --
# one is an image readback of the visibility pass and the other is an atomic in the marcher -- so
# when they agree, they agree about the world rather than about a shared mistake. That distinction
# is `documentation/22-rewrite-handover.md` §4's own trap, written down after three checks all
# reported "agrees, perfectly" with 304 visible faults on screen.
#
#   tools\marchsteps.ps1
#   tools\marchsteps.ps1 -Clip clips\sampler.clip -Views close,outdoor

param(
    [string]$Clip = "clips\facility.clip",
    [string]$Views = "close,outdoor,enclosed,mid",
    [int]$Width = 1280,
    [int]$Height = 800,
    [int]$Frame = 240
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\bin\WorldShaper.exe"
if (-not (Test-Path $exe)) { throw "no WorldShaper.exe at $exe - run build.bat first" }
. (Join-Path $PSScriptRoot "_grid.ps1")

$shots = Join-Path $env:TEMP "ws-marchsteps"
New-Item -ItemType Directory -Force -Path $shots | Out-Null

# A stray from an earlier run goes on sampling on half the machine, and every figure taken beside it
# is wrong gradually rather than obviously. CLAUDE.md's first trap, and it costs nothing to check.
$stray = Get-Process WorldShaper -ErrorAction SilentlyContinue
if ($stray) { throw "WorldShaper.exe is already running (pid $($stray.Id -join ', ')) - close it first" }

# The histogram, compiled rather than interpreted: PowerShell walks a few million simple operations
# a second and a frame is a million pixels of several each. facecount.ps1's own argument.
Add-Type -TypeDefinition @'
public static class WsMarchSteps {
    // 32bppArgb arrives B,G,R,A in memory. The shader wrote the visibility word's four bytes to
    // r,g,b,a low byte first, so byte 2 -- the step count -- is the BLUE channel.
    // A miss is 0xFFFFFFFF and is skipped: it carries no surface and no walk that ended in one.
    public static long[] Histogram(byte[] bytes, int width, int height, int stride) {
        long[] hist = new long[256];
        for (int y = 0; y < height; y++) {
            int row = y * stride;
            for (int x = 0; x < width; x++) {
                int i = row + x * 4;
                if (bytes[i] == 255 && bytes[i + 1] == 255 && bytes[i + 2] == 255 &&
                    bytes[i + 3] == 255) {
                    continue;
                }
                hist[bytes[i]]++;
            }
        }
        return hist;
    }
}
'@

function Percentile([long[]]$hist, [long]$total, [double]$at) {
    $seen = [long]0
    for ($v = 0; $v -lt 256; $v++) {
        $seen += $hist[$v]
        if ($seen -ge $total * $at) { return $v }
    }
    return 255
}

$viewList = Select-Named $WsViews $Views "view"

"clip: $Clip   $Width x $Height   frame $Frame"
""
"                 PRIMARY RAYS (--debug-mode 12, hit pixels)        EVERY RAY (--march-stats)"
"view        pixels   mean  p50  p90  p99  max      rays          mean   worst frame"
"--------------------------------------------------------------------------------------------"

foreach ($view in $viewList) {
    $cam = $WsViews[$view]
    $shot = Join-Path $shots "$view.png"

    # Arm one: the image, primary rays only.
    #
    # `Continue` around the two runs, and it is not laziness. Windows PowerShell wraps every line a
    # native program writes to stderr in an ErrorRecord, and the game logs there -- so under `Stop`
    # the first `[WARN ]` of a perfectly good load ends the script. The exit code is checked
    # explicitly instead, which is the thing that was actually being asked about.
    $ErrorActionPreference = "Continue"
    & $exe --clip-file $Clip --cam $cam --debug-mode 12 --no-title --settle `
           --screenshot $shot --screenshot-frame $Frame --max-seconds 400 `
           --width $Width --height $Height 2>&1 | Out-Null
    Get-Process WorldShaper -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

    $bmp = [System.Drawing.Bitmap]::FromFile($shot)
    $data = $bmp.LockBits((New-Object System.Drawing.Rectangle 0, 0, $bmp.Width, $bmp.Height),
                          'ReadOnly', 'Format32bppArgb')
    $bytes = New-Object byte[] ($data.Stride * $bmp.Height)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
    $bmp.UnlockBits($data)
    $hist = [WsMarchSteps]::Histogram($bytes, $bmp.Width, $bmp.Height, $data.Stride)
    $bmp.Dispose()

    $hits = [long]0
    $sum = [long]0
    $max = 0
    for ($v = 0; $v -lt 256; $v++) {
        $hits += $hist[$v]
        $sum += [long]$v * $hist[$v]
        if ($hist[$v] -gt 0) { $max = $v }
    }
    $mean = if ($hits -gt 0) { [math]::Round($sum / $hits, 1) } else { 0 }
    $p50 = Percentile $hist $hits 0.50
    $p90 = Percentile $hist $hits 0.90
    $p99 = Percentile $hist $hits 0.99

    # Arm two: the counter, every ray of every kind.
    $log = & $exe --clip-file $Clip --cam $cam --march-stats --no-title --settle `
                  --screenshot (Join-Path $shots "$view-stats.png") --screenshot-frame $Frame `
                  --max-seconds 400 --width $Width --height $Height 2>&1 |
           Select-String -Pattern "the marcher's walk"
    Get-Process WorldShaper -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    $ErrorActionPreference = "Stop"

    $rays = "-"
    $avg = "-"
    $worst = "-"
    if ($log -match "walk: (\d+) rays over the run, ([\d.]+) outer steps each on average, worst frame ([\d.]+)") {
        $rays = $Matches[1]
        $avg = $Matches[2]
        $worst = $Matches[3]
    }

    "{0,-10} {1,7} {2,6} {3,4} {4,4} {5,4} {6,4}   {7,14} {8,6} {9,7}" -f `
        $view, $hits, $mean, $p50, $p90, $p99, $max, $rays, $avg, $worst
}

""
"A BVH descent over a world this size is fifteen to twenty node tests. Read the two mean columns"
"against that number: what a BVH would replace is the walk, and the walk is what these say it is."
