# Benchmark Results

Current published corpus: `mixed-large`

## Shipping Lane Snapshot

| Tool | Status | End Size (MB) | Total Time (s) | Delta vs 7z-lzma2 | Weissman Score |
| --- | --- | ---: | ---: | ---: | ---: |
| `devzip-native` | ok | 143.815 | 304.2 | -0.71% | 0.4148 |
| `7z-lzma2` | ok | 144.841 | 107.2 | +0.00% | 0.5038 |
| `7z-lzma` | ok | 144.846 | 107.1 | +0.00% | 0.5039 |
| `7z-ppmd` | ok | 149.306 | 86.1 | +3.08% | 0.5127 |
| `winrar` | ok | 148.261 | 48.9 | +2.36% | 0.5916 |
| `7z-bzip2` | ok | 160.398 | 265.2 | +10.74% | 0.3811 |
| `windows-zip` | ok | 181.973 | 13.8 | +25.64% | 0.7135 |

Notes:

- Lower end size is better.
- `7z-deflate` is intentionally omitted here because the latest aggregate lane was partial, not a clean final comparison row.
- Weissman remains informational; DevZIP is optimized primarily for end size.

## Per-Type Highlights

Against `7z-lzma2`, the current shipping native lane reports:

- Text and structured data: `-32.67%`
- Raw bitmaps: `-23.84%`
- JPEG: `-1.54%`
- PNG: `-0.95%`
- Video: `-0.94%`
- Random data: `-0.01%`
- Software binaries: near-tied at `+0.32%`

## Latest Native Candidate Bake-Off

These are native backend experiments only. All of them still produce readable `.dvz` files.

| Dataset | Backend | End Size (MB) | Total Time (s) | Delta vs `best-of-two` |
| --- | --- | ---: | ---: | ---: |
| `large-text` | `best-of-two` | 175.432 | 516.8 | baseline |
| `large-text` | `best-of-three-ppmd` | 175.432 | 556.8 | +3 bytes |
| `large-text` | `selective-zpaq5` | 175.432 | 521.6 | +4 bytes |
| `large-code` | `best-of-two` | 151.548 | 486.3 | baseline |
| `large-code` | `best-of-three-ppmd` | 151.212 | 525.5 | -0.22% |
| `large-code` | `selective-zpaq5` | 150.442 | 535.8 | -0.73% |

Current takeaway:

- `best-of-three-ppmd` is effectively a tie on `large-text` and a small improvement on `large-code`.
- `selective-zpaq5` is the strongest measured code-heavy candidate so far.
- `mixed-large` still needs a full candidate rerun before the shipping default should change.
