from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "check_xs_gsim_cpp_budget.py"
ARTIFACT_ROOT = REPO_ROOT / "build" / "artifacts" / "xs_gsim_cpp_budget"


def fail(message: str) -> int:
    print(f"[xs-gsim-cpp-budget] {message}", file=sys.stderr)
    return 1


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def reset_dir(path: pathlib.Path) -> None:
    shutil.rmtree(path, ignore_errors=True)
    path.mkdir(parents=True, exist_ok=True)


def run_check(path: pathlib.Path, max_bytes: int) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPT), str(path), str(max_bytes), "XS_ALLOW_MONOLITHIC_GSIM=1"],
        cwd=str(REPO_ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )


def main() -> int:
    try:
        reset_dir(ARTIFACT_ROOT)
        small_cpp = ARTIFACT_ROOT / "small.cpp"
        large_cpp = ARTIFACT_ROOT / "large.cpp"

        small_cpp.write_text("int small_model() { return 0; }\n", encoding="utf-8")
        large_cpp.write_text("x" * 1024, encoding="utf-8")

        small = run_check(small_cpp, 2048)
        expect(small.returncode == 0, f"small artifact should pass: {small.stderr.strip()}")

        large = run_check(large_cpp, 128)
        expect(large.returncode == 2, f"large artifact should be rejected, got {large.returncode}")
        expect("monolithic Wolvrix-emitted C++ model" in large.stderr, "missing unsupported diagnostic")
        expect("XS_ALLOW_MONOLITHIC_GSIM=1" in large.stderr, "missing override hint")
    except Exception as ex:
        return fail(str(ex))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
