# Pushes the current build to itch.io.
#
# Run tools/package.ps1 first, or point -Package at a zip that already exists.
#
# # Why this does not handle your API key
#
# butler stores its own credential once you have run `butler login`, which opens a browser
# and hands the token straight to butler. Nothing else ever sees it — not this script, not a
# file in the repository, not a terminal history. That is the whole reason the login step is
# yours and this script only does the push.
#
# If you ever want this to run unattended (a CI job, say), butler also reads BUTLER_API_KEY
# from the environment. Put it in a secret store, never in a file in the repository.

param(
    [string]$Package,
    [string]$Target = "worldshapergame/worldshaper",
    [string]$Channel = "windows"
)

$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")

if (-not (Get-Command butler -ErrorAction SilentlyContinue)) {
    Write-Host "butler is not installed."
    Write-Host "  Get it from https://itch.io/docs/butler/installing.html, then run:"
    Write-Host "      butler login"
    Write-Host "  That opens a browser and stores the credential itself."
    exit 1
}

# The version comes from the same place everything else does, so the build shown on itch and
# the release on GitHub can never claim to be different things.
$cmake = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
if ($cmake -notmatch 'project\(WorldShaper VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "could not find the version in CMakeLists.txt"
}
$version = $Matches[1]

if (-not $Package) {
    $Package = Join-Path $root "package\WorldShaper-v$version-windows-x64.zip"
}
if (-not (Test-Path $Package)) {
    throw "no package at $Package - run tools/package.ps1 first"
}

Write-Host "pushing $Package to $Target`:$Channel as $version"
& butler push $Package "$Target`:$Channel" --userversion $version
if ($LASTEXITCODE -ne 0) { throw "butler push failed" }

Write-Host ""
Write-Host "Pushed. butler uploads the build only - the page itself (description,"
Write-Host "screenshots, tags, pricing) is edited on the itch.io website. There is no API"
Write-Host "for it. documentation/16-itch.md has the page copy ready to paste."
