from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate DevZip benchmark thresholds")
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--results", required=True, type=Path)
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    results = json.loads(args.results.read_text(encoding="utf-8"))

    goal = float(manifest["goal"]["aggregate_minimum_win_percent_vs_7z"])
    baseline_tool = manifest["goal"].get("baseline_tool", "7z")
    shipping_tool = manifest["goal"].get("shipping_tool", "devzip")
    aggregate = results["aggregate"]
    baseline = aggregate.get(baseline_tool, {})
    shipping = aggregate.get(shipping_tool, {})

    if not baseline or not shipping:
        raise SystemExit(
            f"Both {baseline_tool} and {shipping_tool} aggregate results are required."
        )

    baseline_size = baseline.get("archive_size") or 0
    shipping_size = shipping.get("archive_size") or 0
    if baseline_size <= 0 or shipping_size <= 0:
        raise SystemExit("Benchmark sizes must be positive.")

    improvement = (baseline_size - shipping_size) / baseline_size * 100.0
    print(f"{shipping_tool} improvement vs {baseline_tool}: {improvement:.2f}%")
    if improvement < goal:
        raise SystemExit(
            f"Regression gate failed: expected at least {goal:.2f}% improvement vs {baseline_tool}."
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
