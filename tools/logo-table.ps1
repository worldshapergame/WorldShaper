# The logo, from a picture to a text bitmap to a table the card can read.
#
# Same discipline as tools/font-table.ps1, and for the same reasons. There are three steps and each
# one exists:
#
#   -Extract   assets/icon.png  ->  assets/logo.txt
#              Run once, by hand, when the drawing itself changes. It reads the ALPHA, because the
#              logo is white on nothing and the shape is the transparency.
#
#   (default)  assets/logo.txt  ->  the packed block in shaders/logo.glsl
#              One bit a pixel, thirty-two pixels to a uint, row by row from the top.
#
# Why the text file in the middle. A picture wants to be looked at, and a diff of a `.png` shows
# nothing at all — so the SOURCE is a plain text bitmap that a person can read, edit and review,
# exactly as a glyph is. tests/test_logo.cpp reads the shader's table back and checks every pixel
# against that file, so the copy on the card cannot drift from the drawing.
#
# Why a bitmask rather than an image. The card would need a texture, a sampler, a staging copy and
# an upload to keep in step with it, and the whole thing is 800 uints. More to the point, what the
# logo DOES on the title needs the shape as data rather than as colour: it is dissolved into
# voxels, grown from its roots, extruded into a slab and rotated, and every one of those wants to
# ask "is there mark at this point" and nothing else.
#
# Why the host needs a third output. The sort that re-orders the mark's slices runs on the CPU
# (src/ui/logo.hpp, D467) and one of the keys it can sort by is how much mark a slice holds -- which
# is a property of a drawing the host cannot see. So -Marks writes src/ui/logo_marks.hpp: three
# hundred and twenty numbers, checked against this same file by tests/test_logo.cpp.
#
#   pwsh tools/logo-table.ps1 -Extract      redraw assets/logo.txt from the png
#   pwsh tools/logo-table.ps1               print the block for shaders/logo.glsl
#   pwsh tools/logo-table.ps1 -Marks src/ui/logo_marks.hpp     rewrite the host's row/column counts

param(
    [string]$Png = (Join-Path $PSScriptRoot "..\assets\icon.png"),
    [string]$Source = (Join-Path $PSScriptRoot "..\assets\logo.txt"),
    [switch]$Extract,
    [string]$Marks = ""
)

if ($Extract) {
    Add-Type -AssemblyName System.Drawing
    $bitmap = New-Object System.Drawing.Bitmap($Png)
    $width = $bitmap.Width
    $height = $bitmap.Height
    $out = New-Object System.Text.StringBuilder
    [void]$out.AppendLine("# The logo, as pixels.")
    [void]$out.AppendLine("#")
    [void]$out.AppendLine("# Drawn from assets/icon.png by tools/logo-table.ps1 -Extract, and this file is the SOURCE")
    [void]$out.AppendLine("# from here on: shaders/logo.glsl is generated from it and tests/test_logo.cpp checks the two")
    [void]$out.AppendLine("# against each other. Same discipline as the typeface, and for the same reason -- a picture")
    [void]$out.AppendLine("# wants to be looked at, and a diff of a logo change should show the shape that changed.")
    [void]$out.AppendLine("#")
    [void]$out.AppendLine("# One character per pixel: '#' is the mark, '.' is nothing. $width by $height.")
    [void]$out.AppendLine("")
    [void]$out.AppendLine("size $width $height")
    [void]$out.AppendLine("")
    for ($y = 0; $y -lt $height; $y++) {
        $line = New-Object System.Text.StringBuilder
        for ($x = 0; $x -lt $width; $x++) {
            [void]$line.Append($(if ($bitmap.GetPixel($x, $y).A -gt 127) { "#" } else { "." }))
        }
        [void]$out.AppendLine($line.ToString())
    }
    [System.IO.File]::WriteAllText($Source, $out.ToString())
    $bitmap.Dispose()
    Write-Host "wrote $Source ($width x $height)"
    return
}

$rows = @()
$width = 0
$height = 0
foreach ($line in (Get-Content -Path $Source)) {
    $text = $line.Trim()
    if ($text -eq "" -or $text.StartsWith("#")) { continue }
    if ($text.StartsWith("size ")) {
        $parts = $text -split '\s+'
        $width = [int]$parts[1]
        $height = [int]$parts[2]
        continue
    }
    $rows += $text
}

if ($rows.Count -ne $height) { throw "logo.txt says $height rows and has $($rows.Count)" }

# One bit a pixel, row by row from the top, thirty-two to a uint. Bit 0 of a word is its leftmost
# pixel, so the number reads left to right the way the drawing does.
$words = @()
foreach ($row in $rows) {
    if ($row.Length -ne $width) { throw "a row is $($row.Length) wide and the logo is $width" }
    for ($x = 0; $x -lt $width; $x += 32) {
        $word = 0
        for ($bit = 0; $bit -lt 32; $bit++) {
            if ($x + $bit -ge $width) { break }
            if ([string]$row[$x + $bit] -ne ".") { $word = $word -bor (1 -shl $bit) }
        }
        $words += $word
    }
}


if ($Marks -ne "") {
    # How much mark each row and each column holds, as a count of voxels. Written rather than
    # printed, because it is a whole header with a licence to be read on its own.
    $row_mark = @()
    $column_mark = @(0) * $width
    foreach ($row in $rows) {
        $count = 0
        for ($x = 0; $x -lt $width; $x++) {
            if ([string]$row[$x] -ne ".") {
                $count++
                $column_mark[$x] = $column_mark[$x] + 1
            }
        }
        $row_mark += $count
    }

    $out = New-Object System.Text.StringBuilder
    [void]$out.AppendLine("#pragma once")
    [void]$out.AppendLine("// How much of the mark each row and each column of the drawing holds.")
    [void]$out.AppendLine("//")
    [void]$out.AppendLine("// Generated by ``tools/logo-table.ps1 -Marks`` from ``assets/logo.txt``, which is the same source the")
    [void]$out.AppendLine("// card's bitmask is packed from, and checked against that file by ``tests/test_logo.cpp``.")
    [void]$out.AppendLine("//")
    [void]$out.AppendLine("// # Why the host needs this at all")
    [void]$out.AppendLine("//")
    [void]$out.AppendLine("// The drawing lives on the card as a bitmask (``shaders/logo.glsl``) and the host cannot read it. But")
    [void]$out.AppendLine("// the host is where the sorting happens (``src/ui/logo.hpp``, and see there for why), and one of the")
    [void]$out.AppendLine("// keys a seed can sort the mark's slices by is **how much mark a slice holds** -- which is a property")
    [void]$out.AppendLine("// of the drawing. Three hundred and twenty numbers is the whole of what the host needs to know about")
    [void]$out.AppendLine("// a picture it never sees.")
    [void]$out.AppendLine("//")
    [void]$out.AppendLine("// It is a count of voxels, not a fraction: a row of the drawing is $width voxels wide, so a row holding")
    [void]$out.AppendLine("// ninety-nine of them is the trunk and a row holding none is the air above the canopy.")
    [void]$out.AppendLine("")
    [void]$out.AppendLine("#include ""core/types.hpp""")
    [void]$out.AppendLine("")
    [void]$out.AppendLine("namespace ws::ui {")
    [void]$out.AppendLine("")
    [void]$out.AppendLine("// --- generated: tools/logo-table.ps1 -Marks ---")
    foreach ($pair in @(@("kLogoRowMark", $row_mark), @("kLogoColumnMark", $column_mark))) {
        $name = $pair[0]
        $values = $pair[1]
        [void]$out.AppendLine("inline constexpr u16 $name[$($values.Count)] = {")
        for ($i = 0; $i -lt $values.Count; $i += 16) {
            $slice = @()
            for ($j = $i; $j -lt [Math]::Min($i + 16, $values.Count); $j++) { $slice += "$($values[$j])," }
            [void]$out.AppendLine("    " + ($slice -join " "))
        }
        [void]$out.AppendLine("};")
        [void]$out.AppendLine("")
    }
    [void]$out.AppendLine("// --- end generated ---")
    [void]$out.AppendLine("")
    [void]$out.AppendLine("}  // namespace ws::ui")
    # No BOM: a header with one is a header MSVC reads and glslc would not, and the habit is worth
    # keeping in one place. See the handover's trap 3.
    [System.IO.File]::WriteAllText($Marks, $out.ToString(), (New-Object System.Text.UTF8Encoding $false))
    Write-Host "wrote $Marks"
    return
}

"const int kLogoWidth = $width;"
"const int kLogoHeight = $height;"
"const int kLogoWords = $($words.Count);"
"const uint kLogoBits[$($words.Count)] = uint[$($words.Count)]("
for ($i = 0; $i -lt $words.Count; $i += 6) {
    $slice = @()
    for ($j = $i; $j -lt [Math]::Min($i + 6, $words.Count); $j++) {
        $slice += ("0x{0:X8}u" -f $words[$j])
    }
    $comma = $(if ($i + 6 -lt $words.Count) { "," } else { "" })
    "    " + ($slice -join ", ") + $comma
}
");"
