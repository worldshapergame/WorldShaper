# Where in the frame a debug view's answer is missing, as a number, in vertical bands.
#
# Every instrument in this project so far reports a debug view as a SHARE OF THE FRAME -- "magenta
# plus blue as a share of surface" is how R9d and the shadow-latency table were measured. That is
# the right quantity for a camera that cut, because a cut turns the whole screen over at once. It is
# the wrong quantity for a camera that is PANNING, where the claim being tested is that the missing
# answers are all down one edge: a strip eight pixels wide reads as a rounding error in a whole-frame
# share and is the entire fault.
#
# So: the same classification, split into vertical bands. Band 0 is the left edge of the image and
# band N-1 the right, and which of those is the LEADING edge depends on which way the camera is
# turning -- a positive yaw here sweeps toward the LEFT band, because forward is
# (cos yaw, sin pitch, sin yaw) and increasing yaw swings the view anticlockwise seen from above.
# Print both ends and say which is which rather than trusting a memory of that.
#
# The classification is by exact colour, because the debug views write exact colours: they are stored
# with no tone mapping and no gamma (see the bottom of shaders/resolve.comp), and a unorm8 channel
# returns exactly what byte/255 put into it, which is what makes debug view 11's "the image IS the
# buffer" claim true. A tolerance of two is allowed anyway, so that a view which is later given a
# slightly different constant does not silently start counting nothing -- trap 10 says an instrument
# that has stopped recognising its own input looks exactly like an instrument with nothing to report.
#
#   magenta 255,0,255   no face at all: the composite could not find one for this pixel
#   blue      0,77,255  a face with too few samples to be believed
#   cyan      0,191,217 view 21 only: a coarse face, which has no material to have
#   green     0,89,0    no geometry -- sky, in views 16, 20 and 21
#   black     0,0,0     WHICH MEANS DIFFERENT THINGS IN DIFFERENT VIEWS, so it is counted apart
#                       and never folded into anything: in view 19 it is sky, because that view
#                       floors a surface at 0.02 and paints no geometry black; in view 16 it is a
#                       fully SHADOWED face, which is most of an interior and is the correct answer.
#
# Black gets its own column rather than a switch saying which view this is, and that is the whole
# lesson of the first version of this file: it folded black into the grey mean, so a band that was
# four fifths sky reported a convergence of 1.96 out of 255 and read as a catastrophic hole in the
# leading edge of a pan. Trap 10, in an instrument written to check for trap 10. The grey mean below
# is over the non-black surface only, and the black count sits beside it so the reader can see what
# it is made of.
#
# Views this is meaningful for: 16 (the sun), 19 (ambient convergence), 20 (lamps), 21 (materials).
#
#   .\tools\bands.ps1 -Png shot.png -Bands 8
# `param` has to be the FIRST statement in a script and comments do not count as one, so the
# Add-Type below it rather than above. Putting it above binds nothing, and the first positional
# argument then arrives as a stray path -- which is what it did, once.
param(
    [Parameter(Mandatory = $true)][string]$Png,
    [int]$Bands = 8
)

Add-Type -AssemblyName System.Drawing

function Near([int]$a, [int]$b) { return [Math]::Abs($a - $b) -le 2 }

$bmp = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Png))
try {
    $w = $bmp.Width; $h = $bmp.Height
    $data = $bmp.LockBits(
        (New-Object System.Drawing.Rectangle 0, 0, $w, $h),
        [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $bytes = New-Object byte[] ($data.Stride * $h)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
    $bmp.UnlockBits($data)

    $noFace = New-Object 'int[]' $Bands
    $unsure = New-Object 'int[]' $Bands
    $coarse = New-Object 'int[]' $Bands
    $sky = New-Object 'int[]' $Bands
    $surface = New-Object 'int[]' $Bands
    $black = New-Object 'int[]' $Bands
    # The mean GREY of the answered pixels in each band, which is the reading that needs no second
    # run at all. In view 19 grey is how far through its ambient rays a face is, and in view 16 it is
    # the sun visibility -- so "is the edge the camera is turning towards less converged than the one
    # it is turning away from" is a question one image answers. Matching a panned-into view against a
    # stood-there one is the obvious alternative and it does not work: a fifth of a degree of yaw is
    # three pixels of shift on this facade, and the first attempt at it reported a mean difference of
    # 8.09 of 255 that was entirely misalignment -- 54% of pixels were landing on a different FACE,
    # which the face-key view said in one run.
    $greySum = New-Object 'double[]' $Bands

    for ($y = 0; $y -lt $h; $y++) {
        $row = $y * $data.Stride
        for ($x = 0; $x -lt $w; $x++) {
            $i = $row + $x * 4
            # BGRA in memory, which is what Format32bppArgb means on a little-endian machine.
            $b = [int]$bytes[$i]; $g = [int]$bytes[$i + 1]; $r = [int]$bytes[$i + 2]
            $band = [int]([Math]::Floor($x * $Bands / $w))
            if ((Near $r 255) -and (Near $g 0) -and (Near $b 255)) { $noFace[$band]++ }
            elseif ((Near $r 0) -and (Near $g 77) -and (Near $b 255)) { $unsure[$band]++ }
            elseif ((Near $r 0) -and (Near $g 191) -and (Near $b 217)) { $coarse[$band]++ }
            elseif ((Near $r 0) -and (Near $g 89) -and (Near $b 0)) { $sky[$band]++ }
            elseif ($r -eq 0 -and $g -eq 0 -and $b -eq 0) { $black[$band]++ }
            else { $surface[$band]++; $greySum[$band] += ($r + $g + $b) / 3.0 }
        }
    }

    Write-Output ("{0}  {1}x{2}, {3} bands left to right" -f (Split-Path $Png -Leaf), $w, $h, $Bands)
    Write-Output "band   surface     no face      unsure      coarse         sky       black   mean grey"
    $totalMissing = 0
    $totalSurface = 0
    for ($k = 0; $k -lt $Bands; $k++) {
        $lit = $surface[$k] + $noFace[$k] + $unsure[$k] + $coarse[$k]
        $totalMissing += $noFace[$k] + $unsure[$k]
        $totalSurface += $lit
        # As a share of the SURFACE in that band and not of the band, because a band that is mostly
        # sky has no surface to be missing an answer for, and dividing by the band would report a
        # camera pointed at the horizon as healthy for the wrong reason.
        $share = if ($lit -gt 0) { 100.0 * ($noFace[$k] + $unsure[$k]) / $lit } else { 0.0 }
        $grey = if ($surface[$k] -gt 0) { $greySum[$k] / $surface[$k] } else { 0.0 }
        Write-Output ("{0,4} {1,9} {2,11} {3,11} {4,11} {5,11} {6,11} {7,11:N2}   {8,6:N2}% missing" -f
                      $k, $surface[$k], $noFace[$k], $unsure[$k], $coarse[$k], $sky[$k], $black[$k],
                      $grey, $share)
    }
    $whole = if ($totalSurface -gt 0) { 100.0 * $totalMissing / $totalSurface } else { 0.0 }
    Write-Output ("whole frame: {0} of {1} surface pixels missing an answer ({2:N2}%)" -f
                  $totalMissing, $totalSurface, $whole)
} finally {
    $bmp.Dispose()
}
