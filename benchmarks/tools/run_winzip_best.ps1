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

$winZipCandidates = @(
    "C:\Program Files\WinZip\wzzip.exe",
    "C:\Program Files (x86)\WinZip\wzzip.exe"
)

$winZipCommand = Get-Command wzzip.exe -ErrorAction SilentlyContinue
if ($winZipCommand) {
    $winZipCandidates += $winZipCommand.Source
}

$winZipPath = $winZipCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $winZipPath) {
    Write-BenchmarkSkip "WinZip command-line add-on was not found"
}

$inputParent = Split-Path -Path $resolvedInput.FullName -Parent
$exitCode = 0

Push-Location $inputParent
try {
    & $winZipPath "-a" "-r" "-m" $outputFull $resolvedInput.Name
    $exitCode = $LASTEXITCODE
}
finally {
    Pop-Location
}

exit $exitCode
