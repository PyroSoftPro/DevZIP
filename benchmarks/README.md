# Benchmark Harness

The benchmark harness measures aggregate archive size and wall-clock duration across a curated corpus manifest. It also computes a Weissman Score using the same `gzip`-baseline formula popularized by `lorenzosaino/weissman-score`, but that score is informational only. The actual product win condition remains aggregate end size versus the configured baseline tool. It is designed to compare these active lanes:

- `devzip-native`
- `7z-lzma2`
- `7z-lzma`
- `7z-ppmd`
- `7z-bzip2`
- `7z-deflate`
- `winrar`
- `windows-zip`

## Manifest Shape

Each manifest defines:

- corpus metadata,
- dataset paths and logical tags,
- optional command overrides,
- the target size win required for the shipping path,
- the baseline tool name and the shipping tool name for the regression gate.

See `benchmarks/manifests/mixed-large.json` for the baseline manifest.

## Output

The harness emits:

- console progress by dataset and tool,
- a JSON summary next to the manifest by default, including aggregate `archive_size_mb` values,
- an optional markdown summary via `--markdown-out`, where the aggregate table reports `End Size (MB)`,
- per-dataset and aggregate Weissman Scores for tools that complete successfully.

## Weissman Scoring

The harness follows the same Weissman equation used by the reference repo:

- `W = alpha * (r / r_b) * (log(T_b) / log(T))`
- `r` / `T` are the target tool's compression ratio and wall-clock time
- `r_b` / `T_b` are the same values for a `gzip` baseline

Because the benchmark corpus often contains directories rather than a single file, the harness builds a deterministic tar snapshot for directory datasets before running the `gzip` baseline. That keeps the baseline reproducible while still letting archive tools compress the original directory structure directly.

Configure Weissman scoring per manifest under `scoring.weissman`. The current manifest enables it with `alpha=1.0` and `gzip` as the baseline tool.

Important:

- Product success is based on end size, not Weissman score.
- Weissman is a fun comparison layer for ratio/speed context only.

Use `benchmarks/check_regression.py` to enforce the aggregate improvement gate once a results JSON exists. The gate now reads `goal.baseline_tool` and `goal.shipping_tool` from the manifest instead of assuming fixed tool names, and it compares archive size only. Weissman does not affect pass/fail.

## Command Resolution

Tools are resolved in this order:

1. `--tool name=command`
2. manifest `tools` command template
3. `PATH`

Missing optional tools are marked as `skipped` rather than failing the full run. The PowerShell wrappers in `benchmarks/tools/` emit benchmark-specific skip notes when a locally installed commercial tool is unavailable.

## Default Matrix Notes

- `devzip-native` is the shipping comparison lane and points at `native/engine/build/devzip_cli.exe`.
- `7z-lzma2` is the current baseline tool.
- `7z-lzma2`, `7z-lzma`, `7z-ppmd`, `7z-bzip2`, and `7z-deflate` all use `benchmarks/tools/run_7zip_max.ps1` with their named compression method plus ultra compression and solid mode.
- `winrar` uses `Rar.exe` with RAR5, best compression, solid mode, and a large dictionary through `benchmarks/tools/run_winrar_max.ps1`.
- `windows-zip` uses `Compress-Archive -CompressionLevel Optimal` as the native Windows ZIP baseline.

## Example

```powershell
python benchmarks/run_benchmarks.py `
  --manifest benchmarks/manifests/mixed-large.json `
  --markdown-out docs/benchmarks/baseline-results.md
```
