# Collects the exact source of the FFmpeg a Windows build ships, for LGPL-2.1 section 6(d): the source tarball the
# vcpkg port downloaded (checked against the port's SHA512), the port itself with its patches (the overlay port in
# vcpkg-overlay-ports, a copy of the manifest's vcpkg baseline port plus our patch), and BUILD-INFO.txt with the features, the triplet and ffmpeg's own configuration. CI publishes the
# folder beside the installer.
#
#   powershell -ExecutionPolicy Bypass -File tools\ffmpeg-source.ps1 [-BuildDir build]
#
# Writes BUILD\installer\ffmpeg-source. Needs VCPKG_ROOT (a vcpkg clone holding the baseline commit) and the build's
# vcpkg_installed folder, ffmpeg.exe among it.
param(
    [string]$BuildDir = 'build'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = (Resolve-Path $BuildDir).Path
$out = Join-Path $build 'installer\ffmpeg-source'
$installed = Join-Path $build 'vcpkg_installed\x64-windows'
if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is not set' }

function Invoke-VcpkgGit([string[]]$arguments)
{
    $result = & git.exe -C $env:VCPKG_ROOT @arguments
    if ($LASTEXITCODE -ne 0) { throw "git $arguments failed with exit code $LASTEXITCODE" }
    $result
}

$manifest = Get-Content (Join-Path $root 'vcpkg.json') -Raw | ConvertFrom-Json
$baseline = $manifest.'builtin-baseline'
$overlay = Join-Path $root 'vcpkg-overlay-ports\ffmpeg'
$port = Get-Content (Join-Path $overlay 'vcpkg.json') -Raw | ConvertFrom-Json
$built = (Get-Content "$installed\share\ffmpeg\vcpkg.spdx.json" -Raw | ConvertFrom-Json).packages[0].versionInfo
if ($built -ne "$($port.version)#$($port.'port-version')") {
    throw "the build has FFmpeg $built, the port at vcpkg $baseline is $($port.version)#$($port.'port-version')"
}

if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null

$portfile = Get-Content (Join-Path $overlay 'portfile.cmake') -Raw
if ($portfile -notmatch '(?s)vcpkg_from_github\(.*?REPO\s+(\S+).*?REF\s+"?([^"\s]+)"?.*?SHA512\s+([0-9a-f]+)') {
    throw 'no vcpkg_from_github REPO, REF and SHA512 in the ffmpeg portfile'
}
$repo, $ref, $sha512 = $Matches[1], ($Matches[2] -replace '\$\{VERSION\}', $port.version), $Matches[3]
$tarball = Join-Path $out "ffmpeg-$ref.tar.gz"
Invoke-WebRequest "https://github.com/$repo/archive/$ref.tar.gz" -OutFile $tarball
if ((Get-FileHash $tarball -Algorithm SHA512).Hash -ne $sha512) { throw "$tarball does not match the port's SHA512" }
"downloaded $repo $ref (SHA512 matches the port)"

$portArchive = Join-Path $out "vcpkg-port-ffmpeg-$($port.version)-$($port.'port-version').tar.gz"
& tar.exe -czf $portArchive -C $root --transform 's,^,nocturne-port/,' vcpkg-overlay-ports/ffmpeg vcpkg-configuration.json
if ($LASTEXITCODE -ne 0) { throw "tar failed with exit code $LASTEXITCODE" }
Invoke-VcpkgGit @('archive', '--format=tar.gz', "--prefix=vcpkg-$baseline/", '-o', (Join-Path $out "vcpkg-triplets-$baseline.tar.gz"),
    $baseline, 'triplets/x64-windows.cmake') | Out-Null
"archived vcpkg-overlay-ports/ffmpeg, and triplets/x64-windows.cmake at vcpkg $baseline"

$features = ($manifest.dependencies | Where-Object name -eq 'ffmpeg').features -join ', '
$version = & "$installed\tools\ffmpeg\ffmpeg.exe" -hide_banner -version
if ($LASTEXITCODE -ne 0) { throw "ffmpeg.exe -version failed with exit code $LASTEXITCODE" }
@(
    "FFmpeg $($port.version), as Nocturne's Windows build ships it (the DLLs, ffmpeg.exe and ffprobe.exe)."
    ''
    "Source:   ffmpeg-$ref.tar.gz, https://github.com/$repo/archive/$ref.tar.gz"
    "SHA512:   $sha512"
    "Port:     $(Split-Path -Leaf $portArchive): vcpkg-overlay-ports/ffmpeg (portfile.cmake, vcpkg.json and every patch the"
    "          portfile applies; vcpkg's own ffmpeg port at $baseline plus one patch of ours) and vcpkg-configuration.json;"
    "          the triplet is in vcpkg-triplets-$baseline.tar.gz, from https://github.com/microsoft/vcpkg"
    "Package:  ffmpeg $built, triplet x64-windows, default features off"
    "Features: $features"
    ''
    'To rebuild: check out vcpkg at that commit and run'
    "  vcpkg install ffmpeg[core,$($features -replace ' ')]:x64-windows --overlay-ports=<the unpacked vcpkg-overlay-ports>"
    'vcpkg then downloads the same tarball, checks the same SHA512 and applies the same patches.'
    ''
    'ffmpeg -version of the shipped build:'
    $version
) | Set-Content (Join-Path $out 'BUILD-INFO.txt')
"wrote $out"
