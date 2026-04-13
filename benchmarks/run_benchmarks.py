from __future__ import annotations

import argparse
import gzip
import json
import math
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

DEFAULT_TIMEOUT_SECONDS = 600
MANIFEST_TIMEOUT_KEY = "timeout_seconds"
SKIP_SENTINEL = "BENCHMARK_SKIP:"
BYTES_PER_MB = 1_000_000


@dataclass
class ToolSpec:
    name: str
    command: list[str]
    output_extension: str


@dataclass
class ToolRunResult:
    tool: str
    dataset: str
    status: str
    seconds: float | None
    archive_size: int | None
    command: list[str]
    weissman_score: float | None = None
    note: str | None = None


@dataclass
class WeissmanBaselineResult:
    dataset: str
    status: str
    seconds: float | None
    archive_size: int | None
    baseline_tool: str = "gzip"
    note: str | None = None


def bytes_to_mb(value: int | None) -> float | None:
    if value is None:
        return None
    return value / BYTES_PER_MB


def load_manifest(path: Path) -> dict[str, Any]:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise SystemExit(f"Malformed manifest {path}: {exc}") from exc


def parse_tool_override(value: str) -> tuple[str, list[str]]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("Tool overrides must look like name=command")
    name, command = value.split("=", 1)
    command = command.strip()
    if not name or not command:
        raise argparse.ArgumentTypeError("Tool overrides must include both a name and a command")
    return name.strip(), shlex.split(command, posix=False)


def command_is_available(command: list[str]) -> bool:
    if not command:
        return False
    executable = command[0]
    if Path(executable).exists():
        return True
    return shutil.which(executable) is not None


def resolve_tools(manifest: dict[str, Any], overrides: dict[str, list[str]]) -> dict[str, ToolSpec]:
    resolved: dict[str, ToolSpec] = {}
    for name, spec in manifest.get("tools", {}).items():
        command = overrides.get(name) or list(spec.get("command", []))
        resolved[name] = ToolSpec(
            name=name,
            command=command,
            output_extension=spec.get("output_extension", f".{name}"),
        )
    return resolved


def skip_note_from_output(stdout: str, stderr: str) -> str | None:
    for stream in (stderr, stdout):
        for line in stream.splitlines():
            if line.startswith(SKIP_SENTINEL):
                note = line.removeprefix(SKIP_SENTINEL).strip()
                return note or "Skipped by benchmark wrapper"
    return None


def project_root() -> Path:
    return Path(__file__).resolve().parents[1]


def dataset_archive_name(dataset_id: str, tool: ToolSpec) -> str:
    return f"{dataset_id}-{tool.name}{tool.output_extension}"


def weissman_settings(manifest: dict[str, Any]) -> dict[str, Any]:
    settings = manifest.get("scoring", {}).get("weissman", {})
    return {
        "enabled": settings.get("enabled", True),
        "baseline": settings.get("baseline", "gzip"),
        "alpha": float(settings.get("alpha", 1.0)),
    }


def expand_command(command: list[str], input_path: Path, output_path: Path) -> list[str]:
    return [
        part
        .replace("{input}", str(input_path))
        .replace("{output}", str(output_path))
        .replace("{project_root}", str(project_root()))
        for part in command
    ]


def prepare_weissman_input(input_path: Path, dataset_id: str, temp_dir: Path) -> tuple[Path, bool]:
    if input_path.is_file():
        return input_path, False

    snapshot_path = temp_dir / f"{dataset_id}.tar"
    root_name = input_path.name or dataset_id
    paths = [input_path, *sorted(input_path.rglob("*"), key=lambda path: path.relative_to(input_path).as_posix())]

    with tarfile.open(snapshot_path, mode="w", format=tarfile.PAX_FORMAT) as archive:
        for path in paths:
            relative = "." if path == input_path else path.relative_to(input_path).as_posix()
            arcname = root_name if relative == "." else f"{root_name}/{relative}"
            stat = path.lstat()
            info = tarfile.TarInfo(arcname)
            info.mtime = 0
            info.uid = 0
            info.gid = 0
            info.uname = ""
            info.gname = ""

            if path.is_dir():
                info.type = tarfile.DIRTYPE
                info.mode = 0o755
                info.size = 0
                archive.addfile(info)
            elif path.is_file():
                info.mode = 0o644
                info.size = stat.st_size
                with path.open("rb") as source:
                    archive.addfile(info, source)

    return snapshot_path, True


def run_weissman_baseline(dataset: dict[str, Any], temp_dir: Path) -> WeissmanBaselineResult:
    dataset_id = dataset["id"]
    input_path = project_root() / dataset["path"]

    if not input_path.exists():
        return WeissmanBaselineResult(
            dataset=dataset_id,
            status="skipped",
            seconds=None,
            archive_size=None,
            note=f"Input path not found: {input_path}",
        )

    prepared_input, should_cleanup = prepare_weissman_input(input_path, dataset_id, temp_dir)
    output_path = temp_dir / f"{dataset_id}.gz"

    try:
        started = time.perf_counter()
        with prepared_input.open("rb") as source, output_path.open("wb") as compressed_stream:
            with gzip.GzipFile(
                filename="",
                mode="wb",
                compresslevel=9,
                fileobj=compressed_stream,
                mtime=0,
            ) as gzip_stream:
                shutil.copyfileobj(source, gzip_stream)
        elapsed = time.perf_counter() - started
        archive_size = output_path.stat().st_size
    except OSError as exc:
        return WeissmanBaselineResult(
            dataset=dataset_id,
            status="failed",
            seconds=None,
            archive_size=None,
            note=str(exc),
        )
    finally:
        output_path.unlink(missing_ok=True)
        if should_cleanup:
            prepared_input.unlink(missing_ok=True)

    return WeissmanBaselineResult(
        dataset=dataset_id,
        status="ok",
        seconds=elapsed,
        archive_size=archive_size,
    )


def compute_weissman_score(
    target_archive_size: int | None,
    target_seconds: float | None,
    baseline_archive_size: int | None,
    baseline_seconds: float | None,
    alpha: float,
) -> float | None:
    if (
        target_archive_size is None
        or target_seconds is None
        or baseline_archive_size is None
        or baseline_seconds is None
        or target_archive_size <= 0
        or target_seconds <= 0
        or baseline_archive_size <= 0
        or baseline_seconds <= 0
    ):
        return None

    target_log = math.log(target_seconds)
    baseline_log = math.log(baseline_seconds)
    if math.isclose(target_log, 0.0, abs_tol=1e-12):
        return None

    # Follows the reference repo's formula with gzip as the universal baseline:
    # W = alpha * (r / r_b) * (log(T_b) / log(T)).
    # Using the same input for both algorithms lets the ratio term collapse to
    # baseline_archive_size / target_archive_size.
    return alpha * (baseline_archive_size / target_archive_size) * (baseline_log / target_log)


def run_tool(tool: ToolSpec, dataset: dict[str, Any], results_dir: Path,
             timeout: int = DEFAULT_TIMEOUT_SECONDS) -> ToolRunResult:
    dataset_id = dataset["id"]
    input_path = project_root() / dataset["path"]
    output_path = results_dir / dataset_archive_name(dataset_id, tool)

    if not input_path.exists():
        return ToolRunResult(
            tool=tool.name,
            dataset=dataset_id,
            status="skipped",
            seconds=None,
            archive_size=None,
            command=tool.command,
            note=f"Input path not found: {input_path}",
        )

    if not command_is_available(tool.command):
        return ToolRunResult(
            tool=tool.name,
            dataset=dataset_id,
            status="skipped",
            seconds=None,
            archive_size=None,
            command=tool.command,
            note="Tool executable not found on PATH",
        )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    if output_path.exists():
        output_path.unlink()

    command = expand_command(tool.command, input_path, output_path)
    started = time.perf_counter()

    try:
        # CREATE_NEW_PROCESS_GROUP lets us kill the entire tree (incl. 7z
        # grandchildren) on Windows when the timeout fires.
        kwargs: dict = {}
        if sys.platform == "win32":
            kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
        proc = subprocess.Popen(
            command,
            cwd=project_root(),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            **kwargs,
        )
        try:
            stdout, stderr = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            if sys.platform == "win32":
                subprocess.run(
                    ["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                    capture_output=True,
                )
            else:
                proc.kill()
            proc.communicate()
            return ToolRunResult(
                tool=tool.name,
                dataset=dataset_id,
                status="failed",
                seconds=None,
                archive_size=None,
                command=command,
                note=f"Command exceeded timeout of {timeout} seconds",
            )
        completed = subprocess.CompletedProcess(
            command, proc.returncode, stdout, stderr
        )
    except OSError as exc:
        return ToolRunResult(
            tool=tool.name,
            dataset=dataset_id,
            status="failed",
            seconds=None,
            archive_size=None,
            command=command,
            note=str(exc),
        )
    except OSError as exc:
        return ToolRunResult(
            tool=tool.name,
            dataset=dataset_id,
            status="failed",
            seconds=None,
            archive_size=None,
            command=command,
            note=str(exc),
        )

    elapsed = time.perf_counter() - started

    if completed.returncode != 0:
        skip_note = skip_note_from_output(completed.stdout, completed.stderr)
        if skip_note is not None:
            return ToolRunResult(
                tool=tool.name,
                dataset=dataset_id,
                status="skipped",
                seconds=None,
                archive_size=None,
                command=command,
                note=skip_note,
            )
        note = completed.stderr.strip() or completed.stdout.strip() or f"Exit code {completed.returncode}"
        return ToolRunResult(
            tool=tool.name,
            dataset=dataset_id,
            status="failed",
            seconds=elapsed,
            archive_size=None,
            command=command,
            note=note,
        )

    if not output_path.exists():
        return ToolRunResult(
            tool=tool.name,
            dataset=dataset_id,
            status="failed",
            seconds=elapsed,
            archive_size=None,
            command=command,
            note=f"Expected archive missing: {output_path}",
        )

    return ToolRunResult(
        tool=tool.name,
        dataset=dataset_id,
        status="ok",
        seconds=elapsed,
        archive_size=output_path.stat().st_size,
        command=command,
    )


def aggregate_results(results: list[ToolRunResult]) -> dict[str, Any]:
    by_tool: dict[str, dict[str, Any]] = {}
    for result in results:
        tool_summary = by_tool.setdefault(
            result.tool,
            {
                "tool": result.tool,
                "status": "ok",
                "seconds": 0.0,
                "archive_size": 0,
                "archive_size_mb": 0.0,
                "weissman_score": None,
                "datasets": [],
                "_ok_count": 0,
                "_skipped_count": 0,
                "_failed_count": 0,
            },
        )
        tool_summary["datasets"].append(result.__dict__)
        if result.status == "ok":
            tool_summary["_ok_count"] += 1
            tool_summary["seconds"] += result.seconds or 0.0
            tool_summary["archive_size"] += result.archive_size or 0
        elif result.status == "skipped":
            tool_summary["_skipped_count"] += 1
        else:
            tool_summary["_failed_count"] += 1

        if tool_summary["_failed_count"] > 0:
            tool_summary["status"] = "failed" if tool_summary["_ok_count"] == 0 else "partial"
        elif tool_summary["_ok_count"] > 0 and tool_summary["_skipped_count"] > 0:
            tool_summary["status"] = "partial"
        elif tool_summary["_ok_count"] > 0:
            tool_summary["status"] = "ok"
        elif tool_summary["_skipped_count"] > 0:
            tool_summary["status"] = "skipped"

    for summary in by_tool.values():
        del summary["_ok_count"]
        del summary["_skipped_count"]
        del summary["_failed_count"]
        summary["archive_size_mb"] = bytes_to_mb(summary["archive_size"])
    return by_tool


def aggregate_weissman_baseline(baselines: list[WeissmanBaselineResult]) -> dict[str, Any]:
    ok_datasets = [baseline for baseline in baselines if baseline.status == "ok"]
    failed_count = sum(1 for baseline in baselines if baseline.status == "failed")
    skipped_count = sum(1 for baseline in baselines if baseline.status == "skipped")

    if failed_count > 0:
        status = "failed" if not ok_datasets else "partial"
    elif skipped_count > 0:
        status = "partial" if ok_datasets else "skipped"
    else:
        status = "ok"

    return {
        "tool": "gzip",
        "status": status,
        "seconds": sum(baseline.seconds or 0.0 for baseline in ok_datasets),
        "archive_size": sum(baseline.archive_size or 0 for baseline in ok_datasets),
        "archive_size_mb": bytes_to_mb(sum(baseline.archive_size or 0 for baseline in ok_datasets)),
        "datasets": [baseline.__dict__ for baseline in baselines],
    }


def apply_weissman_scores(
    manifest: dict[str, Any],
    results: list[ToolRunResult],
    aggregate: dict[str, Any],
    baselines: list[WeissmanBaselineResult],
) -> dict[str, Any]:
    settings = weissman_settings(manifest)
    baseline_summary = aggregate_weissman_baseline(baselines)
    if not settings["enabled"]:
        return {"enabled": False, "baseline_tool": settings["baseline"], "alpha": settings["alpha"], "baseline": baseline_summary}

    baseline_by_dataset = {baseline.dataset: baseline for baseline in baselines if baseline.status == "ok"}
    for result in results:
        baseline = baseline_by_dataset.get(result.dataset)
        if result.status != "ok" or baseline is None:
            result.weissman_score = None
            continue
        result.weissman_score = compute_weissman_score(
            result.archive_size,
            result.seconds,
            baseline.archive_size,
            baseline.seconds,
            settings["alpha"],
        )

    if baseline_summary["status"] == "ok":
        for summary in aggregate.values():
            if summary["status"] != "ok":
                summary["weissman_score"] = None
                continue
            summary["weissman_score"] = compute_weissman_score(
                summary["archive_size"],
                summary["seconds"],
                baseline_summary["archive_size"],
                baseline_summary["seconds"],
                settings["alpha"],
            )
    else:
        for summary in aggregate.values():
            summary["weissman_score"] = None

    return {
        "enabled": True,
        "baseline_tool": settings["baseline"],
        "alpha": settings["alpha"],
        "baseline": baseline_summary,
    }


def render_markdown(manifest: dict[str, Any], aggregate: dict[str, Any], weissman: dict[str, Any]) -> str:
    baseline_tool = manifest.get("goal", {}).get("baseline_tool", "7z")
    lines = [
        "# Benchmark Results",
        "",
        f"Corpus: `{manifest['name']}`",
        "",
        f"| Tool | Status | End Size (MB) | Total Time (s) | Delta vs {baseline_tool} | Weissman Score |",
        "|------|--------|---------------|----------------|---------------------------|----------------|",
    ]

    baseline_size = aggregate.get(baseline_tool, {}).get("archive_size")
    for tool_name, summary in sorted(aggregate.items()):
        delta = "n/a"
        if baseline_size and summary["archive_size"]:
            ratio = (summary["archive_size"] - baseline_size) / baseline_size * 100.0
            delta = f"{ratio:+.2f}%"
        size_display = f"{summary['archive_size_mb']:.3f}" if summary["archive_size"] else "n/a"
        time_display = f"{summary['seconds']:.2f}" if summary["seconds"] else "n/a"
        weissman_display = (
            f"{summary['weissman_score']:.4f}" if summary.get("weissman_score") is not None else "n/a"
        )
        lines.append(
            f"| {tool_name} | {summary['status']} | {size_display} | {time_display} | {delta} | {weissman_display} |"
        )

    lines.extend(["", "## Weissman Scoring", ""])
    lines.append(
        f"Baseline: `{weissman['baseline_tool']}` with `alpha={weissman['alpha']}`"
    )
    baseline_summary = weissman["baseline"]
    baseline_size_display = (
        f"{baseline_summary['archive_size_mb']:.3f} MB"
        if baseline_summary["archive_size"]
        else "n/a"
    )
    baseline_time_display = f"{baseline_summary['seconds']:.2f}" if baseline_summary["seconds"] else "n/a"
    lines.append(
        f"Aggregate baseline: status={baseline_summary['status']}, size={baseline_size_display}, time={baseline_time_display}"
    )
    lines.extend(["", "### Gzip Baseline By Dataset"])
    for dataset in baseline_summary["datasets"]:
        note = dataset.get("note") or ""
        size_bytes = dataset.get("archive_size")
        size_display = f"{bytes_to_mb(size_bytes):.3f} MB ({size_bytes} bytes)" if size_bytes else "n/a"
        lines.append(
            f"- `{dataset['dataset']}`: {dataset['status']}, size={size_display}, "
            f"time={dataset.get('seconds')}, {note}".rstrip(", ")
        )

    lines.extend(["", "## Dataset Notes", ""])
    for tool_name, summary in sorted(aggregate.items()):
        lines.append(f"### {tool_name}")
        for dataset in summary["datasets"]:
            note = dataset.get("note") or ""
            weissman_note = (
                f"weissman={dataset['weissman_score']:.4f}"
                if dataset.get("weissman_score") is not None
                else "weissman=n/a"
            )
            size_bytes = dataset.get("archive_size")
            size_display = f"{bytes_to_mb(size_bytes):.3f} MB ({size_bytes} bytes)" if size_bytes else "n/a"
            lines.append(
                f"- `{dataset['dataset']}`: {dataset['status']}, size={size_display}, "
                f"time={dataset.get('seconds')}, {weissman_note}, {note}".rstrip(", ")
            )
        lines.append("")

    return "\n".join(lines).strip() + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description="Run DevZip corpus benchmarks")
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--output-json", type=Path, default=None)
    parser.add_argument("--markdown-out", type=Path, default=None)
    parser.add_argument("--tool", action="append", default=[], type=parse_tool_override)
    args = parser.parse_args()

    manifest = load_manifest(args.manifest)
    overrides = dict(args.tool)
    tools = resolve_tools(manifest, overrides)

    timeout = int(manifest.get(MANIFEST_TIMEOUT_KEY, DEFAULT_TIMEOUT_SECONDS))
    if timeout != DEFAULT_TIMEOUT_SECONDS:
        print(f"Using manifest timeout: {timeout}s (default is {DEFAULT_TIMEOUT_SECONDS}s)")

    output_json = args.output_json or args.manifest.with_suffix(".results.json")
    results_dir = project_root() / "BenchmarkResults" / manifest["name"]
    results_dir.mkdir(parents=True, exist_ok=True)

    results: list[ToolRunResult] = []
    weissman_baselines: list[WeissmanBaselineResult] = []
    with tempfile.TemporaryDirectory(prefix="devzip-weissman-") as temp_directory:
        temp_dir = Path(temp_directory)
        for dataset in manifest.get("datasets", []):
            print(f"[dataset] {dataset['id']}")
            weissman_baselines.append(run_weissman_baseline(dataset, temp_dir))
            for tool in tools.values():
                print(f"  -> {tool.name}")
                results.append(run_tool(tool, dataset, results_dir, timeout=timeout))

    aggregate = aggregate_results(results)
    weissman = apply_weissman_scores(manifest, results, aggregate, weissman_baselines)
    payload = {
        "manifest": manifest["name"],
        "description": manifest.get("description"),
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "results": [
            {
                **result.__dict__,
                "archive_size_mb": bytes_to_mb(result.archive_size),
            }
            for result in results
        ],
        "aggregate": aggregate,
        "weissman": weissman,
    }

    output_json.parent.mkdir(parents=True, exist_ok=True)
    output_json.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(f"Wrote JSON summary to {output_json}")

    if args.markdown_out:
        args.markdown_out.parent.mkdir(parents=True, exist_ok=True)
        args.markdown_out.write_text(render_markdown(manifest, aggregate, weissman), encoding="utf-8")
        print(f"Wrote markdown summary to {args.markdown_out}")

    any_failed = any(result.status == "failed" for result in results)
    return 1 if any_failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
