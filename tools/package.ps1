# Builds the release zip the same way the workflow does, for when the workflow cannot.
#
# The first release was made with this, because GitHub Actions never picked the job up. It is
# the fallback, not the intended path: a build made here carries no provenance attestation,
# so anyone who wants to know where the file came from has only the checksum and the source.
# Say so in the release notes when you use it.

# -SkipBuild runs every step below against a tree that has ALREADY been built and tested, which is
# the only way this script currently works from an agent session or a shell that is not a developer
# command prompt: `build.bat` resolves the compiler through `vswhere`, and `vswhere` does not
# resolve through the invocation this script makes. So build and test by hand first, then
# -SkipBuild here, and the stage / zip / unpack-and-run / hash gates all still run. Do NOT use it
# to skip building; use it because the building already happened.
param([switch]$SkipTests, [switch]$SkipBuild)

$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")

$cmake = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
if ($cmake -notmatch 'project\(WorldShaper VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "could not find the version in CMakeLists.txt"
}
$version = $Matches[1]
$name = "WorldShaper-v$version-windows-x64"

if ($SkipBuild) {
    Write-Host "-SkipBuild: packaging whatever is already in build\bin. It must have been built"
    Write-Host "            and tested by hand, or this zip is a draft with a checksum on it."
} else {
    & (Join-Path $root "build.bat")
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

# The same gate the workflow applies. A build that fails it should never become a download.
if (-not $SkipTests) {
    & (Join-Path $root "build\bin\ws_tests.exe")
    if ($LASTEXITCODE -ne 0) { throw "unit tests failed" }
    & (Join-Path $root "build\bin\WorldShaper.exe") --ticks 20000
    if ($LASTEXITCODE -ne 0) { throw "the world audit failed" }
    # `--stream-frames`, the chunk-mirror audit, went with world/residency.* in R1e. What it
    # checked is `NodePool::stale_leaves` and `stale_masks` now, which ws_tests above covers
    # headlessly and which every screenshot prints.
}

# The executable has to agree with CMakeLists about what it is, for the same reason the
# workflow checks: the update check compares the two and is only meaningful if they match.
$reported = (Get-Item (Join-Path $root "build\bin\WorldShaper.exe")).VersionInfo.FileVersion
if ($reported -ne $version) {
    throw "the executable says $reported but CMakeLists.txt says $version"
}

$package = Join-Path $root "package"
$staging = Join-Path $package $name
Remove-Item $package -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $staging -Force | Out-Null

Copy-Item (Join-Path $root "build\bin\WorldShaper.exe") $staging
Copy-Item (Join-Path $root "build\bin\SDL3.dll") $staging
Copy-Item (Join-Path $root "build\bin\shaders") (Join-Path $staging "shaders") -Recurse

# THE CLIPS, which this script did not stage for three releases and which is the whole game.
#
# The build already puts them beside the executable -- `ws_clips` in CMakeLists.txt, so that a
# source build and a download have the same layout -- and every one of those is read from
# `<the executable's folder>/clips`: the worlds shelf seeds itself from there on first run
# (`Shell::seed_worlds`), an include that is not beside its own file is looked for there (D494),
# and the facility is twenty-two fragments assembled by a manifest that lives there.
#
# Staged from `build\bin\clips` and not from the source `clips\`, deliberately: that folder is
# where `cmake/copy_clips.cmake` has already dropped the built worlds out, and the facility's is
# well over half a gigabyte. Copying the source folder would put it in the zip.
#
# What a player got without this: the shelf empty, the facility nowhere, and the game saying
# "this world built to nothing" about a file that was never in the download. D640.
Copy-Item (Join-Path $root "build\bin\clips") (Join-Path $staging "clips") -Recurse

Copy-Item (Join-Path $root "LICENSE"), (Join-Path $root "README.md") $staging

$zip = Join-Path $package "$name.zip"
Compress-Archive -Path (Join-Path $staging "*") -DestinationPath $zip -Force

# Unpack the zip somewhere else entirely and start it there.
#
# Every check above this line runs the executable inside the build tree, where every path the
# build baked in happens to resolve. v0.6.0 shipped with the shader directory hard-coded to an
# absolute path on the build machine: it passed the unit tests, both audits and the version
# check, and then opened a black window and closed on every computer that was not this one.
#
# So the last gate is the only one that reproduces a player: a different directory, nothing of
# the source tree in reach, and the game has to draw a frame.
$smoke = Join-Path ([System.IO.Path]::GetTempPath()) "ws-smoke-$version"
Remove-Item $smoke -Recurse -Force -ErrorAction SilentlyContinue
Expand-Archive -Path $zip -DestinationPath $smoke -Force

$shot = Join-Path $smoke "smoke.png"
Push-Location $smoke
try {
    # `--no-clip-cache` so this is always the COLD path. The world cache lives in the player's own
    # folder and is keyed on the clip's absolute path, and the smoke test unpacks to the same temp
    # directory every time -- so without this, the second packaging run of a version would load the
    # FIRST one's world and report a success that had nothing to do with the zip being tested.
    $log = & (Join-Path $smoke "WorldShaper.exe") --screenshot $shot --screenshot-frame 3 `
        --no-update-check --no-vsync --no-clip-cache --width 640 --height 360 2>&1 | Out-String
    Write-Host $log
    if ($LASTEXITCODE -ne 0) { throw "the unpacked build failed to run (exit $LASTEXITCODE)" }
} finally {
    Pop-Location
}
if (-not (Test-Path $shot)) { throw "the unpacked build started but drew no frame" }

# AND THAT THERE WAS A WORLD IN THE FRAME, which is the half this gate did not have.
#
# "It drew a frame" is satisfied by an empty sky, and an empty sky is exactly what a download with
# no clips in it produces -- so the release that shipped without `clips/` passed every check in this
# file, including this one, and was broken for every player on the first screen. The comment above
# says the last gate is "the only one that reproduces a player"; a player who has no world is not
# reproduced by a screenshot being written.
#
# So the log is read. The failure is loud and specific -- the game says which file it could not open
# -- and the success carries a voxel count that cannot be faked by a sky.
if ($log -match "is not there to build|did not build|the world is empty|built to nothing") {
    throw "the unpacked build started and had no world in it: $($Matches[0]). " +
          "Check that clips\ is in the zip."
}
$built = [regex]::Match($log, "(?:built|loaded from cache) in [\d.]+ ms[^:]*: (\d+) chunks, (\d+) solid voxels")
if (-not $built.Success) {
    throw "the unpacked build never said it had built a world -- it should log 'built in N ms'"
}
if ([int64]$built.Groups[2].Value -le 0) {
    throw "the unpacked build built a world with no voxels in it"
}
Write-Host ("smoke test: the unpacked build ran from $smoke, built {0} solid voxels and drew a " +
            "frame" -f $built.Groups[2].Value)

# AND THAT THE SHIPPED WORLD IS READ OFF THE SHELF, WHICH IS A DIFFERENT QUESTION AGAIN.
#
# The gate above proves the zip has a game in it. It does not prove the zip has a WORLD in it that
# the game will use: a `.world` whose key does not match is refused in SILENCE and the sampler runs
# for minutes exactly as if the file were not there, producing a perfectly good screenshot and a
# perfectly good voxel count. That is D685 word for word, and it was reported as working within the
# hour. So the release workflow ends with `bake_world.ps1 -GateOnly` against its own unpacked zip
# and this script does the same, from the same file -- a gate the two paths do not share is a gate
# only one of them has.
#
# If it fails saying there is no clips\facility.world, the bake has not been run:
#     tools\bake_world.ps1        (writes build\bin\clips\facility.world, then re-package)
& (Join-Path $PSScriptRoot "bake_world.ps1") -Game $smoke -Clips facility -GateOnly

Remove-Item $smoke -Recurse -Force -ErrorAction SilentlyContinue

$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
$hash | Out-File "$zip.sha256" -Encoding ascii

Write-Host ""
Write-Host "$name.zip  ($([math]::Round((Get-Item $zip).Length / 1MB, 2)) MB)"
Write-Host "SHA-256: $hash"
Write-Host ""
# Said here as well as at the head of the file, because the head of the file is read once and this
# line is read every time somebody publishes one of these. A CI release carries a statement signed
# by GitHub tying the file to the commit and the workflow that built it; this zip carries nothing
# of the kind, and the release notes have to say so or they are claiming something that is not true.
Write-Host "NO PROVENANCE ATTESTATION. This zip was built here, not by .github/workflows/release.yml,"
Write-Host "so there is nothing tying it to a commit but the checksum above and the source. SAY SO IN"
Write-Host "THE RELEASE NOTES. A CI release carries an attestation; this one does not."
Write-Host ""
Write-Host "Publish with:  gh release create v$version `"$zip`" `"$zip.sha256`" --title `"WorldShaper v$version`""
Write-Host "Push to itch:  tools\push-itch.ps1"
