#Requires -Version 5.1
# Build pagemade (Release) and pack the Windows NSIS installer.
# Usage: .\build_win_pagemade.ps1
#        .\build_win_pagemade.ps1 -Version 0.1.1
# nmail uses build_win_pkg.ps1. The two apps share build\bin, so this
# script copies only the DLLs pagemade.exe imports, not every DLL there.
[CmdletBinding()]
param(
    [string]$Version = "0.1.0",
    [string]$BuildDir = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $BuildDir) { $BuildDir = Join-Path $Root "build" }
$BinDir = Join-Path $BuildDir "bin"
$Nsi = Join-Path $Root "installer\pagemade.nsi"
$FontSrc = Join-Path $Root "resources"
$HyphSrc = Join-Path $Root "pagemade\resources\hyphenation"

function Find-CMake {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vs) {
            $p = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if (Test-Path $p) { return $p }
        }
    }
    $fallback = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2019\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path $fallback) { return $fallback }
    throw "cmake.exe not found. Install CMake or Visual Studio with the C++ CMake tools."
}

function Find-Makensis {
    $cmd = Get-Command makensis -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = @(
        (Join-Path ${env:ProgramFiles(x86)} "NSIS\makensis.exe"),
        (Join-Path $env:ProgramFiles "NSIS\makensis.exe")
    )
    foreach ($p in $candidates) {
        if (Test-Path $p) { return $p }
    }
    throw "makensis.exe not found. Install NSIS from https://nsis.sourceforge.io/"
}

function Copy-IfExists([string]$Src, [string]$DstDir) {
    if (Test-Path $Src) {
        New-Item -ItemType Directory -Force -Path $DstDir | Out-Null
        Copy-Item $Src $DstDir -Force
        return $true
    }
    return $false
}

Write-Host "==> cmake"
$cmake = Find-CMake
Write-Host "    $cmake"

if (-not (Test-Path (Join-Path $BuildDir "CMakeCache.txt"))) {
    Write-Host "==> configure"
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    & $cmake -S $Root -B $BuildDir -G "Visual Studio 16 2019" -A x64 `
        -DNANOGUI_BUILD_SHARED=OFF `
        -DNANOGUI_BUILD_PYTHON=OFF `
        -DNANOGUI_USE_QUICKJS=OFF `
        -DNANOGUI_USE_FREETYPE=ON `
        -DNANOGUI_BUILD_EXAMPLES=ON
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }
}

Write-Host "==> build pagemade (Release)"
& $cmake --build $BuildDir --config Release --target pagemade_app -- /m:2
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

if (-not (Test-Path (Join-Path $BinDir "pagemade.exe"))) {
    throw "pagemade.exe was not produced in $BinDir"
}

Write-Host "==> VC runtime DLLs"
$sys = Join-Path $env:SystemRoot "System32"
foreach ($d in @("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll")) {
    if (-not (Copy-IfExists (Join-Path $sys $d) $BinDir)) {
        throw "Missing $d in $sys. pagemade.exe imports it."
    }
}

Write-Host "==> fonts -> bin\resources"
$fontDst = Join-Path $BinDir "resources"
New-Item -ItemType Directory -Force -Path $fontDst | Out-Null
$fonts = @(Get-ChildItem -Path $FontSrc -Filter "*.ttf" -File)
if ($fonts.Count -eq 0) { throw "No .ttf files in $FontSrc" }
foreach ($f in $fonts) {
    Copy-Item $f.FullName $fontDst -Force
    $kb = [math]::Round($f.Length / 1KB, 1)
    Write-Host ("    {0} ({1} KB)" -f $f.Name, $kb)
}
$emoji = Join-Path $fontDst "NotoColorEmoji.ttf"
if (-not (Test-Path $emoji)) {
    throw "NotoColorEmoji.ttf missing. NanoGUI loads it from resources\ at runtime."
}

Write-Host "==> hyphenation -> bin\resources\hyphenation"
$hyphDst = Join-Path $fontDst "hyphenation"
New-Item -ItemType Directory -Force -Path $hyphDst | Out-Null
foreach ($name in @("hyph-en-us.pat.txt", "hyph-en-us.hyp.txt", "hyph-en-us.LICENSE.txt")) {
    $src = Join-Path $HyphSrc $name
    if (-not (Test-Path $src)) { throw "Missing $src" }
    Copy-Item $src $hyphDst -Force
    Write-Host "    $name"
}

Write-Host "==> NSIS installer"
$makensis = Find-Makensis
if (-not (Test-Path $Nsi)) { throw "Missing $Nsi" }
& $makensis "/DPAGEMADE_VERSION=$Version" "/DPAGEMADE_BIN=$BinDir" $Nsi
if ($LASTEXITCODE -ne 0) { throw "makensis failed." }

$setup = Join-Path $Root "installer\Pagemade-Setup-$Version.exe"
if (-not (Test-Path $setup)) { throw "Installer not produced: $setup" }
$sz = (Get-Item $setup).Length
$mb = [math]::Round($sz / 1MB, 1)
Write-Host ("==> done: {0} ({1} MB)" -f $setup, $mb)
Write-Host "    binaries: $BinDir"
