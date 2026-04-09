from __future__ import annotations

import json
import os
import pathlib
import shutil
import subprocess
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
ARTIFACT_ROOT = REPO_ROOT / "build" / "artifacts" / "hdlbits_phase_bench_test"
SCRIPT_PATH = REPO_ROOT / "scripts" / "wolvrix_hdlbits_phase_bench.py"
BUILD_PYTHON_DIR = pathlib.Path(
    os.environ.get("WOLVRIX_PYTHON_BUILD_DIR", str(REPO_ROOT / "wolvrix" / "build" / "python"))
)


def fail(message: str) -> int:
    print(f"[hdlbits-phase-bench] {message}", file=sys.stderr)
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
                "--backend",
                "both",
                "--dut-ids",
                "001",
                "--timeout",
                "120",
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
            "phase benchmark failed\nstdout:\n"
            + (result.stdout or "")
            + "\nstderr:\n"
            + (result.stderr or ""),
        )

        report_path = ARTIFACT_ROOT / "batch_report.json"
        expect(report_path.exists(), "missing phase benchmark report")
        report = json.loads(report_path.read_text(encoding="utf-8"))

        backend_summaries = report["backend_summaries"]
        expect("gsim" in backend_summaries, "missing gsim summary")
        expect("verilator" in backend_summaries, "missing verilator summary")

        rows = {(entry["dut_id"], entry["backend"]): entry for entry in report["dut_results"]}
        gsim = rows.get(("001", "gsim"))
        verilator = rows.get(("001", "verilator"))
        expect(gsim is not None, "missing gsim dut_001 result")
        expect(verilator is not None, "missing verilator dut_001 result")

        for name, row in (("gsim", gsim), ("verilator", verilator)):
            expect(row["result"] == "success", f"{name} dut_001 did not succeed: {row['result']}")
            expect(row["emit_ms"] >= 0.0, f"{name} missing emit_ms")
            expect(row["compile_ms"] >= 0.0, f"{name} missing compile_ms")
            expect(row["runtime_ms"] >= 0.0, f"{name} missing runtime_ms")
            expect(row["total_ms"] >= row["emit_ms"], f"{name} total_ms smaller than emit_ms")
    except Exception as ex:
        return fail(str(ex))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
