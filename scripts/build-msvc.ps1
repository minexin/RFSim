param([ValidateSet('Debug','Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'MSVC installation not found' }
$cmakeBin = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$buildPath = Join-Path $projectRoot 'build-msvc'
& "$cmakeBin\cmake.exe" -S $projectRoot -B $buildPath -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE) { throw 'CMake configuration failed' }
& "$cmakeBin\cmake.exe" --build $buildPath --config $Configuration
if ($LASTEXITCODE) { throw 'Build failed' }
& "$cmakeBin\ctest.exe" --test-dir $buildPath -C $Configuration --output-on-failure --output-junit "$Configuration-results.xml"
if ($LASTEXITCODE) { throw 'Tests failed' }
