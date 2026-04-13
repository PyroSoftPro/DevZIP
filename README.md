# DevZIP

DevZIP is a Windows-first archival project built around a native C++ engine and a readable `.dvz` container format.

Yes, `.dvz` is one letter away from `DBZ`, so a few power-level jokes were inevitable.

The project is focused on size-first compression for mixed real-world datasets. The current engine combines deterministic archive metadata, transform preprocessing, solid packing, deduplication, and pluggable native compression backends so the archive format can evolve without becoming unreadable.

## Status

- Shipping native lane: `best-of-two`
- Archive format: readable `.dvz`
- Native CLI: `compress`, `extract`, `verify`
- Desktop app: WPF shell in `apps/windows-ui/DevZip.App`
- Public repo note: large generated corpora and benchmark output are intentionally excluded to keep the repository lightweight

## Why DevZIP Exists

DevZIP is trying to answer a practical question: can a custom archival container beat familiar desktop archivers on mixed corpora while still staying commercially usable and internally maintainable?

The current answer is promising:

- The shipping native lane beats `7z-lzma2` on the latest `mixed-large` aggregate rerun.
- The archive remains self-describing by stamping the backend identity into the manifest.
- Experimental backend lanes still produce readable `.dvz` archives and can be compared without changing the container format.
- The current shipping lane is the stable "final form." The experimental ones are still in the gravity chamber.

## Benchmark Snapshot

Latest shipping-lane aggregate result from `mixed-large`:

| Tool | Status | End Size (MB) | Total Time (s) | Delta vs 7z-lzma2 |
| --- | --- | ---: | ---: | ---: |
| `devzip-native` | ok | 143.815 | 304.2 | -0.71% |
| `7z-lzma2` | ok | 144.841 | 107.2 | +0.00% |
| `7z-lzma` | ok | 144.846 | 107.1 | +0.00% |
| `winrar` | ok | 148.261 | 48.9 | +2.36% |
| `7z-ppmd` | ok | 149.306 | 86.1 | +3.08% |
| `7z-bzip2` | ok | 160.398 | 265.2 | +10.74% |
| `windows-zip` | ok | 181.973 | 13.8 | +25.64% |

Notes:

- Lower end size is better.
- The `7z-deflate` lane is omitted here because the latest aggregate run was partial, not a clean apples-to-apples result.
- Weissman scoring is still tracked internally, but DevZIP is gated primarily on end size.
- The goal is smaller archives, not yelling until the compression ratio goes over 9000.

## Per-Type Highlights

Against `7z-lzma2`, the current shipping native lane reports:

- Text and structured data: `-32.67%`
- Raw bitmaps: `-23.84%`
- JPEG: `-1.54%`
- PNG: `-0.95%`
- Video: `-0.94%`
- Random data: `-0.01%`
- Software binaries: near-tied at `+0.32%`

## Latest Native Backend Experiments

All rows below are native-only backend variants. They all produce readable `.dvz` files.

| Dataset | Backend | End Size (MB) | Total Time (s) | Delta vs `best-of-two` |
| --- | --- | ---: | ---: | ---: |
| `large-text` | `best-of-two` | 175.432 | 516.8 | baseline |
| `large-text` | `best-of-three-ppmd` | 175.432 | 556.8 | +3 bytes |
| `large-text` | `selective-zpaq5` | 175.432 | 521.6 | +4 bytes |
| `large-code` | `best-of-two` | 151.548 | 486.3 | baseline |
| `large-code` | `best-of-three-ppmd` | 151.212 | 525.5 | -0.22% |
| `large-code` | `selective-zpaq5` | 150.442 | 535.8 | -0.73% |

Current takeaway:

- `best-of-three-ppmd` does not help the large text corpus in this pipeline, but it does help code-heavy data a little.
- `selective-zpaq5` is the strongest measured code-heavy candidate so far. On `large-code`, it currently has the highest scouter reading.
- `mixed-large` still needs a full rerun before the shipping default should change, so this is not the Super Saiyan switchover yet.

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

- `best-of-two` (default)
- `best-of-three-ppmd`
- `selective-zpaq5`
- `lzma2`
- `ppmd`
- `libzpaq-4`
- `libzpaq-5`

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

Basic compression:

```powershell
.\native\engine\build\devzip_cli.exe compress "path\to\folder" "archive.dvz"
```

Select an experimental backend:

```powershell
.\native\engine\build\devzip_cli.exe --backend selective-zpaq5 compress "path\to\folder" "archive.dvz"
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
