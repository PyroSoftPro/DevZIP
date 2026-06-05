# DevZIP

DevZIP is a Windows-first archival project built around a native C++ engine and a readable `.dvz` container format.

Yes, `.dvz` is one letter away from `DBZ`, so a few power-level jokes were inevitable.

The project is focused on size-first compression for mixed real-world datasets. The current engine combines deterministic archive metadata, transform preprocessing, solid packing, deduplication, and pluggable native compression backends so the archive format can evolve without becoming unreadable.

## Status

- Shipping default: `balanced` level (`best-of-N`: LZMA2 vs ZPAQ-5)
- Archive format: readable `.dvz` v4
- Native CLI: `compress`, `extract`, `verify` (with `--level` and `--backend`)
- Desktop app: WPF shell in `apps/windows-ui/DevZip.App`
- Public repo note: large generated corpora and benchmark output are intentionally excluded to keep the repository lightweight

## Why DevZIP Exists

DevZIP is trying to answer a practical question: can a custom archival container beat familiar desktop archivers on mixed corpora while still staying commercially usable and internally maintainable?

The current answer is promising:

- After the compression overhaul, DevZIP beats `7z-lzma2` by `-18.0%` in aggregate across the per-type sweep, winning every category (text, code, executables, JPEG, PNG).
- The archive remains self-describing by stamping the backend identity into the manifest.
- Experimental backend lanes still produce readable `.dvz` archives and can be compared without changing the container format.
- The current shipping lane is the stable "final form." The experimental ones are still in the gravity chamber.

## Top Victories

Headline wins from the compression overhaul sweep, against the project's main
size baseline `7z-lzma2`. Every DevZIP figure round-trips byte-exact. `balanced`
is the default; `max` adds the preflate PNG path. Lower is better.

| Category | DevZIP `balanced` (MB) | DevZIP `max` (MB) | `7z-lzma2` (MB) | Best win |
| --- | ---: | ---: | ---: | ---: |
| Text and structured data | 0.901 | 0.901 | 1.520 | **-40.7%** |
| JPEG photographs | 19.018 | 19.018 | 24.337 | **-21.9%** |
| PNG lossless images | 20.960 | 17.550 | 21.002 | **-16.4%** |
| Code | 0.162 | 0.162 | 0.193 | **-16.1%** |
| Software binaries (PE) | 6.936 | 6.930 | 7.265 | **-4.6%** |
| **Aggregate** | **47.977** | **44.561** | **54.317** | **-18.0%** |

The overhaul flipped the two categories that used to lag: JPEG (`-1.5%` -> `-21.9%`
via the brunsli lossless transcode), PNG (`-0.95%` -> `-16.4%` via preflate), and
software binaries (`+0.32%` loss -> `-4.6%` win via architecture-aware BCJ + ZPAQ-5).
The aggregate moved from roughly break-even into a **double-digit, -18%** win.

## Aggregate Comparison

Sum across the overhaul per-type sweep (text, code, exe, JPEG, PNG; 97.41 MB raw):

| Tool | End Size (MB) | Delta vs 7z-lzma2 |
| --- | ---: | ---: |
| `devzip max` | 44.561 | **-18.0%** |
| `devzip balanced` (default) | 47.977 | **-11.7%** |
| `7z-lzma2` | 54.317 | +0.00% |

Notes:

- Lower end size is better; every DevZIP figure round-trips byte-exact.
- Against the best 7-Zip method per category (lzma2 or ppmd), `devzip max` is still `-17.5%`.
- DevZIP trades time for size (ZPAQ-5 + preflate). Full timings are in `docs/benchmarks/baseline-results.md`.
- The pre-overhaul `mixed-large` aggregate and the older `best-of-two` / `selective-zpaq5` backend bake-offs are retained in git history; the shipping default is now level-driven `best-of-N`.

## Per-Type Highlights

Against `7z-lzma2`, the overhauled engine reports (best level shown):

- Text and structured data: `-40.7%`
- JPEG: `-21.9%` (brunsli lossless transcode)
- PNG: `-16.4%` (preflate deflate-undo, `--level max`)
- Code: `-16.1%`
- Software binaries: `-4.6%` (architecture-aware BCJ + ZPAQ-5)
- Aggregate across the sweep: `-18.0%`

Full numbers, timings, and the level matrix live in
`docs/benchmarks/baseline-results.md`.

## Backends

The engine picks a backend per compression level (override with `--backend`):

| Backend | Codecs competed per solid group | Default at level |
| --- | --- | --- |
| `lzma2` | LZMA2 only | `fast` |
| `best-of-n:lzma2,zpaq5` | LZMA2 vs ZPAQ-5 | `balanced` |
| `best-of-n:lzma2,zpaq5,ppmd` | + PPMd | `max` |
| `best-of-n:lzma2,zpaq5,ppmd,bsc` | + libbsc BWT | `insane` |

Each `best-of-N` block is tagged with its winning codec, so a single archive can mix
codecs and still extract with every decoder present. Codecs run concurrently and solid
groups compress in parallel across cores.

## What Ships Today

### Native engine

Located in `native/engine`.

Key traits:

- Deterministic `.dvz` manifest and block layout
- Transform pipeline with preprocessing and chunking
- Solid packing by file type
- Full-file and block-level deduplication
- Backend stamping for forward-readable archives
- Native backend selection through the CLI

Current CLI backends:

- `best-of-n:<spec>` (e.g. `best-of-n:lzma2,zpaq5,ppmd,bsc`) — competes the listed codecs, keeps the smallest
- `best-of-two` (legacy: LZMA2 vs ZPAQ-4)
- `bsc` (libbsc BWT)
- `lzma2`
- `ppmd`
- `libzpaq-4`
- `libzpaq-5`

When `--backend` is omitted the level picks the backend (see CLI Usage below).

### Windows desktop app

Located in `apps/windows-ui/DevZip.App`.

This is a WPF shell that delegates compression and extraction work to `devzip_cli.exe`.

### Docs and benchmark harness

- Format docs: `docs/format/dvz-format.md`
- Benchmark notes: `docs/benchmarks/baseline-results.md`
- Licensing boundary notes: `docs/specs/licensing.md`
- Benchmark harness: `benchmarks/`

## Building

### Native engine and CLI

```powershell
cmake -S native/engine -B native/engine/build
cmake --build native/engine/build --config Release --target devzip_cli devzip_tests
```

Run the native tests:

```powershell
.\native\engine\build\devzip_tests.exe
```

### Windows UI

```powershell
dotnet build apps/windows-ui/DevZip.App/DevZip.App.csproj
```

## CLI Usage

Basic compression (defaults to the `balanced` level):

```powershell
.\native\engine\build\devzip_cli.exe compress "path\to\folder" "archive.dvz"
```

Pick a compression level (`fast` | `balanced` | `max` | `insane`):

```powershell
.\native\engine\build\devzip_cli.exe compress "path\to\folder" "archive.dvz" --level max
```

The level controls how hard the engine works:

- `fast` — branch/delta filters only, single LZMA2 pass. Quickest.
- `balanced` (default) — adds the brunsli JPEG transcoder, code dictionary, and PNG IDAT strip; routes through `best-of-N` (LZMA2 vs ZPAQ-5).
- `max` — adds the preflate deflate-undo (PNG/zip/gzip) and escalates to `best-of-n` (LZMA2 vs ZPAQ-5 vs PPMd).
- `insane` — `max` plus libbsc BWT in the best-of-N pool and full create-time roundtrip verification.

Select a specific backend explicitly (overrides the level's default backend):

```powershell
.\native\engine\build\devzip_cli.exe --backend "best-of-n:lzma2,zpaq5,ppmd,bsc" compress "path\to\folder" "archive.dvz"
```

Extract:

```powershell
.\native\engine\build\devzip_cli.exe extract "archive.dvz" "output-folder"
```

Verify:

```powershell
.\native\engine\build\devzip_cli.exe verify "archive.dvz"
```

## Benchmarking

Run the benchmark harness:

```powershell
python benchmarks/run_benchmarks.py --manifest benchmarks/manifests/mixed-large.json
```

Write the Markdown report too:

```powershell
python benchmarks/run_benchmarks.py --manifest benchmarks/manifests/mixed-large.json --markdown-out docs/benchmarks/baseline-results.md
```

Important:

- `sample-data/` is not included in this public repository because the generated corpora are very large.
- `BenchmarkResults/` is also excluded because it contains generated output, ad hoc experiments, and large archive artifacts.
- The benchmark scripts and manifests remain in the repo so the datasets and reports can be regenerated locally.

## Repository Layout

| Path | Purpose |
| --- | --- |
| `apps/windows-ui/DevZip.App` | WPF shell |
| `native/engine` | Native C++ engine, CLI, tests, vendored codec deps |
| `benchmarks` | Corpus manifests, wrappers, and benchmark harness |
| `docs/format` | `.dvz` format notes |
| `docs/specs` | Design and licensing notes |
| `python_tests` | Python-side test helpers |
| `README.html` / `README.css` | Visual benchmark dashboard |

## Licensing

This repository is source-available under the custom `DevZIP Non-Commercial License 1.0`.

- Non-commercial use, study, modification, and redistribution are allowed under the license terms.
- Commercial use requires a separate written commercial license.
- Commercial licensing contact: `contact@mikethetech.com`

Important:

- This is not an OSI-approved open source license.
- It is also not a GNU license, because GNU licenses permit commercial use.
- Vendored third-party components keep their own upstream license terms. See `docs/specs/licensing.md` for the project boundary notes.

## Commercial Licensing

For commercial licensing, business use, OEM bundling, internal enterprise deployment, paid services, or other revenue-linked usage, contact:

`contact@mikethetech.com`
