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

if ($env:DEVZIP_RUN_REFERENCE_BENCHMARK -ne "1") {
    Write-BenchmarkSkip "reference codec benchmark is disabled unless DEVZIP_RUN_REFERENCE_BENCHMARK=1"
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
$referenceScript = Join-Path $projectRoot "native\engine\reference\devzip_reference.py"
if (-not (Test-Path -LiteralPath $referenceScript)) {
    Write-BenchmarkSkip "reference codec script was not found"
}

$python = Get-Command python.exe -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command python -ErrorAction SilentlyContinue
}
if (-not $python) {
    Write-BenchmarkSkip "python was not found for the reference codec"
}

& $python.Source $referenceScript "compress" $resolvedInput $outputFull
exit $LASTEXITCODE
