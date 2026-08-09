# How speckled a render is, as a number. Shared by tools\speckle.ps1 and tools\baseline.ps1.
#
# It lived inside speckle.ps1 until the baseline harness needed the same number, and two copies
# of a metric is two metrics: the moment one is tuned the figures stop being comparable, which
# is the exact failure speckle.ps1 exists to end.
#
# WHAT IT MEASURES. For every pixel, how far its luminance sits from the median of its eight
# neighbours, relative to the local level. A smooth surface scores near nought whether it is
# bright or dark; a lone bright pixel on a smooth wall scores near one however dim the room.
# Taking the median rather than the mean is what makes it a speckle metric rather than a
# sharpness metric — a real edge moves the mean of its neighbours and leaves the median alone,
# so an image full of mouldings and joints does not read as noise.
#
# Reported as the mean over the frame times a thousand, plus the count of outright fireflies:
# pixels more than four times their neighbourhood, which are the ones the eye actually catches.

Add-Type -AssemblyName System.Drawing

# How far two renders are apart, for the stages whose whole claim is "and the picture did not
# change". R1 replaces every addressing scheme under the marcher and is supposed to draw the
# identical image; without a number for that, "looks the same to me" is the acceptance test.
#
# Three figures, because they fail differently. The MEAN catches a shift in overall brightness
# that no single pixel makes obvious. The WORST catches one pixel gone wrong — a hole, a firefly,
# a face that lost its light — which a mean of a million pixels buries completely. The COUNT of
# pixels past a threshold separates "everything moved slightly", which is usually a tone or
# exposure difference and often fine, from "a few hundred pixels are completely different",
# which never is.
function Measure-ImageDiff([string]$a, [string]$b, [int]$threshold = 8) {
    $bmpA = [System.Drawing.Bitmap]::FromFile($a)
    $bmpB = [System.Drawing.Bitmap]::FromFile($b)
    try {
        if ($bmpA.Width -ne $bmpB.Width -or $bmpA.Height -ne $bmpB.Height) {
            throw ("cannot compare {0}x{1} against {2}x{3}" -f
                   $bmpA.Width, $bmpA.Height, $bmpB.Width, $bmpB.Height)
        }
        $w = $bmpA.Width; $h = $bmpA.Height
        $rect = New-Object System.Drawing.Rectangle 0, 0, $w, $h
        $mode = [System.Drawing.Imaging.ImageLockMode]::ReadOnly
        $fmt = [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
        $dA = $bmpA.LockBits($rect, $mode, $fmt)
        $dB = $bmpB.LockBits($rect, $mode, $fmt)
        $bytesA = New-Object byte[] ($dA.Stride * $h)
        $bytesB = New-Object byte[] ($dB.Stride * $h)
        [System.Runtime.InteropServices.Marshal]::Copy($dA.Scan0, $bytesA, 0, $bytesA.Length)
        [System.Runtime.InteropServices.Marshal]::Copy($dB.Scan0, $bytesB, 0, $bytesB.Length)
        $bmpA.UnlockBits($dA)
        $bmpB.UnlockBits($dB)

        $sum = 0.0
        $worst = 0
        $over = 0
        $n = 0
        for ($y = 0; $y -lt $h; $y++) {
            $rowA = $y * $dA.Stride
            $rowB = $y * $dB.Stride
            for ($x = 0; $x -lt $w; $x++) {
                $iA = $rowA + $x * 4
                $iB = $rowB + $x * 4
                # The largest of the three channels, not their average: a pure red difference is
                # a difference, and averaging it with two unchanged channels divides it by three.
                $d = [Math]::Max([Math]::Abs([int]$bytesA[$iA] - [int]$bytesB[$iB]),
                     [Math]::Max([Math]::Abs([int]$bytesA[$iA + 1] - [int]$bytesB[$iB + 1]),
                                 [Math]::Abs([int]$bytesA[$iA + 2] - [int]$bytesB[$iB + 2])))
                $sum += $d
                $n++
                if ($d -gt $worst) { $worst = $d }
                if ($d -ge $threshold) { $over++ }
            }
        }
        return [PSCustomObject]@{
            Mean    = if ($n -gt 0) { $sum / $n } else { 0.0 }
            Worst   = $worst
            Over    = $over
            Pixels  = $n
            Identical = ($worst -eq 0)
        }
    } finally {
        $bmpA.Dispose()
        $bmpB.Dispose()
    }
}

# SAMPLED, not exhaustive, and the sample count is fixed rather than the stride.
#
# The walk is a sort of eight neighbours per pixel in PowerShell, which is fine over a million
# pixels and is not fine over eight million: measuring one 4K frame that way costs more than
# rendering the hundred and twenty frames that produced it. So a fixed number of centre pixels
# are examined, spread evenly over the frame.
#
# It stays the same quantity. The neighbourhood is still the eight pixels touching the centre —
# only which centres are asked changes — so this is the same mean estimated from a subset, and a
# quarter of a million samples pins a mean of a bounded quantity far tighter than the differences
# anyone is looking for. It also makes resolutions directly comparable, where an exhaustive walk
# gave 4K eight times the sample count and therefore a smoother number for no real reason.
function Measure-Speckle([string]$png, [int]$maxSamples = 250000) {
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

        # One stride for both axes, so the samples stay on a square lattice and no direction is
        # favoured. At or below the cap this is 1 and every pixel is examined, exactly as before.
        $interior = [double](($w - 2) * ($h - 2))
        $stride = 1
        if ($maxSamples -gt 0 -and $interior -gt $maxSamples) {
            $stride = [int][Math]::Ceiling([Math]::Sqrt($interior / $maxSamples))
        }

        $total = 0.0
        $counted = 0
        $fireflies = 0
        $neighbours = New-Object 'double[]' 8
        for ($y = 1; $y -lt $h - 1; $y += $stride) {
            for ($x = 1; $x -lt $w - 1; $x += $stride) {
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
                # Against the local level plus a floor, so a dark room does not read as all
                # noise for want of anything to divide by.
                $relative = [Math]::Abs($lum[$k] - $median) / ($median + 8.0)
                $total += $relative
                $counted++
                if ($lum[$k] -gt $median * 4.0 + 24.0) { $fireflies++ }
            }
        }
        # Fireflies scaled back up to the whole frame, because it is a count rather than a mean
        # and a raw count over a sampled subset would fall with resolution for no reason but the
        # sampling. The speckle figure needs no such correction: it is already an average.
        return [PSCustomObject]@{
            Speckle   = if ($counted -gt 0) { 1000.0 * $total / $counted } else { 0.0 }
            Fireflies = [int]($fireflies * $stride * $stride)
            Pixels    = $counted
            Stride    = $stride
        }
    } finally {
        $bmp.Dispose()
    }
}
