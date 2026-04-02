from __future__ import annotations

import pathlib
import shutil
import stat
import subprocess
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "resolve_xs_emu_path.py"
ARTIFACT_ROOT = REPO_ROOT / "build" / "artifacts" / "xs_emu_path_resolver"


def fail(message: str) -> int:
    print(f"[xs-emu-path-resolver] {message}", file=sys.stderr)
    return 1


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def reset_dir(path: pathlib.Path) -> None:
    shutil.rmtree(path, ignore_errors=True)
    path.mkdir(parents=True, exist_ok=True)


def make_executable(path: pathlib.Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def run_resolver(build_dir: pathlib.Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPT), str(build_dir)],
        cwd=str(REPO_ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )


def test_prefers_primary_emu(build_dir: pathlib.Path) -> None:
    make_executable(build_dir / "emu")
    make_executable(build_dir / "gsim-compile" / "emu")

    result = run_resolver(build_dir)
    expect(result.returncode == 0, f"primary emu should resolve: {result.stderr.strip()}")
    expect(
        result.stdout.strip() == str((build_dir / "emu").resolve()),
        f"expected build/emu, got {result.stdout.strip()}",
    )


def test_falls_back_to_gsim_compile(build_dir: pathlib.Path) -> None:
    make_executable(build_dir / "gsim-compile" / "emu")

    result = run_resolver(build_dir)
    expect(result.returncode == 0, f"fallback emu should resolve: {result.stderr.strip()}")
    expect(
        result.stdout.strip() == str((build_dir / "gsim-compile" / "emu").resolve()),
        f"expected gsim-compile/emu, got {result.stdout.strip()}",
    )


def test_ignores_broken_primary_symlink(build_dir: pathlib.Path) -> None:
    broken_target = build_dir / "missing-emu"
    (build_dir / "emu").parent.mkdir(parents=True, exist_ok=True)
    (build_dir / "emu").symlink_to(broken_target)
    make_executable(build_dir / "gsim-compile" / "emu")

    result = run_resolver(build_dir)
    expect(result.returncode == 0, f"broken symlink should fall back: {result.stderr.strip()}")
    expect(
        result.stdout.strip() == str((build_dir / "gsim-compile" / "emu").resolve()),
        f"expected fallback to gsim-compile/emu, got {result.stdout.strip()}",
    )


def test_fails_when_missing(build_dir: pathlib.Path) -> None:
    result = run_resolver(build_dir)
    expect(result.returncode != 0, "missing emu should fail")
    expect("no executable emu found" in result.stderr, f"missing diagnostic: {result.stderr.strip()}")


def main() -> int:
    try:
        reset_dir(ARTIFACT_ROOT)
        test_prefers_primary_emu(ARTIFACT_ROOT / "case_primary")
        test_falls_back_to_gsim_compile(ARTIFACT_ROOT / "case_fallback")
        test_ignores_broken_primary_symlink(ARTIFACT_ROOT / "case_broken_symlink")
        test_fails_when_missing(ARTIFACT_ROOT / "case_missing")
    except Exception as ex:
        return fail(str(ex))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
