from __future__ import annotations

import os
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


def run_check(path: pathlib.Path, max_bytes: int, log_path: pathlib.Path | None = None) -> subprocess.CompletedProcess[str]:
    command = [sys.executable, str(SCRIPT)]
    if log_path is not None:
        command.extend(["--log-file", str(log_path)])
    command.extend([str(path), str(max_bytes), "XS_ALLOW_MONOLITHIC_GSIM=1"])
    return subprocess.run(
        command,
        cwd=str(REPO_ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )


def run_manifest_check(
    manifest_path: pathlib.Path,
    base_dir: pathlib.Path,
    max_bytes: int,
    log_path: pathlib.Path | None = None,
) -> subprocess.CompletedProcess[str]:
    command = [sys.executable, str(SCRIPT)]
    if log_path is not None:
        command.extend(["--log-file", str(log_path)])
    command.extend(["--manifest", str(manifest_path), "--base-dir", str(base_dir), str(max_bytes), "XS_ALLOW_MONOLITHIC_GSIM=1"])
    return subprocess.run(
        command,
        cwd=str(REPO_ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )


def run_make_manifest_check(
    manifest_path: pathlib.Path,
    base_dir: pathlib.Path,
    max_bytes: int,
    log_path: pathlib.Path,
) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    env["PYTHONNOUSERSITE"] = "1"
    return subprocess.run(
        [
            "bash",
            "-lc",
            "source env.sh && make check_xs_gsim_budget "
            f"XS_WOLF_GSIM_MANIFEST={manifest_path} "
            f"XS_WOLF_EMIT_DIR={base_dir} "
            f"XS_GSIM_MAX_CPP_BYTES={max_bytes} "
            f"XS_BUILD_LOG_FILE={log_path} "
            "XS_ALLOW_MONOLITHIC_GSIM=0",
        ],
        cwd=str(REPO_ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
        check=False,
    )


def main() -> int:
    try:
        reset_dir(ARTIFACT_ROOT)
        small_cpp = ARTIFACT_ROOT / "small.cpp"
        large_cpp = ARTIFACT_ROOT / "large.cpp"
        shard_cpp = ARTIFACT_ROOT / "step_000.cpp"
        manifest = ARTIFACT_ROOT / "small.manifest"
        reject_log = ARTIFACT_ROOT / "reject.log"
        manifest_reject_log = ARTIFACT_ROOT / "manifest_reject.log"
        make_reject_log = ARTIFACT_ROOT / "make_reject.log"

        small_cpp.write_text("int small_model() { return 0; }\n", encoding="utf-8")
        large_cpp.write_text("x" * 1024, encoding="utf-8")
        shard_cpp.write_text("y" * 1024, encoding="utf-8")
        manifest.write_text("small.cpp\nstep_000.cpp\n", encoding="utf-8")

        small = run_check(small_cpp, 2048)
        expect(small.returncode == 0, f"small artifact should pass: {small.stderr.strip()}")

        large = run_check(large_cpp, 128, reject_log)
        expect(large.returncode == 2, f"large artifact should be rejected, got {large.returncode}")
        expect("monolithic Wolvrix-emitted C++ model" in large.stderr, "missing unsupported diagnostic")
        expect("XS_ALLOW_MONOLITHIC_GSIM=1" in large.stderr, "missing override hint")
        expect(reject_log.exists(), "rejected artifact should write the diagnostic to the requested log file")
        expect("monolithic Wolvrix-emitted C++ model" in reject_log.read_text(encoding="utf-8"),
               "log file should capture the unsupported diagnostic")

        manifest_large = run_manifest_check(manifest, ARTIFACT_ROOT, 128, manifest_reject_log)
        expect(manifest_large.returncode == 2, f"manifest budget should reject oversized shard, got {manifest_large.returncode}")
        expect(str(shard_cpp) in manifest_large.stderr, "manifest budget diagnostic should mention the oversized shard")
        expect(manifest_reject_log.exists(), "manifest rejection should write the diagnostic log")

        make_large = run_make_manifest_check(manifest, ARTIFACT_ROOT, 128, make_reject_log)
        make_output = make_large.stdout + make_large.stderr
        expect(make_large.returncode != 0, "Makefile preflight should reject an oversized shard from the manifest")
        expect("step_000.cpp" in make_output, f"Makefile preflight should report the oversized shard: {make_output.strip()}")
    except Exception as ex:
        return fail(str(ex))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
