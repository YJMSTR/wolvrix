from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
ARTIFACT_ROOT = REPO_ROOT / "build" / "artifacts" / "hdlbits_gsim_runtime_smoke"
SCRIPT_PATH = REPO_ROOT / "scripts" / "wolvrix_hdlbits_gsim_run.py"
BUILD_PYTHON_DIR = pathlib.Path(
    os.environ.get("WOLVRIX_PYTHON_BUILD_DIR", str(REPO_ROOT / "wolvrix" / "build" / "python"))
)


def fail(message: str) -> int:
    print(f"[hdlbits-gsim-runtime-smoke] {message}", file=sys.stderr)
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
        expect(SCRIPT_PATH.exists(), f"missing runtime helper: {SCRIPT_PATH}")

        env = os.environ.copy()
        env["PYTHONNOUSERSITE"] = "1"
        env["WOLVRIX_PYTHON_BUILD_DIR"] = str(BUILD_PYTHON_DIR)
        env["PYTHONPATH"] = str(BUILD_PYTHON_DIR)

        result = subprocess.run(
            [
                sys.executable,
                "-S",
                str(SCRIPT_PATH),
                "--dut",
                "001",
                "--out-dir",
                str(ARTIFACT_ROOT),
            ],
            capture_output=True,
            text=True,
            env=env,
            check=False,
        )
        expect(
            result.returncode == 0,
            "runtime helper failed\nstdout:\n"
            + (result.stdout or "")
            + "\nstderr:\n"
            + (result.stderr or ""),
        )

        header = ARTIFACT_ROOT / "dut_001.hpp"
        source = ARTIFACT_ROOT / "dut_001.cpp"
        runner = ARTIFACT_ROOT / "dut_001_runner.cpp"
        binary = ARTIFACT_ROOT / "dut_001_runner"
        expect(header.exists(), "missing emitted gsim header")
        expect(source.exists(), "missing emitted gsim source")
        expect(runner.exists(), "missing generated runtime runner")
        expect(binary.exists(), "missing compiled runtime runner")
    except Exception as ex:
        return fail(str(ex))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
