from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
DIFFTEST_DIR = REPO_ROOT / "testcase" / "xiangshan" / "difftest"
ARTIFACT_ROOT = REPO_ROOT / "build" / "artifacts" / "xs_gsim_model_selection"


def fail(message: str) -> int:
    print(f"[xs-gsim-model-selection] {message}", file=sys.stderr)
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
        design_dir = ARTIFACT_ROOT / "fixture_design"
        model_dir = design_dir / "model"
        model_dir.mkdir(parents=True, exist_ok=True)

        canonical_cpp = model_dir / "fixture.cpp"
        canonical_hpp = model_dir / "fixture.hpp"
        stale_cpp = model_dir / "fixture_rerun.cpp"

        canonical_cpp.write_text("int fixture_model() { return 0; }\n", encoding="utf-8")
        canonical_hpp.write_text("#pragma once\n", encoding="utf-8")
        stale_cpp.write_text("int stale_model() { return 1; }\n", encoding="utf-8")

        result = subprocess.run(
            [
                "make",
                "-C",
                str(DIFFTEST_DIR),
                "-n",
                "gsim-build-emu",
                "GSIM=1",
                "WOLVRIX_GSIM=1",
                f"DESIGN_DIR={design_dir}",
                f"BUILD_DIR={design_dir / 'build'}",
                f"WOLVRIX_GSIM_CPP={canonical_cpp}",
                f"WOLVRIX_GSIM_HPP={canonical_hpp}",
                f"WOLVRIX_GSIM_INCLUDE_DIR={model_dir}",
                "EMU_OPTIMIZE=-O0 -g0",
                "WITH_CHISELDB=0",
                "WITH_CONSTANTIN=0",
            ],
            cwd=str(REPO_ROOT),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        expect(result.returncode == 0, f"dry-run make failed: {result.stdout.strip()}")

        output = result.stdout
        expect(str(canonical_cpp) in output, "expected canonical Wolvrix model source in dry-run output")
        expect(str(stale_cpp) not in output, "stale extra model source should not be compiled")
    except Exception as ex:
        return fail(str(ex))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
