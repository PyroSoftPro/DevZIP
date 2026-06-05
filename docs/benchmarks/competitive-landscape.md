# Competitive Landscape — The Best-of-the-Best Compressors

Research digest (June 2026) for choosing what DevZIP should be benchmarked
against. Grouped into tiers by *compression ratio* (DevZIP's optimization axis),
with licensing called out so we know which engines we could **integrate**
(permissive) versus only **benchmark against** (GPL / closed).

DevZIP today already fields LZMA2, ZPAQ (context mixing), PPMd, and libbsc (BWT)
via `best-of-N`, plus brunsli for JPEG and preflate for PNG/Deflate. The targets
below are the engines that beat, match, or define the frontier.

## Tier 1 — Extreme ratio (research-grade, very slow; benchmark-only)

These define the theoretical frontier (Hutter Prize / Large Text Compression
Benchmark). Useful as an "how far from optimal are we" ceiling, not for shipping.

| Tool | Approach | Headline result | License | Notes |
| --- | --- | --- | --- | --- |
| **cmix v21 / fx2-cmix** | Context mixing + NN | enwik9 → ~108–110 MB; Hutter Prize leader (fx2-cmix, Sep 2024) | GPL / research | ~600k ns/byte, ~30 GB RAM — absurdly slow |
| **nncp v3.2** | Transformer NN | enwik9 → 106.6 MB (best *raw* enwik9) | BSD-ish (libnc) | Needs GPU; ~240k ns/byte |
| **paq8px (v213)** | Context mixing w/ typed models | #1 on Silesia, Kodak, Maximum Compression; has JPEG/image/audio models | GPL-2.0 | The practical "max ratio" reference for files |

Takeaway: ZPAQ (which DevZIP already uses) is the same CM family, several rungs
down from paq8px/cmix but actually usable. paq8px is the right *reference ceiling*
to print next to DevZIP `max`.

## Tier 2 — Strong practical archivers (high ratio, usable)

| Tool | Approach | Result vs 7-Zip | License | Integrable? |
| --- | --- | --- | --- | --- |
| **zpaq -m5** | Context mixing | PeaZip: 303 MB → 57.6 MB (19.0%) vs 7z 71.2 MB (23.5%) — ~19% smaller than 7z | MIT/PD (zpaqfranz) | Already in DevZIP |
| **kanzi -l9** | BWT + CM, multi-thread | 211 MB → 41.5 MB vs xz-9 48.8 MB (≈15% smaller than xz at L9) | **Apache-2.0** | Yes — permissive, modular |
| **RAZOR** | Strong LZ | Often beats zpaq with far faster extraction | Closed-source | No (benchmark-only) |
| **nanozip -cc** | Context mixing | Strong, but abandoned ~2011 | Closed-source | No (benchmark-only) |
| **bsc / libbsc** | BWT (ST/QLFC) | 211 MB → 47.9 MB (-T16) | Apache-2.0 | Already in DevZIP |
| **mcm** | Context mixing | Comparable to zpaq/ZCM | GPL | No (GPL) |
| **bzip3** | BWT + LZP + arithmetic | Near high-LZMA on some data | LGPL/MIT | Maybe |

Takeaway: **kanzi** is the most interesting new target — Apache-2.0, multi-codec,
multi-threaded, and at high levels reaches the PAQ8 family. It is both a strong
benchmark rival *and* a candidate backend.

## Tier 3 — Mainstream balanced (the everyday rivals)

Weighted ratios from a 3,420-run real-file benchmark (higher = smaller):

| Tool | Setting | Weighted ratio | License | Notes |
| --- | --- | --- | --- | --- |
| **xz / LZMA2** | `-9e` | 1.667× | PD/LGPL | DevZIP's current baseline / backbone |
| **zstd** | `--ultra -22` (+dicts) | 1.633× | BSD | Near brotli-11, far faster decode |
| **brotli** | `-11` | 1.632× | MIT | Already vendored (brunsli dep); strong on web text |

Takeaway: DevZIP already wins these via `best-of-N` (it *contains* LZMA2 and adds
ZPAQ/BSC on top). zstd-22 and brotli-11 are good "fast-tier" comparison points
for DevZIP `--level fast`.

## Image-specific (where DevZIP's transforms already shine)

### JPEG (lossless transcode, byte-exact)

| Tool | Savings vs original JPEG | License | Status |
| --- | --- | --- | --- |
| **Lepton** (Dropbox) | ~22.7% avg | **Apache-2.0** | Candidate — ~1% better than brunsli |
| **brunsli** | ~20–22% | MIT | **Shipping in DevZIP** (-21.9% measured) |
| **JPEG XL** (`cjxl` transcode) | ~16–22% | BSD-3 | Candidate / benchmark |
| **PackJPG / paq8px JPEG model** | ~23% | GPL | Benchmark-only |

General-purpose compressors only get 1–3% on JPEG, so the recompressor is the
whole game. DevZIP's brunsli result is already at the front of this pack; Lepton
(Apache-2.0) is the one engine that might edge it by ~1%.

### PNG / Deflate (lossless)

| Tool | What it does | Savings | License |
| --- | --- | --- | --- |
| **zopflipng** | Optimal DEFLATE re-encode | ~13% over default PNG; best ~87% of the time | Apache-2.0 |
| **ECT** (Efficient Compression Tool) | zopfli-class, faster | Matches/edges zopfli | permissive |
| **zenzop** (Rust) | zopfli + ECT tricks | Beats ECT-9 at equal iters | MIT/Apache |
| **OxiPNG** | Fast practical optimizer | 2–5% behind zopflipng | MIT |

DevZIP takes a *different* path: preflate undoes the PNG's DEFLATE to raw tokens,
then recompresses with a stronger backend and reconstructs the exact bytes
(-16.4% at `max`). zopflipng/ECT stay within DEFLATE, so they cap lower. The fair
comparison target is "best PNG re-encoder" (zopflipng) vs DevZIP `max` PNG.

## Measured Results — DevZIP vs the rivals (this machine)

Ryzen 5900X / Windows / MSVC Release, same corpora as the overhaul sweep. Sizes
in MB (smaller is better). General-purpose codecs compress a solid TAR of each
corpus; `cjxl`/`zopflipng` run per file. Every byte-exact roundtrip was verified
except zopflipng (a pixel-lossless re-encode whose output bytes differ from the
source). Reproduce with `benchmarks/tools/rival_sweep.ps1`.

| Corpus | 7z-lzma2 | zstd-22 | brotli-11 | kanzi-l9 | per-type | DevZIP bal | DevZIP max |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| text (14.48) | 1.520 | 1.604 | 1.599 | 0.918 | — | **0.901** | **0.901** |
| code (1.11) | 0.193 | 0.199 | 0.192 | _0.151_ | — | 0.162 | 0.162 |
| exe (31.86) | 7.265 | 8.679 | 8.253 | _6.792_ | — | 6.936 | 6.930 |
| jpeg (27.68) | 24.337 | 24.323 | 25.579 | 23.661 | cjxl 20.912 | **19.018** | **19.018** |
| png (22.31) | 21.002 | 20.998 | 21.742 | 20.981 | zopflipng 19.629 | 20.960 | **17.550** |
| **Aggregate** | 54.317 | 55.803 | 57.365 | 52.503 | — | 47.977 | **44.561** |

**Bold** = overall winner for that row; _italic_ = a rival beats DevZIP.

Reference ceiling (paq8px -8, GPL, impractically slow — minutes per MB):
code **0.109 MB** (~33% under DevZIP), confirming how much context-mixing headroom
remains on raw streams. Running it on the larger corpora is impractical (hours).

### What this tells us

- **DevZIP `max` has the smallest aggregate of every tool tested** — 15% under the
  best general-purpose rival (kanzi -l9) and 18% under 7z-lzma2. The win is driven
  by content-aware transforms, not a stronger entropy coder:
  - **JPEG:** brunsli (19.018) beats JPEG XL transcode `cjxl` (20.912).
  - **PNG:** preflate `max` (17.550) beats zopflipng (19.629) **and** stays
    byte-exact to the source; zopflipng rewrites the file.
  - **exe:** architecture-aware BCJ + ZPAQ-5.
- **On raw general-purpose streams, kanzi -l9 (Apache-2.0 context mixing) beats
  DevZIP's current backend** on code (0.151 vs 0.162) and executables
  (6.792 vs 6.930), and nearly ties on text (0.918 vs 0.901). This is the clearest
  improvement target.
- **Action:** add a kanzi-class context-mixing codec as a `best-of-N` candidate
  (kanzi is Apache-2.0, so it can be integrated, not just benchmarked). That would
  likely flip code/exe to DevZIP wins and widen the aggregate lead further.

## Proposed benchmark matrix (what to wire into `run_benchmarks.py`)

Reference ceiling (benchmark-only, GPL/closed):
- `paq8px -9` — max-ratio reference for text/code/exe/images
- `zpaq -m5` — practical CM (DevZIP already uses the engine internally)
- `RAZOR` / `nanozip` — Windows strong-archiver references (manual)

Permissive rivals worth installing (and possibly integrating later):
- `kanzi -l9` (Apache-2.0) — strongest permissive general-purpose target
- `zstd --ultra -22`, `brotli -11` — fast-tier rivals for `--level fast`
- `bzip3` — modern BWT

Per-type recompressors:
- JPEG: `cjxl` (JPEG XL transcode) and **Lepton** vs our brunsli
- PNG: `zopflipng` / `ECT` vs our preflate `max` path

Licensing guardrail: anything **integrated** into the shipping engine must stay
Public-Domain / MIT / Apache-2.0 / BSD. kanzi, Lepton, zstd, brotli, zopfli, and
bzip3 qualify; cmix, paq8px, mcm, RAZOR, and nanozip are **benchmark-only**.

## Reproducing

Rival binaries are not committed (large, third-party, platform-specific — see
`.gitignore`). Recreate `benchmarks/rivals/bin/` with:

- **zstd** — `zstd-v1.5.7-win64.zip` from facebook/zstd releases.
- **brotli + cjxl/djxl** — `jxl-x64-windows-static.zip` from libjxl/libjxl
  releases (ships a static `brotli.exe` and the JPEG XL tools).
- **paq8px** — `paq8px_v215_windows_x64.7z` from hxim/paq8px releases.
- **kanzi** — build flanglet/kanzi-cpp with CMake/MSVC (`kanzi_static.exe`).
- **zopflipng** — build google/zopfli with g++ (compile `src/zopfli/*.c` minus
  `zopfli_bin.c`/`zopfli_lib.c`, plus `src/zopflipng/*` and `lodepng/*`).

Then run `benchmarks/tools/rival_sweep.ps1`.

## Sources

- [Large Text Compression Benchmark — Matt Mahoney](http://mattmahoney.net/dc/text.html)
- [500'000€ Prize for Compressing Human Knowledge (Hutter Prize)](http://prize.hutter1.net/)
- [Hutter Prize — Wikipedia](https://en.wikipedia.org/wiki/Hutter_Prize)
- [Neural-Network-Based Lossless Compression Benchmark (NNLCB)](https://fahaihi.github.io/NNLCB/)
- [Compression on Real Files: 3,420 Benchmark Runs — Turhobr (2026)](https://turhobr.cz/blog/compression-benchmark-real-files-2026)
- [Benchmarking compression programs — MaskRay (Aug 2025)](https://maskray.me/blog/2025-08-31-benchmarking-compression-programs)
- [flanglet/kanzi — GitHub](https://github.com/flanglet/kanzi)
- [flanglet/kanzi — DeepWiki](https://deepwiki.com/flanglet/kanzi)
- [hxim/paq8px — GitHub](https://github.com/hxim/paq8px)
- [Maximum file compression benchmark: 7Z vs ZPAQ vs RAR — PeaZip](https://peazip.github.io/maximum-compression-benchmark.html)
- [RAZOR — strong LZ-based archiver (encode.su thread)](https://encode.su/printthread.php?page=6&t=2829)
- [Lepton: compressing JPEGs at Dropbox (arXiv 1704.06192)](https://ar5iv.labs.arxiv.org/html/1704.06192)
- [JPEG XL overview (arXiv 2506.05987)](https://arxiv.org/html/2506.05987)
- [Benchmarking JPEG XL lossy/lossless — Google Research](https://research.google/pubs/benchmarking-jpeg-xl-lossylossless-image-compression/)
- [Efficient-Compression-Tool (ECT) — GitHub](https://github.com/fhanau/Efficient-Compression-Tool/)
- [imazen/zenzop — GitHub](https://github.com/imazen/zenzop)
- [Zopfli is really good at compressing PNGs — iter.ca](https://iter.ca/post/zopfli/)
- [Lossless PNG with OxiPNG — Z.Tools](https://z.tools/blog/oxipng-lossless-png-pixel-perfect)
