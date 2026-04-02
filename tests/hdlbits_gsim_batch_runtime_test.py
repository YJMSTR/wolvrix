from __future__ import annotations

import json
import os
import pathlib
import shutil
import subprocess
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
ARTIFACT_ROOT = REPO_ROOT / "build" / "artifacts" / "hdlbits_gsim_batch_runtime_test"
SCRIPT_PATH = REPO_ROOT / "scripts" / "wolvrix_hdlbits_gsim_batch.py"
BUILD_PYTHON_DIR = pathlib.Path(
    os.environ.get("WOLVRIX_PYTHON_BUILD_DIR", str(REPO_ROOT / "wolvrix" / "build" / "python"))
)


def fail(message: str) -> int:
    print(f"[hdlbits-gsim-batch-runtime] {message}", file=sys.stderr)
    return 1


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def reset_dir(path: pathlib.Path) -> None:
    shutil.rmtree(path, ignore_errors=True)
    path.mkdir(parents=True, exist_ok=True)


def main() -> int:
    try:
        reset_dir(ARTIFACT_ROOT)
        env = os.environ.copy()
        env["PYTHONNOUSERSITE"] = "1"
        env["WOLVRIX_PYTHON_BUILD_DIR"] = str(BUILD_PYTHON_DIR)
        env["PYTHONPATH"] = str(BUILD_PYTHON_DIR)

        result = subprocess.run(
            [
                sys.executable,
                "-S",
                str(SCRIPT_PATH),
                "--output-dir",
                str(ARTIFACT_ROOT),
                "--dut-ids",
                "001,006",
                "--execution-mode",
                "runtime",
                "--timeout",
                "60",
                "--memory-limit-mb",
                "4096",
            ],
            capture_output=True,
            text=True,
            env=env,
            check=False,
        )
        expect(
            result.returncode == 0,
            "batch runtime harness failed\nstdout:\n"
            + (result.stdout or "")
            + "\nstderr:\n"
            + (result.stderr or ""),
        )

        report_path = ARTIFACT_ROOT / "batch_report.json"
        expect(report_path.exists(), "missing batch runtime report")
        report = json.loads(report_path.read_text(encoding="utf-8"))
        summary = report["summary"]
        expect(summary["total_duts"] == 2, f"expected 2 DUTs, got {summary['total_duts']}")
        expect(summary["success"] == 2, f"expected 2 successes, got {summary['success']}")
        expect(summary["tool_failure"] == 0, f"expected no tool failures, got {summary['tool_failure']}")
        results = {entry["dut_id"]: entry["result"] for entry in report["dut_results"]}
        expect(results.get("001") == "success", f"unexpected dut_001 result: {results.get('001')}")
        expect(results.get("006") == "success", f"unexpected dut_006 result: {results.get('006')}")
    except Exception as ex:
        return fail(str(ex))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
