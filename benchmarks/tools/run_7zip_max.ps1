[CmdletBinding()]
param(
    [Parameter()]
    [ValidateSet("lzma2", "lzma", "ppmd", "bzip2", "deflate")]
    [string]$Method = "lzma2",

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

$sevenZipCandidates = @(
    "C:\Program Files\7-Zip\7z.exe"
)

$sevenZipCommand = Get-Command 7z.exe -ErrorAction SilentlyContinue
if ($sevenZipCommand) {
    $sevenZipCandidates += $sevenZipCommand.Source
}

$sevenZipPath = $sevenZipCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $sevenZipPath) {
    Write-BenchmarkSkip "7-Zip was not found"
}

$inputParent = Split-Path -Path $resolvedInput.FullName -Parent
$exitCode = 0

$sevenZipArgs = switch ($Method) {
    "lzma2"   { @("a", "-t7z", "-m0=lzma2",   "-mx=9", "-md=256m", "-mfb=273", "-mmc=10000", "-ms=on", "-mmt=1", "-bd") }
    "lzma"    { @("a", "-t7z", "-m0=lzma",    "-mx=9", "-md=256m", "-mfb=273", "-mmc=10000", "-ms=on", "-mmt=1", "-bd") }
    "ppmd"    { @("a", "-t7z", "-m0=ppmd",    "-mx=9", "-mmem=256m", "-mo=32",                "-ms=on",           "-bd") }
    "bzip2"   { @("a", "-t7z", "-m0=bzip2",   "-mx=9", "-md=900k", "-mpass=7",               "-ms=on", "-mmt=1", "-bd") }
    "deflate" { @("a", "-t7z", "-m0=deflate", "-mx=9", "-mfb=258", "-mpass=15", "-mmc=999",  "-ms=on", "-mmt=1", "-bd") }
}

Push-Location $inputParent
try {
    & $sevenZipPath @sevenZipArgs "--" $outputFull $resolvedInput.Name
    $exitCode = $LASTEXITCODE
}
finally {
    Pop-Location
}

exit $exitCode
