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

$resolvedInput = Get-Item -LiteralPath (Resolve-Path -LiteralPath $InputPath).Path
$outputFull = [System.IO.Path]::GetFullPath($OutputPath)
$outputDirectory = Split-Path -Path $outputFull -Parent

if ($outputDirectory) {
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}

if (Test-Path -LiteralPath $outputFull) {
    Remove-Item -LiteralPath $outputFull -Force
}

$rarCandidates = @(
    "C:\Program Files\WinRAR\Rar.exe"
)

$rarCommand = Get-Command rar.exe -ErrorAction SilentlyContinue
if ($rarCommand) {
    $rarCandidates += $rarCommand.Source
}

$rarPath = $rarCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $rarPath) {
    Write-BenchmarkSkip "WinRAR was not found"
}

$inputParent = Split-Path -Path $resolvedInput.FullName -Parent
$exitCode = 0

Push-Location $inputParent
try {
    & $rarPath "a" "-ma5" "-m5" "-s" "-md1g" "-mt1" "-r" $outputFull $resolvedInput.Name
    $exitCode = $LASTEXITCODE
}
finally {
    Pop-Location
}

exit $exitCode
