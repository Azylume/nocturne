# Stages a Windows build into one folder and compiles installer/nocturne.iss with Inno Setup 6 into
# BUILD\installer\Nocturne-<version>-windows-x64-unsigned.exe. Run it from a VS x64 developer prompt (it copies the
# VC++ runtime from VCToolsRedistDir):
#
#   powershell -ExecutionPolicy Bypass -File tools\make-installer.ps1 [-BuildDir build] [-RequireEngine]
#
# The supervisor, the desk and the UI must be built. The engine and nocturne-ctl go in when they are there;
# -RequireEngine makes a missing nocturne.exe an error. FFmpeg's ffmpeg.exe and ffprobe.exe come from vcpkg. The version is the project() version in CMakeLists.txt. The
# installer is not code signed.
param(
    [string]$BuildDir = 'build',
    [switch]$RequireEngine
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = (Resolve-Path $BuildDir).Path
$out = Join-Path $build 'installer'
$stage = Join-Path $out 'stage'

function Find-Iscc
{
    if ($env:ISCC) { return $env:ISCC }
    $found = Get-Command iscc.exe -ErrorAction SilentlyContinue
    if ($found) { return $found.Source }
    # Inno Setup installs for all users or, from winget, for the current user alone.
    foreach ($folder in ${env:ProgramFiles(x86)}, "$env:LOCALAPPDATA\Programs") {
        $candidate = Join-Path $folder 'Inno Setup 6\ISCC.exe'
        if (Test-Path $candidate) { return $candidate }
    }
    throw 'no Inno Setup 6 (ISCC.exe); install it or set ISCC'
}

function Copy-Binary([string]$name, [bool]$required)
{
    $path = Join-Path $build $name
    if (Test-Path $path) {
        Copy-Item $path $stage
        "staged $name"
    } elseif ($required) {
        throw "no $path (build it first)"
    } else {
        "left out $name (not built)"
    }
}

# The FFmpeg version vcpkg built, as in its package's SPDX file ("9.0.2", without the port version).
function Get-FfmpegVersion
{
    $spdx = Join-Path $build 'vcpkg_installed\x64-windows\share\ffmpeg\vcpkg.spdx.json'
    (Get-Content $spdx -Raw | ConvertFrom-Json).packages[0].versionInfo -replace '#.*$'
}

# ffmpeg.exe and ffprobe.exe from the same vcpkg FFmpeg as the DLLs, with the FFmpeg DLLs they load that the build
# did not copy (avfilter). Fails unless the staged ffmpeg is that version and its configuration is LGPL-2.1 only.
function Copy-FfmpegTools
{
    $tools = Join-Path $build 'vcpkg_installed\x64-windows\tools\ffmpeg'
    foreach ($name in 'ffmpeg.exe', 'ffprobe.exe') {
        $path = Join-Path $tools $name
        if (-not (Test-Path $path)) { throw "no ${path}: vcpkg.json must list FFmpeg's ffmpeg and ffprobe features" }
        Copy-Item $path $stage
    }
    Get-ChildItem $tools -Filter *.dll | Where-Object { -not (Test-Path (Join-Path $stage $_.Name)) } |
        Copy-Item -Destination $stage
    $about = & (Join-Path $stage 'ffmpeg.exe') -hide_banner -version | Out-String
    if ($LASTEXITCODE -ne 0) { throw "the staged ffmpeg.exe does not run (exit code $LASTEXITCODE)" }
    $version = Get-FfmpegVersion
    if ($about -notmatch "^ffmpeg version n?$([regex]::Escape($version))\b") {
        throw "the staged ffmpeg.exe is not FFmpeg ${version}: $about"
    }
    if ($about -match '--enable-(gpl|version3|nonfree)\b') { throw "the staged ffmpeg.exe has $($Matches[0])" }
    "staged ffmpeg.exe and ffprobe.exe (FFmpeg $version, LGPL-2.1)"
}

# Each folder under licenses\ gets the licence texts of one component of THIRD-PARTY-NOTICES.txt, copied from the
# sources the build used: vcpkg's installed copyright files, FetchContent's checkouts and the vendored files.
function Copy-Licences
{
    $licences = Join-Path $stage 'licenses'
    $vcpkg = Join-Path $build 'vcpkg_installed\x64-windows\share'
    $deps = Join-Path $build '_deps'
    $juce = Join-Path $deps 'juce-src'
    $files = [ordered]@{
        'FFmpeg' = @("$vcpkg\ffmpeg\copyright")
        'opus' = @("$vcpkg\opus\copyright")
        'nv-codec-headers' = @("$vcpkg\ffnvcodec\copyright")
        'pocketfft' = @("$vcpkg\pocketfft\copyright")
        'glslang' = @("$deps\glslang-src\LICENSE.txt")
        'SPIRV-Cross' = @("$deps\spirv_cross-src\LICENSE") + (Get-ChildItem "$deps\spirv_cross-src\LICENSES\*.txt").FullName
        'JUCE' = @("$juce\LICENSE.md", "$juce\modules\juce_core\zip\zlib\README",
            "$juce\modules\juce_graphics\image_formats\pnglib\LICENSE",
            "$juce\modules\juce_graphics\image_formats\jpglib\README",
            "$juce\modules\juce_graphics\fonts\harfbuzz\COPYING",
            "$juce\modules\juce_graphics\unicode\sheenbidi\LICENSE")
        'nlohmann-json' = @("$root\third_party\nlohmann\LICENSE.MIT")
        'ISF-Files' = @("$root\third_party\isf\vidvox\LICENSE")
        'gl-transitions' = @("$root\third_party\isf\gl-transitions\LICENSE",
            "$root\third_party\isf\gl-transitions\LICENSE-ISF-Files")
    }
    foreach ($component in $files.Keys) {
        $folder = New-Item -ItemType Directory -Force (Join-Path $licences $component)
        foreach ($file in $files[$component]) {
            if (-not (Test-Path $file)) { throw "no licence file $file" }
            $name = Split-Path -Leaf $file
            # JUCE's dependencies' files share names; their folder tells them apart.
            if ($component -eq 'JUCE' -and $name -ne 'LICENSE.md') {
                $name = (Split-Path -Leaf (Split-Path -Parent $file)) + '-' + $name
            }
            Copy-Item $file (Join-Path $folder $name)
        }
    }
    # The vendored stb headers and the ported FSR shaders carry their licence inside the source file.
    Save-Notice "$root\third_party\stb\stb_image_write.h" 'This software is available under 2 licenses' '^\*/$' "$licences\stb\LICENSE.txt"
    Save-Notice "$root\src\engine\fsr_upscaler.cpp" '^// Copyright \(c\) 2021 Advanced Micro Devices' '^$' "$licences\AMD-FSR1\LICENSE.txt"
    Copy-Item (Join-Path $root 'installer\THIRD-PARTY-NOTICES.txt') $stage
    $ffmpeg = Get-FfmpegVersion
    if (-not (Select-String -Quiet -SimpleMatch "FFmpeg $ffmpeg " (Join-Path $stage 'THIRD-PARTY-NOTICES.txt'))) {
        throw "THIRD-PARTY-NOTICES.txt does not name the FFmpeg the build ships ($ffmpeg)"
    }
    "staged the licences of $($files.Count + 2) components"
}

# Writes the lines of a source file from the first that matches $from up to the next that matches $to (exclusive),
# without their // comment markers.
function Save-Notice([string]$source, [string]$from, [string]$to, [string]$destination)
{
    $lines = Get-Content $source
    $start = ($lines | Select-String -Pattern $from | Select-Object -First 1).LineNumber - 1
    if ($start -lt 0) { throw "no licence notice in $source" }
    $end = $start + 1
    while ($end -lt $lines.Count -and $lines[$end] -notmatch $to) { $end++ }
    New-Item -ItemType Directory -Force (Split-Path -Parent $destination) | Out-Null
    $lines[$start..($end - 1)] -replace '^// ?' | Set-Content $destination
}

$version = Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern '^project\(nocturne VERSION ([0-9.]+)' |
    Select-Object -First 1 | ForEach-Object { $_.Matches[0].Groups[1].Value }
if (-not $version) { throw 'no project() version in CMakeLists.txt' }
$commitFile = Join-Path $build 'source-commit.txt'
if (-not (Test-Path $commitFile)) { throw "no $commitFile (configure the build with CMake first)" }
$commit = (Get-Content $commitFile -Raw).Trim()
if (-not $env:VCToolsRedistDir) { throw 'VCToolsRedistDir is not set: run this from a VS x64 developer prompt' }
$iscc = Find-Iscc

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

foreach ($name in 'nocturne-supervisor.exe', 'nocturne-desk.exe', 'Nocturne UI.exe') { Copy-Binary $name $true }
Copy-Binary 'nocturne.exe' $RequireEngine.IsPresent
Copy-Binary 'nocturne-ctl.exe' $false

# vcpkg's toolchain copies the FFmpeg and opus DLLs each executable loads beside it.
$dlls = Get-ChildItem -Path $build -Filter *.dll
if (-not $dlls) { throw "no DLLs in ${build}: vcpkg did not copy FFmpeg beside the binaries" }
$dlls | Copy-Item -Destination $stage
$crt = Get-ChildItem -Path (Join-Path $env:VCToolsRedistDir 'x64') -Directory -Filter 'Microsoft.VC*.CRT' |
    Select-Object -First 1
if (-not $crt) { throw "no VC++ runtime under $env:VCToolsRedistDir\x64" }
Copy-Item (Join-Path $crt.FullName '*.dll') $stage
Copy-FfmpegTools
Copy-Item (Join-Path $root 'installer\autostart.ps1') $stage
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'LICENSE.txt')
Copy-Licences
"staged $($dlls.Count) DLL(s) from the build and the VC++ runtime from $($crt.Name)"

& $iscc /Q "/DAppVersion=$version" "/DSourceCommit=$commit" "/DStageDir=$stage" "/O$out" `
    (Join-Path $root 'installer\nocturne.iss')
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }
"wrote $out\Nocturne-$version-windows-x64-unsigned.exe ($commit)"
