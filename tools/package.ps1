# Builds the release zip the same way the workflow does, for when the workflow cannot.
#
# The first release was made with this, because GitHub Actions never picked the job up. It is
# the fallback, not the intended path: a build made here carries no provenance attestation,
# so anyone who wants to know where the file came from has only the checksum and the source.
# Say so in the release notes when you use it.

param([switch]$SkipTests)

$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")

$cmake = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
if ($cmake -notmatch 'project\(WorldShaper VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "could not find the version in CMakeLists.txt"
}
$version = $Matches[1]
$name = "WorldShaper-v$version-windows-x64"

& (Join-Path $root "build.bat")
if ($LASTEXITCODE -ne 0) { throw "build failed" }

# The same gate the workflow applies. A build that fails it should never become a download.
if (-not $SkipTests) {
    & (Join-Path $root "build\bin\ws_tests.exe")
    if ($LASTEXITCODE -ne 0) { throw "unit tests failed" }
    & (Join-Path $root "build\bin\WorldShaper.exe") --ticks 20000
    if ($LASTEXITCODE -ne 0) { throw "the world audit failed" }
    & (Join-Path $root "build\bin\WorldShaper.exe") --stream-frames 120
    if ($LASTEXITCODE -ne 0) { throw "the streaming audit failed" }
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
Copy-Item (Join-Path $root "LICENSE"), (Join-Path $root "README.md") $staging

$zip = Join-Path $package "$name.zip"
Compress-Archive -Path (Join-Path $staging "*") -DestinationPath $zip -Force
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
$hash | Out-File "$zip.sha256" -Encoding ascii

Write-Host ""
Write-Host "$name.zip  ($([math]::Round((Get-Item $zip).Length / 1MB, 2)) MB)"
Write-Host "SHA-256: $hash"
Write-Host ""
Write-Host "Publish with:  gh release create v$version `"$zip`" `"$zip.sha256`" --title `"WorldShaper v$version`""
Write-Host "Push to itch:  tools\push-itch.ps1"
