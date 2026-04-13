#!/usr/bin/env python3
"""
Generate large-scale (~1 GB) benchmark datasets for DevZip performance testing.

Dataset types generated:
  large-text   prose, logs, CSV, JSON — 40% unique, 30% exact duplicates,
               20% near-duplicates, 10% random bytes
  large-code   C++, Python, YAML, config — same duplicate mix as text
  large-jpeg   real JPEG photos from the wcDogg corpus + duplicates/near-dups
  large-png    real PNG images from the wcDogg corpus + synthetics + duplicates
  large-raw    uncompressed BMPs and TIFFs from the Kodak corpus + synthetics
  large-mixed  one slice of every type above in a single 1 GB corpus

Run from the repository root:
    python benchmarks/tools/generate_large_datasets.py

Options:
    --target-dir PATH   root for generated datasets
                        (default: sample-data/large-benchmarks)
    --source-images PATH  wcDogg-test-files directory
                        (default: sample-data/wcDogg-test-files)
    --size-gb FLOAT     approximate size per dataset in GB (default: 1.0)
    --types LIST        comma-separated subset to generate, e.g. text,code,mixed
                        (default: all)
    --seed INT          random seed for reproducibility (default: 42)
    --skip-existing     skip datasets whose directory already exists
"""

from __future__ import annotations

import argparse
import math
import os
import random
import shutil
import struct
import sys
import time
import zlib
from pathlib import Path

# ---------------------------------------------------------------------------
# Text generation helpers
# ---------------------------------------------------------------------------

_WORDS = (
    "the quick brown fox jumps over the lazy dog and then returned home "
    "compress decompress archive extract backup restore version release build "
    "data file directory folder path size bytes megabytes gigabytes storage "
    "system network server client request response error warning debug info "
    "function class method interface implementation algorithm performance test "
    "result output input parameter configuration option settings default value "
    "string integer float boolean null true false list array map dictionary "
    "timestamp date time year month day hour minute second millisecond "
    "memory cpu disk io thread process kernel user mode stack heap pointer "
    "engine pipeline transform filter encode decode compress expand ratio "
    "image pixel width height channel color depth format jpeg png bmp tiff "
    "source target destination origin remote local cache buffer stream chunk "
    "block sector page frame window segment fragment packet header footer "
    "ascii unicode utf8 binary hex octal decimal base64 checksum hash crc "
    "lzma zstd bzip2 deflate gzip lz4 snappy brotli zopfli context model "
    "corpus benchmark metric weissman ratio delta baseline shipping compare "
    "alpha beta release candidate stable experimental prototype production "
    "windows linux macos unix posix ntfs ext4 fat32 apfs hfs filesystem "
    "cmake ninja make gcc clang msvc llvm toolchain compiler linker object "
    "library executable shared static dynamic link symbol export import flag "
    "debug release profile optimize inline loop branch predict speculate "
    "register operand instruction cycle latency throughput bandwidth capacity "
    "enterprise solution scalable robust reliable secure performant efficient "
    "customer user developer engineer architect analyst manager director team "
    "project sprint iteration milestone deliverable requirement specification "
    "documentation readme changelog license copyright notice attribution "
    "open source community contribution pull request review merge conflict "
    "branch commit tag push fetch clone repository remote origin upstream "
    "api endpoint route handler middleware authentication authorization token "
    "session cookie header payload body status code redirect resource uri url "
    "database table column row index query transaction commit rollback schema "
    "migration seed fixture mock stub spy assertion expect describe context "
    "worker queue job task scheduler cron trigger event listener callback "
    "promise future async await coroutine generator iterator protocol adapter "
).split()

_LOG_PATHS = [
    "/api/v1/compress", "/api/v1/extract", "/api/v2/archive", "/health",
    "/metrics", "/status", "/upload", "/download", "/files", "/jobs",
    "/api/v1/verify", "/api/v2/stream", "/admin/settings", "/auth/token",
]
_LOG_METHODS = ["GET", "POST", "PUT", "DELETE", "PATCH"]
_LOG_CODES   = [200, 200, 200, 200, 201, 204, 301, 400, 401, 403, 404, 500]
_LOG_AGENTS  = [
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36",
    "curl/8.4.0", "Python-urllib/3.11", "Go-http-client/2.0",
    "DevZip-CLI/1.0", "Benchmark-Harness/2.0",
]

_CPP_TYPES   = ["int", "uint32_t", "uint64_t", "size_t", "bool", "float",
                "double", "std::string", "std::vector<uint8_t>", "std::span<const uint8_t>"]
_CPP_NAMES   = ["buffer", "data", "size", "offset", "result", "status",
                "context", "handle", "stream", "block", "chunk", "payload",
                "header", "footer", "checksum", "digest", "level", "flags"]

_PY_NAMES    = ["process", "encode", "decode", "compress", "expand",
                "validate", "serialize", "deserialize", "transform", "filter",
                "build", "parse", "format", "convert", "analyze", "measure"]


def _rwords(rng: random.Random, n: int) -> str:
    return " ".join(rng.choices(_WORDS, k=n))


def _sentence(rng: random.Random) -> str:
    length = rng.randint(6, 20)
    s = _rwords(rng, length)
    return s[0].upper() + s[1:] + rng.choice([".", ".", ".", "!", "?"])


def _paragraph(rng: random.Random) -> str:
    return "  ".join(_sentence(rng) for _ in range(rng.randint(3, 8)))


def _log_line(rng: random.Random, ts_base: int) -> str:
    ts   = ts_base + rng.randint(0, 3600)
    ip   = f"10.{rng.randint(0,255)}.{rng.randint(0,255)}.{rng.randint(1,254)}"
    meth = rng.choice(_LOG_METHODS)
    path = rng.choice(_LOG_PATHS)
    code = rng.choice(_LOG_CODES)
    size = rng.randint(128, 1_048_576)
    ms   = rng.randint(1, 9999)
    ua   = rng.choice(_LOG_AGENTS)
    return f'[{ts}] {ip} "{meth} {path} HTTP/1.1" {code} {size} {ms}ms "{ua}"\n'


def _csv_row(rng: random.Random, cols: list[str]) -> str:
    vals = []
    for col in cols:
        if "id" in col:
            vals.append(str(rng.randint(1, 999_999)))
        elif "name" in col:
            vals.append(_rwords(rng, 2).replace(",", ""))
        elif "size" in col or "bytes" in col:
            vals.append(str(rng.randint(1024, 1_073_741_824)))
        elif "ratio" in col or "score" in col:
            vals.append(f"{rng.uniform(0.1, 10.0):.4f}")
        elif "time" in col or "ms" in col:
            vals.append(str(rng.randint(1, 100_000)))
        else:
            vals.append(_rwords(rng, rng.randint(1, 4)).replace(",", ""))
    return ",".join(vals) + "\n"


def _json_line(rng: random.Random) -> str:
    import json
    obj = {
        "id":        rng.randint(1, 10_000_000),
        "name":      _rwords(rng, 2),
        "status":    rng.choice(["ok", "error", "pending", "skipped"]),
        "size":      rng.randint(1024, 1_073_741_824),
        "ratio":     round(rng.uniform(0.1, 10.0), 4),
        "elapsed_ms": rng.randint(1, 100_000),
        "tags":      rng.choices(_WORDS, k=rng.randint(2, 6)),
    }
    return json.dumps(obj) + "\n"


def _cpp_function(rng: random.Random) -> str:
    ret   = rng.choice(_CPP_TYPES)
    name  = rng.choice(_PY_NAMES) + "_" + rng.choice(_CPP_NAMES)
    p1t   = rng.choice(_CPP_TYPES)
    p1n   = rng.choice(_CPP_NAMES) + "_a"
    p2t   = rng.choice(_CPP_TYPES)
    p2n   = rng.choice(_CPP_NAMES) + "_b"
    body_var = rng.choice(_CPP_NAMES)
    body_val = rng.randint(0, 65535)
    lines = [
        f"{ret} {name}({p1t} {p1n}, {p2t} {p2n}) {{",
        f"  {rng.choice(_CPP_TYPES)} {body_var} = {body_val};",
        f"  // {_sentence(rng)}",
        f"  (void){p1n}; (void){p2n};",
        f"  return static_cast<{ret}>({body_var});",
        "}",
        "",
    ]
    return "\n".join(lines)


def _cpp_class(rng: random.Random, n_methods: int = 4) -> str:
    name   = "".join(w.capitalize() for w in rng.choices(_PY_NAMES, k=2))
    base   = "".join(w.capitalize() for w in rng.choices(_PY_NAMES, k=2))
    lines  = [
        f"// {_sentence(rng)}",
        f"class {name} : public {base} {{",
        "public:",
    ]
    for _ in range(n_methods):
        ret  = rng.choice(["void", "bool", "int", "std::string"])
        meth = rng.choice(_PY_NAMES) + "_" + rng.choice(_CPP_NAMES)
        param = rng.choice(_CPP_TYPES) + " " + rng.choice(_CPP_NAMES)
        lines += [
            f"  {ret} {meth}({param}) override;",
        ]
    lines += ["", "private:"]
    for _ in range(rng.randint(2, 5)):
        ftype = rng.choice(_CPP_TYPES)
        fname = rng.choice(_CPP_NAMES) + "_"
        lines.append(f"  {ftype} {fname}{{}};")
    lines += ["};", ""]
    return "\n".join(lines)


def _python_function(rng: random.Random) -> str:
    name    = rng.choice(_PY_NAMES) + "_" + rng.choice(_CPP_NAMES)
    p1      = rng.choice(_CPP_NAMES)
    p2      = rng.choice(_CPP_NAMES)
    ret_var = rng.choice(_CPP_NAMES)
    lines   = [
        f"def {name}({p1}, {p2}):",
        f'    """{_sentence(rng)}"""',
        f"    # {_sentence(rng)}",
        f"    {ret_var} = {p1}",
        f"    _ = {p2}",
        f"    return {ret_var}",
        "",
    ]
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Binary / image generation helpers
# ---------------------------------------------------------------------------

def _png_bytes(width: int, height: int, r: int, g: int, b: int,
               noise: int = 0, rng: random.Random | None = None) -> bytes:
    """Minimal valid PNG; solid color with optional per-pixel noise."""
    raw = bytearray()
    for _y in range(height):
        raw.append(0)  # filter = None
        for _x in range(width):
            nr = max(0, min(255, r + (rng.randint(-noise, noise) if noise else 0)))
            ng = max(0, min(255, g + (rng.randint(-noise, noise) if noise else 0)))
            nb = max(0, min(255, b + (rng.randint(-noise, noise) if noise else 0)))
            raw += bytes([nr, ng, nb])

    def chunk(tag: bytes, data: bytes) -> bytes:
        crc_data = tag + data
        return (struct.pack(">I", len(data)) + crc_data
                + struct.pack(">I", zlib.crc32(crc_data) & 0xFFFFFFFF))

    sig  = b"\x89PNG\r\n\x1a\n"
    ihdr = chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    idat = chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    iend = chunk(b"IEND", b"")
    return sig + ihdr + idat + iend


def _bmp_bytes(width: int, height: int, r: int, g: int, b: int,
               noise: int = 0, rng: random.Random | None = None) -> bytes:
    """Minimal uncompressed 24-bit BMP."""
    row_bytes  = (width * 3 + 3) & ~3  # 4-byte aligned
    pixel_data = bytearray()
    for _y in range(height):  # BMP is bottom-up, but order doesn't matter for size
        for _x in range(width):
            br = max(0, min(255, b + (rng.randint(-noise, noise) if noise else 0)))
            bg = max(0, min(255, g + (rng.randint(-noise, noise) if noise else 0)))
            bb = max(0, min(255, r + (rng.randint(-noise, noise) if noise else 0)))
            pixel_data += bytes([br, bg, bb])  # BGR order in BMP
        pixel_data += bytes(row_bytes - width * 3)   # padding

    file_size = 54 + len(pixel_data)
    header = struct.pack("<2sIHHI", b"BM", file_size, 0, 0, 54)
    dib    = struct.pack("<IiiHHIIiiII",
                         40, width, height, 1, 24, 0,
                         len(pixel_data), 2835, 2835, 0, 0)
    return header + dib + bytes(pixel_data)


def _perturb(data: bytes, rng: random.Random, n: int = 4) -> bytes:
    """Return a copy of data with n random byte values changed."""
    ba = bytearray(data)
    for _ in range(n):
        pos = rng.randrange(len(ba))
        ba[pos] = rng.randint(0, 255)
    return bytes(ba)


# ---------------------------------------------------------------------------
# File writers
# ---------------------------------------------------------------------------

def write_text_file(path: Path, target_bytes: int, rng: random.Random) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    written = 0
    with path.open("w", encoding="utf-8") as f:
        while written < target_bytes:
            para = _paragraph(rng) + "\n\n"
            f.write(para)
            written += len(para.encode())


def write_log_file(path: Path, target_bytes: int, rng: random.Random) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    ts_base = 1_700_000_000
    written = 0
    with path.open("w", encoding="utf-8") as f:
        while written < target_bytes:
            line = _log_line(rng, ts_base)
            f.write(line)
            written += len(line.encode())


def write_csv_file(path: Path, target_bytes: int, rng: random.Random) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    cols = ["id", "name", "tool", "dataset", "size_bytes", "ratio",
            "elapsed_ms", "status", "score", "tag"]
    written = 0
    with path.open("w", encoding="utf-8") as f:
        header = ",".join(cols) + "\n"
        f.write(header)
        written += len(header.encode())
        while written < target_bytes:
            row = _csv_row(rng, cols)
            f.write(row)
            written += len(row.encode())


def write_jsonl_file(path: Path, target_bytes: int, rng: random.Random) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    written = 0
    with path.open("w", encoding="utf-8") as f:
        while written < target_bytes:
            line = _json_line(rng)
            f.write(line)
            written += len(line.encode())


def write_cpp_file(path: Path, target_bytes: int, rng: random.Random,
                   is_header: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    guard  = path.stem.upper() + "_H"
    lines  = []
    if is_header:
        lines = [f"#pragma once", f"// {_sentence(rng)}", ""]
    else:
        lines = [f'#include "{path.stem}.h"', f"// {_sentence(rng)}", ""]
    written = sum(len(l.encode()) + 1 for l in lines)
    with path.open("w", encoding="utf-8") as f:
        f.write("\n".join(lines))
        while written < target_bytes:
            block = _cpp_class(rng, rng.randint(3, 8)) if rng.random() < 0.3 else _cpp_function(rng)
            f.write(block)
            written += len(block.encode())


def write_python_file(path: Path, target_bytes: int, rng: random.Random) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    header = f'"""Generated module: {_sentence(rng)}"""\n\nimport os\nimport sys\n\n'
    written = len(header.encode())
    with path.open("w", encoding="utf-8") as f:
        f.write(header)
        while written < target_bytes:
            fn = _python_function(rng)
            f.write(fn)
            written += len(fn.encode())


def write_random_file(path: Path, target_bytes: int, rng: random.Random) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    seed_bytes = rng.randbytes(8)
    # Use os.urandom so blocks are truly independent (no compressible pattern)
    chunk_size = 65536
    with path.open("wb") as f:
        remaining = target_bytes
        while remaining > 0:
            n = min(chunk_size, remaining)
            f.write(os.urandom(n))
            remaining -= n


def write_synthetic_png(path: Path, width: int, height: int,
                        rng: random.Random, noise: int = 0) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    r, g, b = rng.randint(0, 255), rng.randint(0, 255), rng.randint(0, 255)
    path.write_bytes(_png_bytes(width, height, r, g, b, noise, rng))


def write_synthetic_bmp(path: Path, width: int, height: int,
                        rng: random.Random, noise: int = 0) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    r, g, b = rng.randint(0, 255), rng.randint(0, 255), rng.randint(0, 255)
    path.write_bytes(_bmp_bytes(width, height, r, g, b, noise, rng))


# ---------------------------------------------------------------------------
# Dataset builders
# ---------------------------------------------------------------------------

def _copy_or_link(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def _fill_with_copies(src_files: list[Path], out_dir: Path, prefix: str,
                      target_bytes: int, rng: random.Random,
                      near_dup_fraction: float = 0.0) -> int:
    """Copy src_files into out_dir (cycling) until target_bytes is reached.
    near_dup_fraction of copies will have a few bytes randomly perturbed."""
    written = 0
    cycle   = 0
    shuffled = list(src_files)
    rng.shuffle(shuffled)
    while written < target_bytes:
        for src in shuffled:
            if written >= target_bytes:
                break
            dst = out_dir / f"{prefix}_{cycle:04d}_{src.name}"
            dst.parent.mkdir(parents=True, exist_ok=True)
            if near_dup_fraction > 0 and rng.random() < near_dup_fraction:
                raw = src.read_bytes()
                dst.write_bytes(_perturb(raw, rng, n=rng.randint(1, 8)))
            else:
                shutil.copy2(src, dst)
            written += src.stat().st_size
        cycle += 1
        rng.shuffle(shuffled)
    return written


def _report(label: str, out_dir: Path) -> None:
    total = sum(f.stat().st_size for f in out_dir.rglob("*") if f.is_file())
    n     = sum(1 for f in out_dir.rglob("*") if f.is_file())
    print(f"  {label}: {n} files, {total / 1e9:.3f} GB  ->  {out_dir}")


# ---- TEXT ------------------------------------------------------------------

def create_text_dataset(out_dir: Path, target_bytes: int, rng: random.Random) -> None:
    print("[large-text] generating …")
    out_dir.mkdir(parents=True, exist_ok=True)

    prose_dir    = out_dir / "prose"
    log_dir      = out_dir / "logs"
    csv_dir      = out_dir / "csv"
    jsonl_dir    = out_dir / "jsonlines"
    dup_dir      = out_dir / "duplicates"
    neardup_dir  = out_dir / "near-duplicates"
    random_dir   = out_dir / "random-segments"

    # 40% unique generated content
    unique_budget  = int(target_bytes * 0.40)
    prose_budget   = unique_budget // 4
    log_budget     = unique_budget // 4
    csv_budget     = unique_budget // 4
    jsonl_budget   = unique_budget - prose_budget - log_budget - csv_budget

    chunk = max(2 * 1024 * 1024, prose_budget // 20)   # ~20 files per category
    originals: list[Path] = []

    for i in range(math.ceil(prose_budget / chunk)):
        p = prose_dir / f"prose_{i:04d}.txt"
        write_text_file(p, chunk, rng)
        originals.append(p)

    for i in range(math.ceil(log_budget / chunk)):
        p = log_dir / f"access_{i:04d}.log"
        write_log_file(p, chunk, rng)
        originals.append(p)

    for i in range(math.ceil(csv_budget / chunk)):
        p = csv_dir / f"metrics_{i:04d}.csv"
        write_csv_file(p, chunk, rng)
        originals.append(p)

    for i in range(math.ceil(jsonl_budget / chunk)):
        p = jsonl_dir / f"records_{i:04d}.jsonl"
        write_jsonl_file(p, chunk, rng)
        originals.append(p)

    # 30% exact duplicates
    dup_budget   = int(target_bytes * 0.30)
    _fill_with_copies(originals, dup_dir, "dup", dup_budget, rng,
                      near_dup_fraction=0.0)

    # 20% near-duplicates (1–3% bytes changed)
    nd_budget    = int(target_bytes * 0.20)
    _fill_with_copies(originals, neardup_dir, "neardup", nd_budget, rng,
                      near_dup_fraction=1.0)

    # 10% random bytes (incompressible padding)
    rand_budget  = target_bytes - (prose_budget + log_budget + csv_budget +
                                    jsonl_budget + dup_budget + nd_budget)
    rand_chunk   = max(1 * 1024 * 1024, rand_budget // 5)
    for i in range(math.ceil(rand_budget / rand_chunk)):
        write_random_file(random_dir / f"random_{i:04d}.bin", rand_chunk, rng)

    _report("large-text", out_dir)


# ---- CODE ------------------------------------------------------------------

def create_code_dataset(out_dir: Path, target_bytes: int,
                        engine_src: Path | None, rng: random.Random) -> None:
    print("[large-code] generating …")
    out_dir.mkdir(parents=True, exist_ok=True)

    cpp_dir     = out_dir / "cpp"
    py_dir      = out_dir / "python"
    dup_dir     = out_dir / "duplicates"
    neardup_dir = out_dir / "near-duplicates"
    random_dir  = out_dir / "random-segments"

    unique_budget = int(target_bytes * 0.40)
    cpp_budget    = unique_budget * 2 // 3
    py_budget     = unique_budget - cpp_budget

    chunk  = max(512 * 1024, cpp_budget // 40)
    originals: list[Path] = []

    # C++ files
    for i in range(math.ceil(cpp_budget // 2 / chunk)):
        p = cpp_dir / f"module_{i:04d}.hpp"
        write_cpp_file(p, chunk, rng, is_header=True)
        originals.append(p)
    for i in range(math.ceil(cpp_budget // 2 / chunk)):
        p = cpp_dir / f"module_{i:04d}.cpp"
        write_cpp_file(p, chunk, rng, is_header=False)
        originals.append(p)

    # Python files
    for i in range(math.ceil(py_budget / chunk)):
        p = py_dir / f"module_{i:04d}.py"
        write_python_file(p, chunk, rng)
        originals.append(p)

    # Copy actual engine source if available (adds real-world code patterns)
    if engine_src and engine_src.exists():
        real_src_files = [f for f in engine_src.rglob("*")
                          if f.is_file() and f.suffix in {".cpp", ".h", ".hpp"}]
        real_dir = out_dir / "engine-src"
        for f in real_src_files:
            dst = real_dir / f.relative_to(engine_src)
            _copy_or_link(f, dst)
            originals.append(dst)

    dup_budget  = int(target_bytes * 0.30)
    _fill_with_copies(originals, dup_dir, "dup", dup_budget, rng)

    nd_budget   = int(target_bytes * 0.20)
    _fill_with_copies(originals, neardup_dir, "neardup", nd_budget, rng,
                      near_dup_fraction=1.0)

    rand_budget = target_bytes - (unique_budget + dup_budget + nd_budget)
    rand_chunk  = max(512 * 1024, rand_budget // 5)
    for i in range(math.ceil(rand_budget / rand_chunk)):
        write_random_file(random_dir / f"random_{i:04d}.bin", rand_chunk, rng)

    _report("large-code", out_dir)


# ---- JPEG ------------------------------------------------------------------

def create_jpeg_dataset(out_dir: Path, target_bytes: int,
                        src_jpegs: list[Path], rng: random.Random) -> None:
    print("[large-jpeg] generating …")
    out_dir.mkdir(parents=True, exist_ok=True)

    if not src_jpegs:
        print("  WARNING: no source JPEGs found — skipping large-jpeg")
        return

    real_dir    = out_dir / "originals"
    dup_dir     = out_dir / "duplicates"
    neardup_dir = out_dir / "near-duplicates"

    # First: copy all real JPEGs once
    real_size = 0
    for src in src_jpegs:
        dst = real_dir / src.name
        _copy_or_link(src, dst)
        real_size += src.stat().st_size

    # 30% of budget: exact duplicates
    dup_budget = int(target_bytes * 0.30)
    _fill_with_copies(src_jpegs, dup_dir, "dup", dup_budget, rng)

    # 20% of budget: near-duplicates (perturb a few JPEG comment/header bytes)
    nd_budget  = int(target_bytes * 0.20)
    _fill_with_copies(src_jpegs, neardup_dir, "neardup", nd_budget, rng,
                      near_dup_fraction=1.0)

    # Fill remaining with more real copies if we're under budget
    copied = real_size + dup_budget + nd_budget
    if copied < target_bytes:
        extra_dir = out_dir / "extra-copies"
        _fill_with_copies(src_jpegs, extra_dir, "extra",
                          target_bytes - copied, rng)

    _report("large-jpeg", out_dir)


# ---- PNG -------------------------------------------------------------------

def create_png_dataset(out_dir: Path, target_bytes: int,
                       src_pngs: list[Path], rng: random.Random) -> None:
    print("[large-png] generating …")
    out_dir.mkdir(parents=True, exist_ok=True)

    real_dir    = out_dir / "originals"
    dup_dir     = out_dir / "duplicates"
    neardup_dir = out_dir / "near-duplicates"
    synth_dir   = out_dir / "synthetic"

    # Copy all real PNGs
    real_size = 0
    for src in src_pngs:
        dst = real_dir / src.parent.name / src.name
        _copy_or_link(src, dst)
        real_size += src.stat().st_size

    # Synthetic PNGs: solid colors, gradients, noisy fills
    # Each ~2 MB at 1024×1024 → generates several hundred MB of variety
    synth_size    = 0
    synth_budget  = int(target_bytes * 0.10)
    synth_sizes   = [(1024, 1024), (800, 600), (1920, 1080), (512, 512)]
    synth_noises  = [0, 0, 8, 24, 64]
    i = 0
    while synth_size < synth_budget:
        w, h = rng.choice(synth_sizes)
        noise = rng.choice(synth_noises)
        p = synth_dir / f"synthetic_{i:05d}_{'noisy' if noise else 'solid'}.png"
        write_synthetic_png(p, w, h, rng, noise)
        synth_size += p.stat().st_size
        i += 1

    dup_budget = int(target_bytes * 0.25)
    _fill_with_copies(src_pngs, dup_dir, "dup", dup_budget, rng)

    nd_budget  = int(target_bytes * 0.15)
    _fill_with_copies(src_pngs, neardup_dir, "neardup", nd_budget, rng,
                      near_dup_fraction=1.0)

    copied = real_size + synth_size + dup_budget + nd_budget
    if copied < target_bytes:
        extra_dir = out_dir / "extra-copies"
        _fill_with_copies(src_pngs, extra_dir, "extra",
                          target_bytes - copied, rng)

    _report("large-png", out_dir)


# ---- RAW -------------------------------------------------------------------

def create_raw_dataset(out_dir: Path, target_bytes: int,
                       src_bmps: list[Path], src_tiffs: list[Path],
                       rng: random.Random) -> None:
    print("[large-raw] generating …")
    out_dir.mkdir(parents=True, exist_ok=True)

    bmp_dir     = out_dir / "bitmaps"
    tiff_dir    = out_dir / "tiff"
    synth_dir   = out_dir / "synthetic-bmp"
    dup_dir     = out_dir / "duplicates"
    neardup_dir = out_dir / "near-duplicates"

    real_size = 0
    for src in src_bmps:
        dst = bmp_dir / src.name
        _copy_or_link(src, dst)
        real_size += src.stat().st_size
    for src in src_tiffs:
        dst = tiff_dir / src.name
        _copy_or_link(src, dst)
        real_size += src.stat().st_size

    # Synthetic BMPs to pad if we don't have enough real raw images
    synth_budget = max(0, int(target_bytes * 0.15) - real_size)
    synth_sizes  = [(2048, 1536), (1920, 1080), (1024, 768)]
    i = 0
    synth_size = 0
    while synth_size < synth_budget:
        w, h = rng.choice(synth_sizes)
        p = synth_dir / f"synthetic_{i:05d}.bmp"
        write_synthetic_bmp(p, w, h, rng, noise=rng.choice([0, 0, 16, 32]))
        synth_size += p.stat().st_size
        i += 1

    all_real = src_bmps + src_tiffs
    dup_budget = int(target_bytes * 0.25)
    if all_real:
        _fill_with_copies(all_real, dup_dir, "dup", dup_budget, rng)

    nd_budget = int(target_bytes * 0.15)
    if all_real:
        _fill_with_copies(all_real, neardup_dir, "neardup", nd_budget, rng,
                          near_dup_fraction=1.0)

    copied = real_size + synth_size + dup_budget + nd_budget
    if copied < target_bytes and all_real:
        extra_dir = out_dir / "extra-copies"
        _fill_with_copies(all_real, extra_dir, "extra",
                          target_bytes - copied, rng)

    _report("large-raw", out_dir)


# ---- MIXED -----------------------------------------------------------------

def create_mixed_dataset(out_dir: Path, target_bytes: int,
                         src_images: Path, engine_src: Path | None,
                         rng: random.Random) -> None:
    print("[large-mixed] generating …")
    out_dir.mkdir(parents=True, exist_ok=True)

    # Slices: 25% text, 20% code, 20% JPEG, 20% PNG, 10% raw, 5% random
    text_budget  = int(target_bytes * 0.25)
    code_budget  = int(target_bytes * 0.20)
    jpeg_budget  = int(target_bytes * 0.20)
    png_budget   = int(target_bytes * 0.20)
    raw_budget   = int(target_bytes * 0.10)
    rand_budget  = target_bytes - text_budget - code_budget - jpeg_budget - png_budget - raw_budget

    # Text slice
    t_dir  = out_dir / "text"
    prose_originals: list[Path] = []
    chunk = max(2 * 1024 * 1024, text_budget // 20)
    for i in range(math.ceil(text_budget * 0.55 / chunk)):
        p = t_dir / "prose" / f"prose_{i:04d}.txt"
        write_text_file(p, chunk, rng)
        prose_originals.append(p)
    for i in range(math.ceil(text_budget * 0.25 / chunk)):
        p = t_dir / "logs" / f"access_{i:04d}.log"
        write_log_file(p, chunk, rng)
        prose_originals.append(p)
    _fill_with_copies(prose_originals, t_dir / "duplicates", "dup",
                      int(text_budget * 0.20), rng)

    # Code slice
    c_dir = out_dir / "code"
    code_originals: list[Path] = []
    chunk = max(512 * 1024, code_budget // 40)
    for i in range(math.ceil(code_budget * 0.35 / chunk)):
        p = c_dir / "cpp" / f"module_{i:04d}.hpp"
        write_cpp_file(p, chunk, rng, is_header=True)
        code_originals.append(p)
    for i in range(math.ceil(code_budget * 0.35 / chunk)):
        p = c_dir / "python" / f"module_{i:04d}.py"
        write_python_file(p, chunk, rng)
        code_originals.append(p)
    _fill_with_copies(code_originals, c_dir / "duplicates", "dup",
                      int(code_budget * 0.30), rng)

    # JPEG slice
    src_jpegs = sorted(src_images.rglob("*.jpg")) + sorted(src_images.rglob("*.jpeg"))
    if src_jpegs:
        _fill_with_copies(src_jpegs, out_dir / "jpeg" / "originals", "jpeg",
                          int(jpeg_budget * 0.70), rng)
        _fill_with_copies(src_jpegs, out_dir / "jpeg" / "duplicates", "dup",
                          int(jpeg_budget * 0.30), rng)

    # PNG slice
    src_pngs = sorted(src_images.rglob("*.png"))
    if src_pngs:
        _fill_with_copies(src_pngs, out_dir / "png" / "originals", "png",
                          int(png_budget * 0.60), rng)
        _fill_with_copies(src_pngs, out_dir / "png" / "duplicates", "dup",
                          int(png_budget * 0.25), rng)
        # Synthetics for the remainder
        i = 0
        synth_done = 0
        synth_target = int(png_budget * 0.15)
        while synth_done < synth_target:
            p = out_dir / "png" / "synthetic" / f"synth_{i:05d}.png"
            write_synthetic_png(p, 512, 512, rng, noise=rng.randint(0, 32))
            synth_done += p.stat().st_size
            i += 1

    # Raw BMP slice
    src_bmps = sorted(src_images.rglob("*.bmp"))
    if src_bmps:
        _fill_with_copies(src_bmps, out_dir / "raw" / "bitmaps", "bmp",
                          int(raw_budget * 0.70), rng)
    else:
        i = 0
        raw_done = 0
        while raw_done < raw_budget:
            p = out_dir / "raw" / "synthetic" / f"synth_{i:05d}.bmp"
            write_synthetic_bmp(p, 1024, 768, rng, noise=rng.randint(0, 32))
            raw_done += p.stat().st_size
            i += 1

    # Random slice
    rand_chunk = max(1 * 1024 * 1024, rand_budget // 5)
    for i in range(math.ceil(rand_budget / rand_chunk)):
        write_random_file(out_dir / "random" / f"random_{i:04d}.bin", rand_chunk, rng)

    _report("large-mixed", out_dir)


# ---------------------------------------------------------------------------
# Manifest writer
# ---------------------------------------------------------------------------

TOOLS_BLOCK = {
    "devzip-native": {
        "command": [
            "powershell", "-ExecutionPolicy", "Bypass", "-File",
            "{project_root}\\benchmarks\\tools\\run_devzip_native.ps1",
            "-InputPath", "{input}", "-OutputPath", "{output}"
        ],
        "output_extension": ".dvz"
    },
    "7z-lzma2": {
        "command": [
            "powershell", "-ExecutionPolicy", "Bypass", "-File",
            "{project_root}\\benchmarks\\tools\\run_7zip_max.ps1",
            "-Method", "lzma2", "-InputPath", "{input}", "-OutputPath", "{output}"
        ],
        "output_extension": ".7z"
    },
    "7z-ppmd": {
        "command": [
            "powershell", "-ExecutionPolicy", "Bypass", "-File",
            "{project_root}\\benchmarks\\tools\\run_7zip_max.ps1",
            "-Method", "ppmd", "-InputPath", "{input}", "-OutputPath", "{output}"
        ],
        "output_extension": ".7z"
    },
    "winrar": {
        "command": [
            "powershell", "-ExecutionPolicy", "Bypass", "-File",
            "{project_root}\\benchmarks\\tools\\run_winrar_max.ps1",
            "-InputPath", "{input}", "-OutputPath", "{output}"
        ],
        "output_extension": ".rar"
    },
    "windows-zip": {
        "command": [
            "powershell", "-ExecutionPolicy", "Bypass", "-File",
            "{project_root}\\benchmarks\\tools\\run_windows_zip.ps1",
            "-InputPath", "{input}", "-OutputPath", "{output}"
        ],
        "output_extension": ".zip"
    }
}


def write_manifest(manifest_path: Path, datasets: list[dict]) -> None:
    import json
    manifest = {
        "name": "large-scale",
        "timeout_seconds": 1800,
        "description": (
            "Large-scale (~1 GB per type) benchmark with real images, "
            "generated text/code, duplicates, near-duplicates, and random data. "
            "Designed to amplify compression ratio differences between tools."
        ),
        "goal": {
            "aggregate_minimum_win_percent_vs_7z": 5.0,
            "baseline_tool": "7z-lzma2",
            "shipping_tool": "devzip-native",
            "shipping_backend": "zpaq-m4"
        },
        "scoring": {
            "weissman": {"enabled": True, "baseline": "gzip", "alpha": 1.0}
        },
        "datasets": datasets,
        "tools": TOOLS_BLOCK
    }
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(f"Wrote manifest -> {manifest_path}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--target-dir",    default="sample-data/large-benchmarks",
                   help="Root directory for generated datasets")
    p.add_argument("--source-images", default="sample-data/wcDogg-test-files",
                   help="Path to wcDogg-test-files directory")
    p.add_argument("--engine-src",    default="native/engine/src",
                   help="Path to native engine source tree (for large-code)")
    p.add_argument("--size-gb",       type=float, default=1.0,
                   help="Approximate GB per dataset (default: 1.0)")
    p.add_argument("--types",         default="text,code,jpeg,png,raw,mixed",
                   help="Comma-separated dataset types to generate")
    p.add_argument("--seed",          type=int, default=42,
                   help="Random seed for reproducibility")
    p.add_argument("--skip-existing", action="store_true",
                   help="Skip dataset if its directory already exists")
    return p.parse_args()


def main() -> int:
    args   = parse_args()
    rng    = random.Random(args.seed)
    root   = Path(args.target_dir)
    imgs   = Path(args.source_images)
    esrc   = Path(args.engine_src)
    target = int(args.size_gb * 1_000_000_000)
    types  = {t.strip().lower() for t in args.types.split(",")}

    # Gather source images
    src_jpegs = sorted(imgs.rglob("*.jpg")) + sorted(imgs.rglob("*.jpeg"))
    src_pngs  = sorted(imgs.rglob("*.png"))
    src_bmps  = sorted(imgs.rglob("*.bmp"))
    src_tiffs = sorted(imgs.rglob("*.tif")) + sorted(imgs.rglob("*.tiff"))

    print(f"Source images:  {len(src_jpegs)} JPEGs, {len(src_pngs)} PNGs, "
          f"{len(src_bmps)} BMPs, {len(src_tiffs)} TIFFs")
    print(f"Target size:    {args.size_gb:.1f} GB per dataset")
    print(f"Output root:    {root.resolve()}")
    print()

    dataset_meta: list[dict] = []
    t0 = time.perf_counter()

    def _maybe(name: str, fn, out_sub: str, tags: list[str]) -> None:
        if name not in types:
            return
        out = root / out_sub
        if args.skip_existing and out.exists():
            print(f"[{out_sub}] skipped (already exists)")
        else:
            fn(out, target, rng)
        rel = f"sample-data/large-benchmarks/{out_sub}".replace("\\", "/")
        dataset_meta.append({
            "id": out_sub, "label": f"Large {name} corpus (~{args.size_gb:.0f} GB)",
            "path": rel, "tags": tags
        })

    _maybe("text",  lambda d, t, r: create_text_dataset(d, t, r),
           "large-text",  ["text", "prose", "logs", "duplicates", "large"])

    _maybe("code",  lambda d, t, r: create_code_dataset(d, t, esrc if esrc.exists() else None, r),
           "large-code",  ["code", "text", "repetition", "duplicates", "large"])

    _maybe("jpeg",  lambda d, t, r: create_jpeg_dataset(d, t, src_jpegs, r),
           "large-jpeg",  ["image", "binary", "precompressed", "duplicates", "large"])

    _maybe("png",   lambda d, t, r: create_png_dataset(d, t, src_pngs, r),
           "large-png",   ["image", "binary", "precompressed", "synthetic", "large"])

    _maybe("raw",   lambda d, t, r: create_raw_dataset(d, t, src_bmps, src_tiffs, r),
           "large-raw",   ["image", "binary", "uncompressed", "large"])

    _maybe("mixed", lambda d, t, r: create_mixed_dataset(d, t, imgs, esrc if esrc.exists() else None, r),
           "large-mixed", ["mixed", "text", "image", "binary", "duplicates", "large"])

    # Write manifest
    manifest_path = Path("benchmarks/manifests/large-scale.json")
    write_manifest(manifest_path, dataset_meta)

    elapsed = time.perf_counter() - t0
    print(f"\nDone in {elapsed:.1f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
