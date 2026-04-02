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
                "001,004,005,011,014,021,025,029,030,031,033,038",
                "--execution-mode",
                "runtime",
                "--timeout",
                "60",
                "--memory-limit-mb",
                "32768",
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
        expect(summary["total_duts"] == 12, f"expected 12 DUTs, got {summary['total_duts']}")
        expect(summary["success"] == 12, f"expected 12 successes, got {summary['success']}")
        expect(summary["tool_failure"] == 0, f"expected no tool failures, got {summary['tool_failure']}")
        results = {entry["dut_id"]: entry["result"] for entry in report["dut_results"]}
        expect(results.get("001") == "success", f"unexpected dut_001 result: {results.get('001')}")
        expect(results.get("004") == "success", f"unexpected dut_004 result: {results.get('004')}")
        expect(results.get("005") == "success", f"unexpected dut_005 result: {results.get('005')}")
        expect(results.get("011") == "success", f"unexpected dut_011 result: {results.get('011')}")
        expect(results.get("014") == "success", f"unexpected dut_014 result: {results.get('014')}")
        expect(results.get("021") == "success", f"unexpected dut_021 result: {results.get('021')}")
        expect(results.get("025") == "success", f"unexpected dut_025 result: {results.get('025')}")
        expect(results.get("029") == "success", f"unexpected dut_029 result: {results.get('029')}")
        expect(results.get("030") == "success", f"unexpected dut_030 result: {results.get('030')}")
        expect(results.get("031") == "success", f"unexpected dut_031 result: {results.get('031')}")
        expect(results.get("033") == "success", f"unexpected dut_033 result: {results.get('033')}")
        expect(results.get("038") == "success", f"unexpected dut_038 result: {results.get('038')}")

        fail_env = env.copy()
        fail_env["WOLVRIX_GSIM_RUNTIME_FORCE_FAIL"] = "1"
        fail_out = ARTIFACT_ROOT / "forced_fail"
        fail_out.mkdir(parents=True, exist_ok=True)
        fail_result = subprocess.run(
            [
                sys.executable,
                "-S",
                str(SCRIPT_PATH),
                "--output-dir",
                str(fail_out),
                "--dut-ids",
                "001",
                "--execution-mode",
                "runtime",
                "--timeout",
                "60",
                "--memory-limit-mb",
                "32768",
            ],
            capture_output=True,
            text=True,
            env=fail_env,
            check=False,
        )
        expect(fail_result.returncode != 0, "forced runtime failure should fail the batch harness")
        fail_report = json.loads((fail_out / "batch_report.json").read_text(encoding="utf-8"))
        fail_entry = fail_report["dut_results"][0]
        expect(fail_entry["result"] == "sim-failure", f"expected sim-failure, got {fail_entry['result']}")

        mem_out = ARTIFACT_ROOT / "tiny_mem"
        mem_out.mkdir(parents=True, exist_ok=True)
        mem_result = subprocess.run(
            [
                sys.executable,
                "-S",
                str(SCRIPT_PATH),
                "--output-dir",
                str(mem_out),
                "--dut-ids",
                "001",
                "--execution-mode",
                "runtime",
                "--timeout",
                "60",
                "--memory-limit-mb",
                "1",
            ],
            capture_output=True,
            text=True,
            env=env,
            check=False,
        )
        expect(mem_result.returncode != 0, "tiny-memory runtime batch should not succeed")
        mem_report = json.loads((mem_out / "batch_report.json").read_text(encoding="utf-8"))
        mem_entry = mem_report["dut_results"][0]
        expect(mem_entry["result"] == "tool-failure", f"expected tool-failure under tiny memory, got {mem_entry['result']}")
    except Exception as ex:
        return fail(str(ex))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
