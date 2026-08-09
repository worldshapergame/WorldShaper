# The cameras and resolutions every measurement in this repository is taken at.
#
# One copy, dot-sourced, because a benchmark whose cameras drift between tools produces numbers
# that look comparable and are not. Adding a view here adds it to everything at once.
#
# The scene is the facility, which src/app/main.cpp makes the only scene the engine is judged
# against. Its matter runs -17..17 x -4.3..16.2 x -16.5..8.5 metres, so the origin is INSIDE it —
# which is why the engine's own default camera is the enclosed case, and why there was no outdoor
# number to compare it against until this file existed.
#
# Each view answers one question:
#
#   enclosed   the case that costs. Every bounce lands on a wall and none escapes to sky.
#   outdoor    the same building from far enough back to frame it: the ordinary case.
#   close      standing at the steps, where geometry is at full voxel detail and fills the frame.
#   mid / far  260 m and 900 m. Distance is supposed to be free; this is where that is tested.
#   distant    3 km, past what full residency holds, so it is drawn from summaries.
#   sky        straight up. The floor: what the renderer costs when asked for nothing.

$WsViews = [ordered]@{
    enclosed = "0,0,0,-90,0"
    outdoor  = "0,10,-60,90,-6"
    close    = "0,2,-20,90,0"
    mid      = "0,25,-260,90,-5"
    far      = "0,60,-900,90,-4"
    distant  = "0,200,-3000,90,-4"
    sky      = "0,10,-60,90,80"
}

$WsSizes = [ordered]@{
    deck = @(1280, 800)     # the Steam Deck target, and the resolution T0's budget is written at
    dev  = @(2560, 1440)    # the development target
    high = @(3840, 2160)    # where the rewrite is supposed to make the game playable
}

# Always an array, even for one name. A pipeline that yields a single item yields it as a scalar,
# and a scalar string has a .Count of 1 whatever is in it.
#
# Assign the result to a name that is NOT one of the caller's parameters. PowerShell variable
# names are case-insensitive, so `$views` and a parameter `$Views` are one variable — and a
# parameter declared `[string]` keeps that constraint for life, so assigning the array back to it
# silently rejoins it into "enclosed outdoor" and the caller runs one view instead of two.
function Select-Named($table, [string]$wanted, [string]$what) {
    if ($wanted -eq "all") { return @($table.Keys) }
    $names = @($wanted -split "[,\s]+" | Where-Object { $_ -ne "" })
    foreach ($n in $names) {
        if (-not $table.Contains($n)) {
            throw ("unknown {0} '{1}'. Known: {2}" -f $what, $n, ($table.Keys -join ", "))
        }
    }
    return @($names)
}
