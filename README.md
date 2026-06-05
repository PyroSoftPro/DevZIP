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
- The pre-overhaul `mixed-large` aggregate and the older `best-of-two` / `selective-zpaq5` backend bake-offs are retained in git history; the shipping default is now level-driven `best-of-N`.

## Time Cost (the tradeoff)

DevZIP buys its smaller archives with CPU time: ZPAQ-5 context mixing and the
preflate PNG path are the dominant costs. Codecs run concurrently per solid group
and groups compress in parallel across cores, so wall-time tracks the slowest
codec rather than the sum. Compression time, seconds, this machine (5900X):

| Category | `7z-lzma2` | `devzip balanced` | `devzip max` |
| --- | ---: | ---: | ---: |
| Text | 2.7 | 94.5 | 83.0 |
| Code | 0.1 | 2.1 | 2.1 |
| Executables | 6.7 | 91.1 | 89.6 |
| JPEG | 1.8 | 33.4 | 89.1 |
| PNG | 1.2 | 54.1 | 309.7 |
| **Total** | **12.5** | **275.2** | **573.8** |

- DevZIP is roughly **20-45x** slower than 7z-lzma2 here in exchange for the
  `-18%` size win; the gap is largest on PNG (`max`, preflate) and smallest on code.
- Decompression is fast and symmetric across levels (no per-level penalty).
- Use `--level fast` for a near-7-Zip-speed run when size matters less.

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

## Versus the Best-of-the-Best

We benchmarked the overhauled engine against the strongest compressors available
(same corpora, all roundtrip-verified). Aggregate end size across the sweep, MB
(smaller is better):

| Tool | Aggregate (MB) | vs DevZIP `max` |
| --- | ---: | ---: |
| **DevZIP `max`** | **44.561** | — |
| kanzi `-l9` (context mixing) | 52.503 | +17.8% |
| 7z-lzma2 | 54.317 | +21.9% |
| zstd `--ultra -22` | 55.803 | +25.2% |
| brotli `-11` | 57.365 | +28.7% |

Per-corpus end size (MB). **Bold** = overall winner; _italic_ = a rival beats
DevZIP. General-purpose codecs compress a solid TAR per corpus; `cjxl`/`zopflipng`
run per file:

| Corpus | 7z-lzma2 | zstd `-22` | brotli `-11` | kanzi `-l9` | per-type | DevZIP `max` |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| text | 1.520 | 1.604 | 1.599 | 0.918 | — | **0.901** |
| code | 0.193 | 0.199 | 0.192 | _0.151_ | — | 0.162 |
| exe | 7.265 | 8.679 | 8.253 | _6.792_ | — | 6.930 |
| jpeg | 24.337 | 24.323 | 25.579 | 23.661 | cjxl 20.912 | **19.018** |
| png | 21.002 | 20.998 | 21.742 | 20.981 | zopflipng 19.629 | **17.550** |
| **Aggregate** | 54.317 | 55.803 | 57.365 | 52.503 | — | **44.561** |

DevZIP `max` produces the **smallest archive of any tool tested** — driven by
content-aware transforms: brunsli beats JPEG XL (`cjxl`) on JPEG (19.018 vs
20.912 MB) and preflate beats `zopflipng` on PNG (17.550 vs 19.629 MB) while
staying byte-exact. The paq8px `-8` ceiling (GPL, ~minutes/MB) on code is
**0.109 MB**. Honest gap: on raw code/exe streams, **kanzi -l9 (Apache-2.0)
edges DevZIP's current backend**. To close it we built **DZCM**, DevZIP's own
clean-room context-mixing codec (see below) — it already beats LZMA2 by 30% on
text — with a roadmap to match the strongest CM coders. Full analysis:
`docs/benchmarks/competitive-landscape.md`.

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

### DZCM — DevZIP's own context-mixing codec

`dzcm` is a clean-room compressor written for DevZIP (no third-party codec): a
32-bit binary arithmetic coder driven by a bank of finite-context models (orders
0–8), a word model, sparse models, and a match model, blended by a
context-selected logistic mixer and refined by two SSE/APM stages. It is fully
deterministic and round-trips byte-exact. Backend-only results on the same
pipeline (smaller is better):

| Corpus | `lzma2` | `dzcm` | DZCM vs LZMA2 | `zpaq5` |
| --- | ---: | ---: | ---: | ---: |
| code | 216,684 | 201,014 | **−7.2%** | 169,879 |
| text | 1,531,493 | 1,066,340 | **−30.4%** | 945,068 |

DZCM already beats LZMA2 (and libbsc) on text and code and is available as
`--backend dzcm` or as a `best-of-N` token (`best-of-n:lzma2,zpaq5,dzcm`), where
it can only help since the smallest block wins. It does not yet beat the
bit-history ICM in ZPAQ-5; the next step is adding nonstationary state-machine
counters and an ISSE chain. Design notes: `docs/dzcm-codec.md`.

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

- `best-of-n:<spec>` (e.g. `best-of-n:lzma2,zpaq5,ppmd,bsc,dzcm`) — competes the listed codecs, keeps the smallest
- `best-of-two` (legacy: LZMA2 vs ZPAQ-4)
- `bsc` (libbsc BWT)
- `dzcm` (DevZIP's own context-mixing codec)
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
