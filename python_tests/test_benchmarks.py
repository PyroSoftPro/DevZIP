from __future__ import annotations

import importlib.util
import math
import sys
import tempfile
import unittest
from pathlib import Path


def load_module(name: str, relative_path: str):
    path = Path(__file__).resolve().parents[1] / relative_path
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


run_benchmarks = load_module("run_benchmarks", "benchmarks/run_benchmarks.py")


class BenchmarkHarnessTests(unittest.TestCase):
    def test_aggregate_results_preserves_failures(self) -> None:
        results = [
            run_benchmarks.ToolRunResult("7z-lzma2", "codebase", "ok", 1.2, 100, ["7z"]),
            run_benchmarks.ToolRunResult(
                "devzip-native", "codebase", "failed", 0.9, None, ["devzip"], note="boom"
            ),
        ]

        aggregate = run_benchmarks.aggregate_results(results)
        self.assertEqual("failed", aggregate["devzip-native"]["status"])
        self.assertEqual(100, aggregate["7z-lzma2"]["archive_size"])

    def test_aggregate_results_marks_skipped_tools(self) -> None:
        results = [
            run_benchmarks.ToolRunResult("winrar", "codebase", "skipped", None, None, ["wrapper"], note="not installed"),
            run_benchmarks.ToolRunResult("winrar", "media", "skipped", None, None, ["wrapper"], note="not installed"),
        ]

        aggregate = run_benchmarks.aggregate_results(results)
        self.assertEqual("skipped", aggregate["winrar"]["status"])
        self.assertEqual(0, aggregate["winrar"]["archive_size"])

    def test_compute_weissman_score_matches_reference_formula(self) -> None:
        score = run_benchmarks.compute_weissman_score(
            target_archive_size=80,
            target_seconds=2.0,
            baseline_archive_size=100,
            baseline_seconds=4.0,
            alpha=1.0,
        )
        expected = (100 / 80) * (math.log(4.0) / math.log(2.0))
        self.assertAlmostEqual(expected, score)

    def test_apply_weissman_scores_uses_gzip_baseline(self) -> None:
        manifest = {"name": "mixed-large", "scoring": {"weissman": {"alpha": 1.0}}}
        results = [
            run_benchmarks.ToolRunResult("7z-lzma2", "codebase", "ok", 2.0, 100, ["7z"]),
            run_benchmarks.ToolRunResult("devzip-native", "codebase", "ok", 4.0, 80, ["devzip"]),
        ]
        aggregate = run_benchmarks.aggregate_results(results)
        baselines = [
            run_benchmarks.WeissmanBaselineResult("codebase", "ok", 3.0, 120),
        ]

        weissman = run_benchmarks.apply_weissman_scores(manifest, results, aggregate, baselines)

        self.assertEqual("gzip", weissman["baseline_tool"])
        self.assertAlmostEqual(
            run_benchmarks.compute_weissman_score(80, 4.0, 120, 3.0, 1.0),
            results[1].weissman_score,
        )
        self.assertAlmostEqual(
            run_benchmarks.compute_weissman_score(100, 2.0, 120, 3.0, 1.0),
            aggregate["7z-lzma2"]["weissman_score"],
        )

    def test_render_markdown_includes_delta_and_weissman(self) -> None:
        manifest = {"name": "mixed-large", "goal": {"baseline_tool": "7z-lzma2"}}
        aggregate = {
            "7z-lzma2": {
                "status": "ok",
                "archive_size": 100_000_000,
                "archive_size_mb": 100.125,
                "seconds": 1.0,
                "weissman_score": 1.0,
                "datasets": [],
            },
            "devzip-native": {
                "status": "ok",
                "archive_size": 80_000_000,
                "archive_size_mb": 80.250,
                "seconds": 2.0,
                "weissman_score": 0.5,
                "datasets": [],
            },
        }
        weissman = {
            "baseline_tool": "gzip",
            "alpha": 1.0,
            "baseline": {
                "status": "ok",
                "archive_size": 90_000_000,
                "archive_size_mb": 90.375,
                "seconds": 0.5,
                "datasets": [],
            },
        }

        markdown = run_benchmarks.render_markdown(manifest, aggregate, weissman)
        self.assertIn("| Tool | Status | End Size (MB) | Total Time (s) | Delta vs 7z-lzma2 | Weissman Score |", markdown)
        self.assertIn("-20.00%", markdown)
        self.assertIn("| devzip-native | ok | 80.250 | 2.00 | -20.00% | 0.5000 |", markdown)
        self.assertIn("Baseline: `gzip` with `alpha=1.0`", markdown)
        self.assertIn("Aggregate baseline: status=ok, size=90.375 MB, time=0.50", markdown)

    def test_run_tool_skips_missing_input(self) -> None:
        with tempfile.TemporaryDirectory() as temp_directory:
            results_dir = Path(temp_directory)
            tool = run_benchmarks.ToolSpec("devzip", ["python", "missing-script.py"], ".dvz")
            dataset = {"id": "missing", "path": "does-not-exist"}

            result = run_benchmarks.run_tool(tool, dataset, results_dir)
            self.assertEqual("skipped", result.status)
            self.assertIn("Input path not found", result.note)

    def test_run_tool_skips_missing_executable(self) -> None:
        with tempfile.TemporaryDirectory() as temp_directory:
            project_root = Path(__file__).resolve().parents[1]
            input_path = project_root / "python_tests" / "fixture.txt"
            input_path.write_text("fixture", encoding="utf-8")
            self.addCleanup(lambda: input_path.unlink(missing_ok=True))

            results_dir = Path(temp_directory)
            tool = run_benchmarks.ToolSpec("missing-tool", ["definitely-not-a-real-command"], ".out")
            dataset = {"id": "fixture", "path": str(input_path.relative_to(project_root))}

            result = run_benchmarks.run_tool(tool, dataset, results_dir)
            self.assertEqual("skipped", result.status)
            self.assertIn("Tool executable not found", result.note)

    def test_run_tool_reports_failed_subprocess(self) -> None:
        with tempfile.TemporaryDirectory() as temp_directory:
            project_root = Path(__file__).resolve().parents[1]
            input_path = project_root / "python_tests" / "fixture.txt"
            input_path.write_text("fixture", encoding="utf-8")
            self.addCleanup(lambda: input_path.unlink(missing_ok=True))

            script_path = Path(temp_directory) / "fail.py"
            script_path.write_text("import sys; sys.stderr.write('boom'); raise SystemExit(3)", encoding="utf-8")

            results_dir = Path(temp_directory) / "results"
            tool = run_benchmarks.ToolSpec("failing-tool", [sys.executable, str(script_path)], ".out")
            dataset = {"id": "fixture", "path": str(input_path.relative_to(project_root))}

            result = run_benchmarks.run_tool(tool, dataset, results_dir)
            self.assertEqual("failed", result.status)
            self.assertIn("boom", result.note)

    def test_run_tool_treats_wrapper_skip_as_skipped(self) -> None:
        with tempfile.TemporaryDirectory() as temp_directory:
            project_root = Path(__file__).resolve().parents[1]
            input_path = project_root / "python_tests" / "fixture.txt"
            input_path.write_text("fixture", encoding="utf-8")
            self.addCleanup(lambda: input_path.unlink(missing_ok=True))

            script_path = Path(temp_directory) / "skip.py"
            script_path.write_text(
                "import sys; print('BENCHMARK_SKIP: tool not installed'); raise SystemExit(85)",
                encoding="utf-8",
            )

            results_dir = Path(temp_directory) / "results"
            tool = run_benchmarks.ToolSpec("winrar", [sys.executable, str(script_path)], ".rar")
            dataset = {"id": "fixture", "path": str(input_path.relative_to(project_root))}

            result = run_benchmarks.run_tool(tool, dataset, results_dir)
            self.assertEqual("skipped", result.status)
            self.assertIn("tool not installed", result.note)


if __name__ == "__main__":
    unittest.main()
