[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InputPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = "Stop"

function Write-BenchmarkSkip {
    param([string]$Message)

    Write-Output "BENCHMARK_SKIP: $Message"
    exit 85
}

function Add-ToolchainRuntimePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CliPath,

        [Parameter(Mandatory = $true)]
        [string]$ProjectRoot
    )

    $cliDirectory = Split-Path -Parent $CliPath
    $cacheCandidates = @(
        (Join-Path $cliDirectory "CMakeCache.txt"),
        (Join-Path (Split-Path -Parent $cliDirectory) "CMakeCache.txt"),
        (Join-Path $ProjectRoot "native\engine\build\CMakeCache.txt"),
        (Join-Path $ProjectRoot "native\engine\out\CMakeCache.txt")
    ) | Select-Object -Unique

    foreach ($cachePath in $cacheCandidates) {
        if (-not (Test-Path -LiteralPath $cachePath)) {
            continue
        }

        $compilerLine = Select-String -Path $cachePath -Pattern '^CMAKE_CXX_COMPILER:FILEPATH=' | Select-Object -First 1
        if (-not $compilerLine) {
            continue
        }

        $compilerPath = $compilerLine.Line.Split('=', 2)[1].Trim()
        if ([string]::IsNullOrWhiteSpace($compilerPath)) {
            continue
        }

        $toolchainBin = Split-Path -Path $compilerPath -Parent
        if (-not $toolchainBin -or -not (Test-Path -LiteralPath $toolchainBin)) {
            continue
        }

        $pathEntries = @($env:PATH -split ';') | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
        $hasToolchainPath = $pathEntries | Where-Object {
            [string]::Equals($_, $toolchainBin, [System.StringComparison]::OrdinalIgnoreCase)
        } | Select-Object -First 1

        if (-not $hasToolchainPath) {
            $env:PATH = "$toolchainBin;$env:PATH"
        }

        return
    }
}

$resolvedInput = (Resolve-Path -LiteralPath $InputPath).Path
$outputFull = [System.IO.Path]::GetFullPath($OutputPath)
$outputDirectory = Split-Path -Path $outputFull -Parent

if ($outputDirectory) {
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}

if (Test-Path -LiteralPath $outputFull) {
    Remove-Item -LiteralPath $outputFull -Force
}

$projectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$cliCandidates = @(
    (Join-Path $projectRoot "native\engine\build\devzip_cli.exe"),
    (Join-Path $projectRoot "native\engine\build\Debug\devzip_cli.exe"),
    (Join-Path $projectRoot "native\engine\build\RelWithDebInfo\devzip_cli.exe"),
    (Join-Path $projectRoot "native\engine\build\MinSizeRel\devzip_cli.exe"),
    (Join-Path $projectRoot "native\engine\build\Release\devzip_cli.exe"),
    (Join-Path $projectRoot "native\engine\out\devzip_cli.exe")
)

$cliPath = $cliCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $cliPath) {
    Write-BenchmarkSkip "native devzip_cli.exe was not found"
}

Add-ToolchainRuntimePath -CliPath $cliPath -ProjectRoot $projectRoot
& $cliPath "compress" $resolvedInput $outputFull
exit $LASTEXITCODE
