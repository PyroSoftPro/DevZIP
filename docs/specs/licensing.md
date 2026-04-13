# Licensing Boundary

DevZip ships only commercially friendly code in the product artifact.

## Shipping Dependencies

- `libzpaq`: public domain / unencumbered usage grant in the upstream header and `COPYING`
- `divsufsort` portion embedded in upstream `libzpaq.cpp`: MIT
- `LZMA SDK` (`vendor/lzma-sdk`): public domain
- DevZip-authored engine, transform, CLI, benchmark, and UI code: repository license to be chosen by the project owner

## Benchmark-Only Tools

These tools are allowed for local research, corpus comparison, and CI benchmark reporting, but they must not be linked into the shipping application:

- `paq8px` (GPL)
- `cmix` (GPL)

## Rules

- Do not add GPL code under `apps/windows-ui` or `native/engine/src`.
- Keep benchmark adapters and command templates confined to `benchmarks/`.
- If a future dependency has reciprocal licensing, document it here before vendor or runtime integration.

## Review Checklist

- Is the dependency used in the shipped binary or only in local benchmarking?
- Does the dependency require source disclosure for derivative works?
- Is vendored upstream code carrying its own copyright or license notice?
- Are benchmark-only tools clearly absent from runtime resolution paths in shipping builds?
