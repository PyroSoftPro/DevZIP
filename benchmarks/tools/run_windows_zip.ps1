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

$resolvedInput = (Resolve-Path -LiteralPath $InputPath).Path
$outputFull = [System.IO.Path]::GetFullPath($OutputPath)
$outputDirectory = Split-Path -Path $outputFull -Parent

if ($outputDirectory) {
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}

if (Test-Path -LiteralPath $outputFull) {
    Remove-Item -LiteralPath $outputFull -Force
}

$compressArchive = Get-Command Compress-Archive -ErrorAction SilentlyContinue
if (-not $compressArchive) {
    Write-BenchmarkSkip "Compress-Archive is not available in this PowerShell session"
}

Compress-Archive -LiteralPath $resolvedInput -DestinationPath $outputFull -CompressionLevel Optimal -Force
