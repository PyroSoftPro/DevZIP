# DZCM — DevZIP Context-Mixing Codec

DZCM is a compression codec written from scratch for DevZIP. It is not a wrapper
around an existing library: the arithmetic coder, the models, the mixer, and the
SSE stages are all original code, so DevZIP owns a permissively licensed strong
coder and can do something no off-the-shelf compressor can — feed it structural
hints from the surrounding transform pipeline.

Source: `native/engine/src/backends/dzcm_backend.cpp`.

## Why build our own?

The competitive study (`docs/benchmarks/competitive-landscape.md`) showed the one
place DevZIP trails the field is raw code/exe streams, where context-mixing
coders (ZPAQ-5, kanzi -l9, paq8px) beat LZMA2/PPMd. Rather than vendor a
GPL/closed CM coder, we grow our own in the `best-of-N` pool. Adding a codec to
`best-of-N` is strictly safe: every block is tagged with its winning codec and
the smallest wins, so DZCM can only ever reduce archive size, never grow it.

## How it works

DZCM predicts the input one **bit** at a time and codes each bit with a binary
arithmetic coder. The probability for each bit comes from blending many models:

1. **Finite-context models** — orders 0, 1, 2, 3, 4, 5, 6, and 8. Each maps a
   hash of the recent bytes (plus the partial current byte) to an adaptive
   probability counter. The counter is a packed `uint32` holding a 22-bit
   probability and a 10-bit hit count; the count gives it a self-slowing
   learning rate (fast early, stable once a context is well seen).
2. **Word model** — a rolling hash over the current run of alphanumeric bytes,
   reset on punctuation/whitespace. Captures token-level structure in text.
3. **Sparse models** — skip-byte contexts (e.g. bytes at offsets 1 and 3) that
   pick up fixed-stride structure in records and binaries.
4. **Match model** — hashes the last few bytes to find the most recent identical
   context in history and predicts the byte that followed it, with confidence
   proportional to how long the match has held. This is the big win on
   repetitive data.

Every model emits a probability; each is mapped through `stretch` (logit) and
combined by a **logistic mixer** whose weights are trained online by the
prediction error and selected by a small context (partial byte + a history bit).
The mixed probability is then refined by two **SSE/APM** stages (interpolated
adaptive maps keyed by the current byte context) before it drives the coder.

Everything is integer-only and the decoder replays the identical model updates,
so archives are portable and round-trip **byte-exact**. If the coder ever fails
to beat a verbatim copy (e.g. already-compressed data), the block is stored raw
behind a one-byte frame.

## Measured results

Backend-only, identical DevZIP pipeline, this machine (5900X). Bytes, smaller is
better; `best-of-N` keeps the smallest per block so it is the realistic shipping
number.

| Corpus | `lzma2` | `bsc` | `ppmd` | **`dzcm`** | `zpaq5` |
| --- | ---: | ---: | ---: | ---: | ---: |
| code | 216,684 | 207,560 | 192,501 | **201,014** | 169,879 |
| text | 1,531,493 | — | — | **1,066,340** | 945,068 |
| exe | 7,909,881 | — | — | **9,555,630** | 7,273,338 |

- **Text:** DZCM beats LZMA2 by **−30%** and libbsc/PPMd-class coders, trailing
  only ZPAQ-5 — exactly where context mixing is expected to shine.
- **Code:** DZCM beats LZMA2 (−7%) and libbsc, behind PPMd and ZPAQ-5.
- **Exe:** DZCM trails here; machine code needs better structural modeling than
  a generic byte CM provides (see roadmap).

All results above were produced with `--verify`, which compresses then
decompresses and compares against the original; code was additionally extracted
and SHA-256-compared file-by-file (120/120 identical).

## Usage

```
devzip_cli compress <src> <archive.dvz> --backend dzcm --verify
devzip_cli compress <src> <archive.dvz> --backend best-of-n:lzma2,zpaq5,dzcm
```

## Roadmap to beating ZPAQ-5

The remaining gap to ZPAQ-5 is the modeling primitive, not the coder. The proven
next steps, in order of expected impact:

1. **Nonstationary bit-history states + StateMap (ICM).** Replace direct
   probability counters with an 8-bit bit-history state per context mapped
   through an adaptive `StateMap`. This is the single biggest PAQ/ZPAQ advantage.
2. **ISSE chain.** Refine the prediction order-by-order with per-context adaptive
   2-input mixers keyed by bit-history state (not just a context hash, which is
   what the v2 experiment tried and why it underperformed the flat mix).
3. **Executable model.** A dedicated address/opcode model (on top of the existing
   BCJ transforms) to close the exe gap.
4. **Transform-aware modeling.** Feed the pipeline's per-span type tag (text /
   code / binary / already-transformed) into model selection and mixer context —
   information standalone coders never have.

Each is additive and safe behind `best-of-N`.
