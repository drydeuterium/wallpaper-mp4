param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"

if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "Visual Studio Installer (vswhere.exe) was not found. Install the C++ build tools."
}

$visualStudio = & $vswhere `
    -latest `
    -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath

if (-not $visualStudio) {
    throw "MSVC x64/x86 build tools were not found. Add the Desktop development with C++ workload."
}

$developerShell = Join-Path $visualStudio "Common7\Tools\Launch-VsDevShell.ps1"
$cmake = Join-Path $visualStudio "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = Join-Path $visualStudio "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

if (-not (Test-Path -LiteralPath $developerShell)) {
    throw "Visual Studio Developer Shell was not found. Repair Visual Studio Build Tools."
}

if (-not (Test-Path -LiteralPath $cmake) -or -not (Test-Path -LiteralPath $ninja)) {
    throw "Visual Studio CMake/Ninja components were not found. Add C++ CMake tools for Windows."
}

& $developerShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

$buildDirectory = Join-Path $repositoryRoot "build\$Configuration"

& $cmake `
    -S $repositoryRoot `
    -B $buildDirectory `
    -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_BUILD_TYPE=$Configuration"

if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed."
}

& $cmake --build $buildDirectory

if ($LASTEXITCODE -ne 0) {
    throw "Build failed."
}

Write-Host "Built: $(Join-Path $buildDirectory 'wallpaper-mp4-laptop.exe')"
