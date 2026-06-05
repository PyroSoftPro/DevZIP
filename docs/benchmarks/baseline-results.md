# Benchmark Results

Compression overhaul sweep (5900X / Windows, MSVC Release build). Lower end size
is better; "Win vs 7z-lzma2" is negative when DevZIP is smaller. Every DevZIP row
below was extracted and compared byte-for-byte against the source (`roundtrip OK`).

## Overhaul Headline — Per-Category Sweep

Source corpora: text 14.47 MB, code 1.10 MB, executables 31.86 MB, JPEG 27.67 MB,
PNG 22.31 MB (97.41 MB total).

| Category | `7z-lzma2` (MB) | DevZIP `balanced` (MB) | Win | DevZIP `max` (MB) | Win |
| --- | ---: | ---: | ---: | ---: | ---: |
| Text / structured | 1.520 | 0.901 | **-40.7%** | 0.901 | **-40.7%** |
| Code | 0.193 | 0.162 | **-16.1%** | 0.162 | **-15.9%** |
| Executables (PE) | 7.265 | 6.936 | **-4.5%** | 6.930 | **-4.6%** |
| JPEG photos | 24.337 | 19.018 | **-21.9%** | 19.018 | **-21.9%** |
| PNG lossless | 21.002 | 20.960 | -0.2% | 17.550 | **-16.4%** |
| **Aggregate** | **54.317** | **47.977** | **-11.7%** | **44.561** | **-18.0%** |

Against the *best* 7-Zip method per category (lzma2 or ppmd), DevZIP `max` is
still **-17.5%** smaller in aggregate.

### Notes

- **Text/code** wins come from `best-of-N` (LZMA2 vs ZPAQ-5 context mixing) plus
  the code dictionary and adaptive LZMA2 literal model.
- **JPEG** `-21.9%` is the brunsli lossless transcode (byte-exact JPEG rebuild).
- **PNG** `-16.4%` requires the preflate deflate-undo, which is gated to `max`
  because it is the single most expensive transform (it inflates IDAT to raw
  pixels and deeply recompresses them).
- **Executables** now win at every level: `balanced` was upgraded from
  `best-of-two` (ZPAQ-4) to `best-of-N` with ZPAQ-5, which models PE binaries
  better than 7-Zip's LZMA2 once the architecture-aware BCJ filter is applied.

## Compression Time (seconds, this machine)

| Category | `7z-lzma2` | DevZIP `balanced` | DevZIP `max` |
| --- | ---: | ---: | ---: |
| Text | 2.7 | 94.5 | 83.0 |
| Code | 0.1 | 2.1 | 2.1 |
| Executables | 6.7 | 91.1 | 89.6 |
| JPEG | 1.8 | 33.4 | 89.1 |
| PNG | 1.2 | 54.1 | 309.7 |

DevZIP trades time for size: ZPAQ-5 context mixing and the preflate PNG path are
the dominant costs. LZMA2 and ZPAQ run concurrently per solid group, and groups
compress in parallel across cores, so wall-time scales with the slowest codec
rather than their sum. Use `--level fast` when speed matters more than size.

## Levels

| Level | Transforms | Backend pool | Use when |
| --- | --- | --- | --- |
| `fast` | BCJ + delta filters | LZMA2 | Speed first |
| `balanced` (default) | + brunsli, code dict, PNG IDAT strip | LZMA2 + ZPAQ-5 | Wins every category, no 5-minute PNG path |
| `max` | + preflate (PNG/zip/gzip) | LZMA2 + ZPAQ-5 + PPMd | Smallest size |
| `insane` | + create-time roundtrip verify | + libbsc BWT | Smallest size with paranoid verification |

## Correctness

- All DevZIP rows above round-trip byte-exact (SHA-256 of extracted tree equals
  source). `insane` additionally decodes and compares every block at create time.
- New transforms (brunsli, preflate, architecture BCJ) self-verify at compress
  time, so a non-reconstructable input falls back to the raw bytes instead of
  producing a bad archive.
- Format bumped to `.dvz` v4; `checked_transform_kind` accepts the new transform
  IDs (7 brunsli, 8 preflate, 9-11 ARM BCJ) so archives stay readable.
