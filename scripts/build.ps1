param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"

if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "Visual Studio Installer (vswhere.exe) が見つからない。Visual Studio Build Tools の C++ ビルドツールをインストールすること。"
}

$visualStudio = & $vswhere `
    -latest `
    -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath

if (-not $visualStudio) {
    throw "MSVC x64/x86 ビルドツールが見つからない。Visual Studio Build Tools の「C++ によるデスクトップ開発」を追加すること。"
}

$developerShell = Join-Path $visualStudio "Common7\Tools\Launch-VsDevShell.ps1"
$cmake = Join-Path $visualStudio "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = Join-Path $visualStudio "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

if (-not (Test-Path -LiteralPath $developerShell)) {
    throw "Visual Studio Developer Shell が見つからない。Visual Studio Build Tools を修復すること。"
}

if (-not (Test-Path -LiteralPath $cmake) -or -not (Test-Path -LiteralPath $ninja)) {
    throw "Visual Studio の CMake/Ninja コンポーネントが見つからない。「Windows 用 C++ CMake ツール」を追加すること。"
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
    throw "CMake の構成に失敗した。"
}

& $cmake --build $buildDirectory

if ($LASTEXITCODE -ne 0) {
    throw "ビルドに失敗した。"
}

Write-Host "Built: $(Join-Path $buildDirectory 'wallpaper-mp4.exe')"
