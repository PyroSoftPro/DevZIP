"""Assemble a credible, fully-local benchmark corpus for the compression overhaul.

No network is required. Sources:
  - text:   synthetic prose / CSV / JSON / XML / log (deterministic)
  - code:   the DevZIP native engine sources + vendored LZMA SDK C sources
  - jpeg:   real photographic JPEGs shipped with Windows (C:\\Windows\\Web)
  - png:    those same photos re-encoded as 24-bit PNG via Pillow (real deflate)
  - exe:    a curated set of native x64 PE binaries from System32 (BCJ lane)

Everything lands under sample-data/bench/<category>/ (gitignored). The script
also (re)writes category + mixed benchmark manifests under benchmarks/manifests/.
"""
from __future__ import annotations

import json
import os
import random
import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BENCH = REPO_ROOT / "sample-data" / "bench"
MANIFESTS = REPO_ROOT / "benchmarks" / "manifests"


def _ps(path: str) -> list[str]:
    return [
        "powershell",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        "{project_root}\\benchmarks\\tools\\" + path,
        "-InputPath",
        "{input}",
        "-OutputPath",
        "{output}",
    ]


def tools_block() -> dict:
    return {
        "devzip-native": {"command": _ps("run_devzip_native.ps1"), "output_extension": ".dvz"},
        "7z-lzma2": {
            "command": [
                "powershell", "-ExecutionPolicy", "Bypass", "-File",
                "{project_root}\\benchmarks\\tools\\run_7zip_max.ps1",
                "-Method", "lzma2", "-InputPath", "{input}", "-OutputPath", "{output}",
            ],
            "output_extension": ".7z",
        },
        "7z-ppmd": {
            "command": [
                "powershell", "-ExecutionPolicy", "Bypass", "-File",
                "{project_root}\\benchmarks\\tools\\run_7zip_max.ps1",
                "-Method", "ppmd", "-InputPath", "{input}", "-OutputPath", "{output}",
            ],
            "output_extension": ".7z",
        },
        "winrar": {"command": _ps("run_winrar_max.ps1"), "output_extension": ".rar"},
        "windows-zip": {"command": _ps("run_windows_zip.ps1"), "output_extension": ".zip"},
    }


def write_manifest(name: str, description: str, datasets: list[dict]) -> None:
    manifest = {
        "name": name,
        "description": description,
        "goal": {
            "aggregate_minimum_win_percent_vs_7z": 10.0,
            "baseline_tool": "7z-lzma2",
            "shipping_tool": "devzip-native",
            "shipping_backend": "balanced",
        },
        "scoring": {"weissman": {"enabled": True, "baseline": "gzip", "alpha": 1.0}},
        "datasets": datasets,
        "tools": tools_block(),
    }
    out = MANIFESTS / f"{name}.json"
    out.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(f"  manifest -> {out.relative_to(REPO_ROOT)}")


def gen_text() -> None:
    d = BENCH / "text"
    d.mkdir(parents=True, exist_ok=True)
    paras = [
        "The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor jugs.",
        "How vexingly quick daft zebras jump! The five boxing wizards jump quickly.",
        "Sphinx of black quartz, judge my vow. Two driven jocks help fax my big quiz.",
        "Jackdaws love my big sphinx of quartz. Mr Jock, TV quiz PhD, bags few lynx.",
        "Crazy Frederick bought many very exquisite opal jewels.",
        "A mad boxer shot a quick, gloved jab to the jaw of his dizzy opponent.",
    ]
    rng = random.Random(42)
    with open(d / "prose-large.txt", "w", encoding="utf-8") as f:
        for _ in range(60000):
            f.write(rng.choice(paras) + "\n")
    rng = random.Random(99)
    names = ["Alice", "Bob", "Carol", "Dave", "Eve", "Frank", "Grace", "Hank", "Iris", "Jack"]
    depts = ["Engineering", "Sales", "Marketing", "Support", "Finance", "Legal", "HR", "Ops"]
    with open(d / "data-table.csv", "w", encoding="utf-8") as f:
        f.write("id,timestamp,name,email,department,value,score,notes\n")
        for i in range(60000):
            ts = f"2025-{rng.randint(1,12):02d}-{rng.randint(1,28):02d}T{rng.randint(0,23):02d}:{rng.randint(0,59):02d}:{rng.randint(0,59):02d}Z"
            note = f"note {i}" if rng.random() < 0.3 else ""
            f.write(
                f"{i},{ts},{rng.choice(names)},{rng.choice(names).lower()}@example.com,"
                f"{rng.choice(depts)},{rng.uniform(0,10000):.2f},{rng.randint(0,100)},{note}\n"
            )
    rng = random.Random(77)
    records = [
        {
            "id": i,
            "name": f"item-{i:05d}",
            "tags": [f"tag-{rng.randint(0,50)}" for _ in range(rng.randint(1, 5))],
            "value": round(rng.uniform(0, 1000), 3),
            "active": rng.random() > 0.3,
        }
        for i in range(20000)
    ]
    (d / "records.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
    rng = random.Random(33)
    levels = ["INFO", "DEBUG", "WARN", "ERROR", "TRACE"]
    components = ["auth", "api", "db", "cache", "scheduler", "worker", "gateway"]
    with open(d / "application.log", "w", encoding="utf-8") as f:
        for _ in range(40000):
            ts = f"2025-03-{rng.randint(1,28):02d}T{rng.randint(0,23):02d}:{rng.randint(0,59):02d}:{rng.randint(0,59):02d}.{rng.randint(0,999):03d}Z"
            action = rng.choice(["completed", "failed", "started", "retried", "queued"])
            f.write(f"[{ts}] [{rng.choice(levels):5s}] [{rng.choice(components):10s}] Request {action} id={rng.randint(10000,99999)}\n")
    print("  text generated")


def gen_code() -> None:
    d = BENCH / "code"
    if d.exists():
        shutil.rmtree(d)
    d.mkdir(parents=True, exist_ok=True)
    srcs = [
        REPO_ROOT / "native" / "engine" / "src",
        REPO_ROOT / "native" / "engine" / "include",
        REPO_ROOT / "native" / "engine" / "vendor" / "lzma-sdk" / "C",
    ]
    for s in srcs:
        if not s.exists():
            continue
        for root, _dirs, files in os.walk(s):
            for fn in files:
                if fn.lower().endswith((".c", ".h", ".cpp", ".hpp", ".cc", ".cs", ".py", ".txt", ".md")):
                    sp = Path(root) / fn
                    rel = sp.relative_to(s.parent)
                    dp = d / rel
                    dp.parent.mkdir(parents=True, exist_ok=True)
                    try:
                        shutil.copy2(sp, dp)
                    except OSError:
                        pass
    print("  code assembled")


def gen_jpeg() -> list[Path]:
    d = BENCH / "jpeg"
    d.mkdir(parents=True, exist_ok=True)
    web = Path(r"C:\Windows\Web")
    collected: list[Path] = []
    if web.exists():
        for sp in sorted(web.rglob("*.jpg")) + sorted(web.rglob("*.jpeg")):
            try:
                if sp.stat().st_size < 100 * 1024:
                    continue
                name = (sp.parent.name + "_" + sp.name).replace(" ", "_")
                dp = d / name
                if not dp.exists():
                    shutil.copy2(sp, dp)
                collected.append(dp)
            except OSError:
                pass
    print(f"  jpeg collected: {len(collected)} files")
    return collected


def gen_png(jpegs: list[Path]) -> None:
    d = BENCH / "png"
    d.mkdir(parents=True, exist_ok=True)
    try:
        from PIL import Image
    except Exception as exc:  # pragma: no cover
        print(f"  png SKIPPED (Pillow missing: {exc})")
        return
    max_side = 1280
    made = 0
    for sp in jpegs:
        dp = d / (sp.stem + ".png")
        if dp.exists():
            made += 1
            continue
        try:
            with Image.open(sp) as im:
                im = im.convert("RGB")
                w, h = im.size
                scale = min(1.0, max_side / max(w, h))
                if scale < 1.0:
                    im = im.resize((max(1, int(w * scale)), max(1, int(h * scale))), Image.LANCZOS)
                im.save(dp, format="PNG")
                made += 1
        except Exception as exc:
            print(f"    png fail {sp.name}: {exc}")
    print(f"  png generated: {made} files")


def gen_exe() -> None:
    d = BENCH / "exe"
    d.mkdir(parents=True, exist_ok=True)
    sys32 = Path(r"C:\Windows\System32")
    budget = 32 * 1024 * 1024
    used = 0
    count = 0
    if sys32.exists():
        for sp in sorted(sys32.glob("*.dll")):
            try:
                sz = sp.stat().st_size
            except OSError:
                continue
            if sz < 300 * 1024 or sz > 4 * 1024 * 1024:
                continue
            if used + sz > budget:
                continue
            dp = d / sp.name
            try:
                if not dp.exists():
                    shutil.copy2(sp, dp)
                used += sz
                count += 1
            except OSError:
                pass
            if used >= budget:
                break
    print(f"  exe collected: {count} files, {used/1024/1024:.1f} MB")


def summary() -> None:
    print("\nCorpus summary:")
    for name in ["text", "code", "jpeg", "png", "exe"]:
        p = BENCH / name
        if not p.exists():
            continue
        total = sum(f.stat().st_size for f in p.rglob("*") if f.is_file())
        count = sum(1 for f in p.rglob("*") if f.is_file())
        print(f"  {name:5s}: {count:4d} files, {total/1024/1024:8.2f} MB")


def main() -> int:
    print("Building overhaul benchmark corpus (local sources only)...")
    BENCH.mkdir(parents=True, exist_ok=True)
    gen_text()
    gen_code()
    jpegs = gen_jpeg()
    gen_png(jpegs)
    gen_exe()
    summary()

    print("\nWriting manifests...")
    cats = {
        "text": ("Synthetic text/structured data", "sample-data/bench/text"),
        "code": ("Source code (C/C++/headers)", "sample-data/bench/code"),
        "jpeg": ("Real photographic JPEGs", "sample-data/bench/jpeg"),
        "png": ("24-bit PNG photos (real deflate streams)", "sample-data/bench/png"),
        "exe": ("Native x64 PE binaries", "sample-data/bench/exe"),
    }
    for cat, (label, path) in cats.items():
        write_manifest(
            f"ov-{cat}",
            f"Overhaul benchmark: {label}",
            [{"id": cat, "label": label, "path": path, "tags": [cat]}],
        )
    write_manifest(
        "ov-mixed",
        "Overhaul benchmark: mixed corpus across all categories",
        [{"id": cat, "label": label, "path": path, "tags": [cat]} for cat, (label, path) in cats.items()],
    )
    print("\nDone.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
