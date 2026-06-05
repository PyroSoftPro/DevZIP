[CmdletBinding()]
param(
    [string[]]$Corpora = @("text", "code", "exe", "jpeg", "png"),
    [string[]]$Levels = @("balanced", "max"),
    [string]$OutJson
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutJson) {
    $OutJson = Join-Path $repo "benchmarks\manifests\ov-sweep.results.json"
}
$cli = Join-Path $repo "native\engine\build\Release\devzip_cli.exe"
$sevenZip = "C:\Program Files\7-Zip\7z.exe"
$work = Join-Path $env:TEMP "ov-sweep"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force $work | Out-Null

function Get-DirHashSet([string]$dir) {
    Get-ChildItem -Recurse -File $dir | ForEach-Object { (Get-FileHash $_.FullName -Algorithm SHA256).Hash } | Sort-Object
}

function MB([long]$b) { [math]::Round($b / 1MB, 3) }

$results = @()
foreach ($corpus in $Corpora) {
    $in = Join-Path $repo "sample-data\bench\$corpus"
    if (-not (Test-Path $in)) { Write-Output "SKIP $corpus (missing)"; continue }
    $srcHashes = Get-DirHashSet $in
    $srcBytes = (Get-ChildItem -Recurse -File $in | Measure-Object Length -Sum).Sum

    # 7-Zip baselines
    $sevenSizes = @{}
    foreach ($m in @("lzma2", "ppmd")) {
        $o = Join-Path $work "$corpus-$m.7z"
        Remove-Item $o -ErrorAction SilentlyContinue
        $t = Measure-Command { & $sevenZip a -t7z "-m0=$m" -mx=9 $o "$in\*" | Out-Null }
        $sevenSizes[$m] = (Get-Item $o).Length
        Write-Output ("[{0}] 7z-{1,-6} {2,8} MB  {3,6}s" -f $corpus, $m, (MB $sevenSizes[$m]), [math]::Round($t.TotalSeconds, 1))
    }
    $best7z = [Math]::Min($sevenSizes["lzma2"], $sevenSizes["ppmd"])

    foreach ($lvl in $Levels) {
        $out = Join-Path $work "$corpus-$lvl.dvz"
        Remove-Item $out -ErrorAction SilentlyContinue
        $t = Measure-Command { & $cli compress $in $out --level $lvl | Out-Null }
        $sz = (Get-Item $out).Length
        $ex = Join-Path $work "x-$corpus-$lvl"
        if (Test-Path $ex) { Remove-Item -Recurse -Force $ex }
        & $cli extract $out $ex | Out-Null
        $roundtrip = (@(Compare-Object $srcHashes (Get-DirHashSet $ex)).Count -eq 0)
        $winVsBest = [math]::Round((($best7z - $sz) / $best7z) * 100, 2)
        $winVsLzma2 = [math]::Round((($sevenSizes["lzma2"] - $sz) / $sevenSizes["lzma2"]) * 100, 2)
        Write-Output ("[{0}] devzip-{1,-8} {2,8} MB  {3,6}s  vs7z-best={4,6}%  vsLZMA2={5,6}%  roundtrip={6}" -f `
                $corpus, $lvl, (MB $sz), [math]::Round($t.TotalSeconds, 1), $winVsBest, $winVsLzma2, $roundtrip)
        $results += [pscustomobject]@{
            corpus = $corpus; level = $lvl; source_bytes = $srcBytes
            devzip_bytes = $sz; sevenzip_lzma2_bytes = $sevenSizes["lzma2"]; sevenzip_ppmd_bytes = $sevenSizes["ppmd"]
            win_vs_best7z_pct = $winVsBest; win_vs_lzma2_pct = $winVsLzma2
            compress_seconds = [math]::Round($t.TotalSeconds, 1); roundtrip_ok = $roundtrip
        }
    }
    Write-Output "----"
}

$results | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 $OutJson
Write-Output "WROTE $OutJson"
