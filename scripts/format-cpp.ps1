param(
    [switch]$Check,
    [string]$ClangFormat
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent

if (-not $ClangFormat) {
    $formatterCommand = Get-Command clang-format -ErrorAction SilentlyContinue
    if ($formatterCommand) {
        $ClangFormat = $formatterCommand.Source
    } else {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $installation = & $vswhere -latest -products '*' -property installationPath
            if ($installation) {
                $ClangFormat = Join-Path $installation 'VC\Tools\Llvm\bin\clang-format.exe'
            }
        }
    }
}

if (-not $ClangFormat -or -not (Test-Path -LiteralPath $ClangFormat)) {
    throw 'clang-format not found; pass -ClangFormat with its executable path.'
}

$sourceDirectories = @('include', 'src', 'tests') | ForEach-Object {
    Join-Path $projectRoot $_
}
$sourceFiles = @(Get-ChildItem -LiteralPath $sourceDirectories -Recurse -File |
    Where-Object { $_.Extension -in '.hpp', '.cpp' } |
    Sort-Object FullName |
    Select-Object -ExpandProperty FullName)

if ($Check) {
    & $ClangFormat --style=file --dry-run --Werror @sourceFiles
} else {
    & $ClangFormat --style=file -i @sourceFiles
}
if ($LASTEXITCODE) {
    throw 'C++ formatting failed.'
}
Write-Output "Checked/formatted $($sourceFiles.Count) C++ files."
