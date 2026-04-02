from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
ARTIFACT_ROOT = REPO_ROOT / "build" / "artifacts" / "hdlbits_gsim_runner_patterns"
SCRIPT_PATH = REPO_ROOT / "scripts" / "wolvrix_hdlbits_gsim_run.py"
BUILD_PYTHON_DIR = pathlib.Path(
    os.environ.get("WOLVRIX_PYTHON_BUILD_DIR", str(REPO_ROOT / "wolvrix" / "build" / "python"))
)


def fail(message: str) -> int:
    print(f"[hdlbits-gsim-runner-patterns] {message}", file=sys.stderr)
    return 1


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def reset_dir(path: pathlib.Path) -> None:
    shutil.rmtree(path, ignore_errors=True)
    path.mkdir(parents=True, exist_ok=True)


def run_dut(dut_id: str) -> None:
    env = os.environ.copy()
    env["PYTHONNOUSERSITE"] = "1"
    env["WOLVRIX_PYTHON_BUILD_DIR"] = str(BUILD_PYTHON_DIR)
    env["PYTHONPATH"] = str(BUILD_PYTHON_DIR)
    dut_dir = ARTIFACT_ROOT / dut_id
    dut_dir.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [
            sys.executable,
            "-S",
            str(SCRIPT_PATH),
            "--dut",
            dut_id,
            "--out-dir",
            str(dut_dir),
        ],
        capture_output=True,
        text=True,
        env=env,
        check=False,
    )
    expect(
        result.returncode == 0,
        f"dut_{dut_id} runtime failed\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}",
    )
    expect((dut_dir / f"dut_{dut_id}_runner").exists(), f"dut_{dut_id} missing runtime binary")


def main() -> int:
    try:
        reset_dir(ARTIFACT_ROOT)
        expect(SCRIPT_PATH.exists(), f"missing runtime helper: {SCRIPT_PATH}")
        for dut_id in (
            "001",
            "002",
            "004",
            "005",
            "008",
            "011",
            "014",
            "018",
            "021",
            "025",
            "029",
            "031",
            "033",
            "038",
            "030",
        ):
            run_dut(dut_id)
    except Exception as ex:
        return fail(str(ex))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
