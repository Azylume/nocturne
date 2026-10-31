# Stages a Windows build into one folder and compiles installer/nocturne.iss with Inno Setup 6 into
# BUILD\installer\Nocturne-<version>-windows-x64-unsigned.exe. Run it from a VS x64 developer prompt (it copies the
# VC++ runtime from VCToolsRedistDir):
#
#   powershell -ExecutionPolicy Bypass -File tools\make-installer.ps1 [-BuildDir build] [-RequireEngine]
#
# The supervisor, the desk and the UI must be built. The engine and nocturne-ctl go in when they are there;
# -RequireEngine makes a missing nocturne.exe an error. The version is the project() version in CMakeLists.txt. The
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

$version = Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern '^project\(nocturne VERSION ([0-9.]+)' |
    Select-Object -First 1 | ForEach-Object { $_.Matches[0].Groups[1].Value }
if (-not $version) { throw 'no project() version in CMakeLists.txt' }
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
Copy-Item (Join-Path $root 'installer\autostart.ps1') $stage
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'LICENSE.txt')
"staged $($dlls.Count) DLL(s) from the build and the VC++ runtime from $($crt.Name)"

& $iscc /Q "/DAppVersion=$version" "/DStageDir=$stage" "/O$out" (Join-Path $root 'installer\nocturne.iss')
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }
"wrote $out\Nocturne-$version-windows-x64-unsigned.exe"
