# Regenerates the glyph table that shaders/ui.glsl carries, from the font's own source.
#
# The interface draws its text in a compute shader, so the glyphs have to exist on the card as well
# as on the CPU. They are the SAME drawings: this prints the packed table, and a test
# (tests/test_font.cpp, "the shader's font is the font") reads the shader back and checks every
# glyph against assets/font/00-basic-latin.txt. Two copies of a drawing is one place for them to
# disagree, so the copy is generated and the disagreement is a failing test rather than a letter
# that is wrong only on screen.
#
#   pwsh tools/font-table.ps1                 print the GLSL block
#   pwsh tools/font-table.ps1 -Format cpp     print the C++ block for src/ui/glyphs.hpp
#
# There are two copies because there are two jobs. The card DRAWS the text; the shell MEASURES it —
# where a word ends decides where a button ends decides what a click lands on, and a layout worked
# out from advances the card does not share is a layout whose hit boxes are somewhere else. Both
# are generated from this one source and both are checked against it by the same test, so the third
# copy that would settle an argument between them never has to exist.
#
# The packing: a five by six cell, row 0 at the cap height and row 5 below the baseline, five bits
# a row, bit 4 of a row being its leftmost column — so the number reads the way the drawing was
# written down. Thirty bits, which is why a glyph is one uint. The second component is the advance,
# before tracking.

param(
    [string]$Font = (Join-Path $PSScriptRoot "..\assets\font\00-basic-latin.txt"),
    [ValidateSet("glsl", "cpp")]
    [string]$Format = "glsl"
)

$script:code = -1
$script:top = 5
$script:advance = 0
$script:draw = @()
$script:name = ""
$script:table = @{}

function Save-Glyph {
    if ($script:code -lt 0) { return }
    $width = 0
    foreach ($row in $script:draw) { if ($row.Length -gt $width) { $width = $row.Length } }
    $step = $script:advance
    if ($step -eq 0) { $step = $width }
    $script:table[$script:code] = [pscustomobject]@{
        Rows    = @($script:draw)
        Top     = $script:top
        Advance = $step
        Name    = $script:name
    }
    $script:code = -1
    $script:top = 5
    $script:advance = 0
    $script:draw = @()
    $script:name = ""
}

foreach ($line in (Get-Content -Path $Font)) {
    $text = $line.Trim()
    if ($text -eq "") { Save-Glyph; continue }
    if ($script:code -lt 0 -and $text.StartsWith("#")) { continue }

    $parts = $text -split '\s+'
    if ($parts[0] -eq "glyph") {
        Save-Glyph
        $script:code = [Convert]::ToInt32($parts[1].Substring(2), 16)
        if ($parts.Count -gt 2) { $script:name = ($parts[2..($parts.Count - 1)] -join ' ') }
        continue
    }
    if ($script:code -ge 0 -and $parts[0] -eq "top") { $script:top = [int]$parts[1]; continue }
    if ($script:code -ge 0 -and $parts[0] -eq "advance") { $script:advance = [int]$parts[1]; continue }
    if ($script:code -ge 0) { $script:draw += $text; continue }
}
Save-Glyph

for ($point = 0x20; $point -le 0x7E; $point++) {
    $bits = 0
    $step = 3
    $label = "(missing)"
    if ($script:table.ContainsKey($point)) {
        $glyph = $script:table[$point]
        $step = $glyph.Advance
        $label = $glyph.Name
        for ($i = 0; $i -lt $glyph.Rows.Count; $i++) {
            $row = 5 - ($glyph.Top - $i)
            if ($row -lt 0 -or $row -gt 5) {
                throw "U+{0:X4} row {1} falls outside the six-row cell" -f $point, $i
            }
            $drawn = $glyph.Rows[$i]
            if ($drawn.Length -gt 5) {
                throw "U+{0:X4} is {1} wide; the cell is five" -f $point, $drawn.Length
            }
            for ($x = 0; $x -lt $drawn.Length; $x++) {
                $pixel = [string]$drawn[$x]
                if ($pixel -ne "." -and $pixel -ne " ") {
                    $bits = $bits -bor (1 -shl ($row * 5 + (4 - $x)))
                }
            }
        }
    }
    if ($Format -eq "cpp") {
        "    {{0x{0:X8}u, {1}u}},   // U+{2:X4} {3}" -f $bits, $step, $point, $label
    } else {
        "    uvec2(0x{0:X8}u, {1}u),   // U+{2:X4} {3}" -f $bits, $step, $point, $label
    }
}
