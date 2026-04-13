"""Run benchmarks for every per-type manifest and write a combined summary."""
from __future__ import annotations

import json
import subprocess
import sys
import time
from pathlib import Path

MANIFEST_DIR = Path(__file__).resolve().parent / "manifests"
RUNNER = Path(__file__).resolve().parent / "run_benchmarks.py"

MANIFESTS = [
    "text.json",
    "images-jpeg.json",
    "images-png.json",
    "images-raw.json",
    "software.json",
    "video.json",
    "random.json",
]


def run_manifest(name: str) -> dict | None:
    manifest_path = MANIFEST_DIR / name
    results_path = manifest_path.with_suffix(".results.json")
    print(f"\n{'='*60}")
    print(f"Running: {name}")
    print(f"{'='*60}")
    started = time.perf_counter()
    result = subprocess.run(
        [
            sys.executable,
            str(RUNNER),
            "--manifest", str(manifest_path),
            "--output-json", str(results_path),
        ],
        capture_output=False,
        text=True,
    )
    elapsed = time.perf_counter() - started
    print(f"  Finished in {elapsed:.1f}s (exit {result.returncode})")
    if results_path.exists():
        return json.loads(results_path.read_text(encoding="utf-8"))
    return None


def main() -> int:
    all_results = {}
    for name in MANIFESTS:
        data = run_manifest(name)
        if data:
            all_results[data["manifest"]] = data

    summary_path = MANIFEST_DIR / "all-types-summary.json"
    summary_path.write_text(json.dumps(all_results, indent=2), encoding="utf-8")
    print(f"\nWrote combined summary to {summary_path}")

    print("\n" + "=" * 70)
    print("RESULTS SUMMARY")
    print("=" * 70)
    for category, data in all_results.items():
        agg = data.get("aggregate", {})
        print(f"\n  [{category}]")
        for tool_name in sorted(agg.keys()):
            info = agg[tool_name]
            size_mb = info.get("archive_size_mb", 0)
            status = info.get("status", "?")
            print(f"    {tool_name:20s}  {size_mb:>10.3f} MB  ({status})")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
