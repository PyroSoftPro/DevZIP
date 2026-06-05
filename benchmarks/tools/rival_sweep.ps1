<#
  rival_sweep.ps1 - benchmark DevZIP's "best of the best" rivals on the same
  corpora used by overhaul_sweep.ps1. Measures compressed size, compression and
  decompression wall-time, and byte-exact roundtrip.

  General-purpose codecs (zstd, brotli, kanzi, paq8px) operate on a TAR of the
  corpus so they see one solid stream like DevZIP's solid archive. Per-type
  recompressors run file-by-file: cjxl/djxl for JPEG (byte-exact reconstruction),
  zopflipng for PNG (pixel-lossless re-encode; output bytes differ from source).

  paq8px is the impractically slow max-ratio reference, so it is gated to the
  smaller corpora by default.
#>
[CmdletBinding()]
param(
    [string[]]$Corpora = @("text", "code", "exe", "jpeg", "png"),
    [string[]]$Tools = @("zstd", "brotli", "kanzi", "paq8px", "cjxl", "zopflipng"),
    [string[]]$PaqCorpora = @("code", "text"),
    [string]$PaqLevel = "-8",
    [int]$Threads = [Environment]::ProcessorCount,
    [string]$OutJson
)

# Continue (not Stop): the image tools (cjxl/djxl) print banners to stderr, which
# would otherwise be promoted to terminating errors. Correctness is guarded by the
# explicit roundtrip hash checks below, not by the error preference.
$ErrorActionPreference = "Continue"
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $repo "benchmarks\rivals\bin"
if (-not $OutJson) { $OutJson = Join-Path $repo "benchmarks\manifests\rival-sweep.results.json" }
$work = Join-Path $env:TEMP "rival-sweep"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force $work | Out-Null

function MB([long]$b) { [math]::Round($b / 1MB, 3) }
function FileHash([string]$p) { (Get-FileHash $p -Algorithm SHA256).Hash }

$results = @()

foreach ($corpus in $Corpora) {
    $in = Join-Path $repo "sample-data\bench\$corpus"
    if (-not (Test-Path $in)) { Write-Output "SKIP $corpus (missing)"; continue }
    $srcBytes = (Get-ChildItem -Recurse -File $in | Measure-Object Length -Sum).Sum

    # One solid TAR per corpus for the general-purpose codecs.
    $tar = Join-Path $work "$corpus.tar"
    & tar -cf $tar -C $in .
    $tarHash = FileHash $tar
    $tarBytes = (Get-Item $tar).Length
    Write-Output ("[{0}] source {1} MB  tar {2} MB" -f $corpus, (MB $srcBytes), (MB $tarBytes))

    function Add-Result($tool, $bytes, $cs, $ds, $rt, $note) {
        $script:results += [pscustomobject]@{
            corpus = $corpus; tool = $tool; source_bytes = $srcBytes; tar_bytes = $tarBytes
            comp_bytes = $bytes; compress_seconds = $cs; decompress_seconds = $ds
            roundtrip_ok = $rt; note = $note
        }
        $rtStr = if ($null -eq $rt) { "n/a" } else { $rt }
        Write-Output ("  {0,-10} {1,9} MB  c={2,6}s d={3,6}s  rt={4}  {5}" -f $tool, (MB $bytes), $cs, $ds, $rtStr, $note)
    }

    if ($Tools -contains "zstd") {
        $o = Join-Path $work "$corpus.tar.zst"; $r = Join-Path $work "z-$corpus.tar"
        $c = Measure-Command { & "$bin\zstd.exe" --ultra -22 -T0 -f -q $tar -o $o }
        $d = Measure-Command { & "$bin\zstd.exe" -d -f -q $o -o $r }
        Add-Result "zstd-22" (Get-Item $o).Length ([math]::Round($c.TotalSeconds, 1)) ([math]::Round($d.TotalSeconds, 1)) ((FileHash $r) -eq $tarHash) "tar"
    }
    if ($Tools -contains "brotli") {
        $o = Join-Path $work "$corpus.tar.br"; $r = Join-Path $work "b-$corpus.tar"
        $c = Measure-Command { & "$bin\brotli.exe" -q 11 -f -o $o $tar }
        $d = Measure-Command { & "$bin\brotli.exe" -d -f -o $r $o }
        Add-Result "brotli-11" (Get-Item $o).Length ([math]::Round($c.TotalSeconds, 1)) ([math]::Round($d.TotalSeconds, 1)) ((FileHash $r) -eq $tarHash) "tar"
    }
    if ($Tools -contains "kanzi") {
        $o = Join-Path $work "$corpus.knz"; $r = Join-Path $work "k-$corpus.tar"
        $c = Measure-Command { & "$bin\kanzi.exe" -c -i $tar -o $o -l 9 -j $Threads --force | Out-Null }
        $d = Measure-Command { & "$bin\kanzi.exe" -d -i $o -o $r -j $Threads --force | Out-Null }
        Add-Result "kanzi-l9" (Get-Item $o).Length ([math]::Round($c.TotalSeconds, 1)) ([math]::Round($d.TotalSeconds, 1)) ((FileHash $r) -eq $tarHash) "tar"
    }
    if ($Tools -contains "paq8px" -and $PaqCorpora -contains $corpus) {
        $o = Join-Path $work "$corpus.paq"; $r = Join-Path $work "p-$corpus.tar"
        Remove-Item $o -ErrorAction SilentlyContinue
        $c = Measure-Command { & "$bin\paq8px.exe" $PaqLevel $tar $o | Out-Null }
        $d = Measure-Command { & "$bin\paq8px.exe" -d $o $r | Out-Null }
        Add-Result ("paq8px" + $PaqLevel) (Get-Item $o).Length ([math]::Round($c.TotalSeconds, 1)) ([math]::Round($d.TotalSeconds, 1)) ((FileHash $r) -eq $tarHash) "tar; reference ceiling"
    }

    # Per-type recompressors.
    if ($corpus -eq "jpeg" -and $Tools -contains "cjxl") {
        $files = Get-ChildItem -Recurse -File $in
        $sum = 0L; $cs = 0.0; $ds = 0.0; $allOk = $true
        foreach ($f in $files) {
            $j = Join-Path $work ($f.BaseName + ".jxl"); $rj = Join-Path $work ($f.BaseName + ".recon.jpg")
            $cs += (Measure-Command { & "$bin\cjxl.exe" $f.FullName $j --lossless_jpeg=1 -q 100 2>&1 | Out-Null }).TotalSeconds
            $ds += (Measure-Command { & "$bin\djxl.exe" $j $rj 2>&1 | Out-Null }).TotalSeconds
            $sum += (Get-Item $j).Length
            if ((FileHash $rj) -ne (FileHash $f.FullName)) { $allOk = $false }
        }
        Add-Result "cjxl-jpeg" $sum ([math]::Round($cs, 1)) ([math]::Round($ds, 1)) $allOk "per-file lossless JPEG transcode"
    }
    if ($corpus -eq "png" -and $Tools -contains "zopflipng") {
        $files = Get-ChildItem -Recurse -File $in
        $sum = 0L; $cs = 0.0
        foreach ($f in $files) {
            $o = Join-Path $work ($f.BaseName + ".opt.png")
            $cs += (Measure-Command { & "$bin\zopflipng.exe" -y $f.FullName $o 2>&1 | Out-Null }).TotalSeconds
            $sum += (Get-Item $o).Length
        }
        Add-Result "zopflipng" $sum ([math]::Round($cs, 1)) 0 $null "per-file pixel-lossless re-encode (bytes differ from source)"
    }
    Write-Output "----"
}

$results | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 $OutJson
Write-Output "WROTE $OutJson"
