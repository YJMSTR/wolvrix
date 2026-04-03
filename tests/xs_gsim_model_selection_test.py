from __future__ import annotations

import pathlib
import shlex
import shutil
import subprocess
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
DIFFTEST_DIR = REPO_ROOT / "testcase" / "xiangshan" / "difftest"
XIANGSHAN_DIR = REPO_ROOT / "testcase" / "xiangshan"
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


def write_file(path: pathlib.Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def run_make(model_dir: pathlib.Path) -> subprocess.CompletedProcess[str]:
    build_dir = model_dir.parent / "build"
    cpp_path = model_dir / "fixture.cpp"
    hpp_path = model_dir / "fixture.hpp"
    return subprocess.run(
        [
            "make",
            "-C",
            str(DIFFTEST_DIR),
            "-n",
            "gsim-build-emu",
            "GSIM=1",
            "WOLVRIX_GSIM=1",
            f"DESIGN_DIR={model_dir}",
            f"BUILD_DIR={build_dir}",
            f"WOLVRIX_GSIM_CPP={cpp_path}",
            f"WOLVRIX_GSIM_HPP={hpp_path}",
            f"WOLVRIX_GSIM_INCLUDE_DIR={model_dir}",
            "EMU_OPTIMIZE=-O0",
            "WITH_CHISELDB=0",
            "WITH_CONSTANTIN=0",
        ],
        cwd=str(REPO_ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )


def run_top_make(model_dir: pathlib.Path) -> subprocess.CompletedProcess[str]:
    build_dir = model_dir.parent / "build_top"
    cpp_path = model_dir / "fixture.cpp"
    hpp_path = model_dir / "fixture.hpp"
    manifest_path = model_dir / "fixture.manifest"
    return subprocess.run(
        [
            "make",
            "-C",
            str(XIANGSHAN_DIR),
            "-n",
            "gsim",
            "GSIM=1",
            "NUM_CORES=1",
            "RTL_SUFFIX=sv",
            "WITH_CHISELDB=0",
            "WITH_CONSTANTIN=0",
            f"NOOP_HOME={REPO_ROOT}",
            f"DESIGN_DIR={model_dir}",
            f"BUILD_DIR={build_dir}",
            "WOLVRIX_GSIM=1",
            f"WOLVRIX_GSIM_CPP={cpp_path}",
            f"WOLVRIX_GSIM_HPP={hpp_path}",
            f"WOLVRIX_GSIM_MANIFEST={manifest_path}",
            f"WOLVRIX_GSIM_INCLUDE_DIR={model_dir}",
        ],
        cwd=str(REPO_ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )


def run_root_path_print(model_dir: pathlib.Path) -> subprocess.CompletedProcess[str]:
    base_rel = (model_dir / "fixture").relative_to(REPO_ROOT)
    print_rule = (
        "print-xs-gsim-paths:\n"
        "\t@printf 'CPP=%s\\nHPP=%s\\nMANIFEST=%s\\n' "
        "'$(XS_WOLF_GSIM_CPP_ABS)' '$(XS_WOLF_GSIM_HPP_ABS)' '$(XS_WOLF_GSIM_MANIFEST_ABS)'"
    )
    return subprocess.run(
        [
            "bash",
            "-lc",
            f"source env.sh && make --eval {shlex.quote(print_rule)} "
            "print-xs-gsim-paths "
            f"XS_WOLF_GSIM_BASE={shlex.quote(str(base_rel))}",
        ],
        cwd=str(REPO_ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )


def run_root_shard_config_print(override: str | None = None) -> subprocess.CompletedProcess[str]:
    print_rule = (
        "print-xs-gsim-shard-config:\n"
        "\t@printf 'BYTES=%s\\nFRAGMENT=%s\\n' "
        "'$(XS_GSIM_BEHAVIOR_SHARD_MAX_BYTES)' "
        "'$(if $(strip $(XS_GSIM_BEHAVIOR_SHARD_MAX_BYTES)),WOLVRIX_XS_GSIM_BEHAVIOR_SHARD_MAX_BYTES=$(XS_GSIM_BEHAVIOR_SHARD_MAX_BYTES),)'"
    )
    command = f"source env.sh && make --eval {shlex.quote(print_rule)} print-xs-gsim-shard-config"
    if override is not None:
        command += f" XS_GSIM_BEHAVIOR_SHARD_MAX_BYTES={shlex.quote(override)}"
    return subprocess.run(
        [
            "bash",
            "-lc",
            command,
        ],
        cwd=str(REPO_ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )


def test_uses_manifest_instead_of_globbing(model_dir: pathlib.Path) -> None:
    canonical_cpp = model_dir / "fixture.cpp"
    shard_cpp = model_dir / "fixture__meta_000.cpp"
    stale_cpp = model_dir / "fixture_rerun.cpp"
    manifest = model_dir / "fixture.manifest"

    write_file(model_dir / "fixture.hpp", "#pragma once\n")
    write_file(canonical_cpp, "int fixture_model() { return 0; }\n")
    write_file(shard_cpp, "int fixture_model_meta() { return 0; }\n")
    write_file(stale_cpp, "int fixture_model_stale() { return 0; }\n")
    write_file(manifest, "fixture.cpp\nfixture__meta_000.cpp\n")

    result = run_make(model_dir)
    stdout = result.stdout + result.stderr
    expect(result.returncode == 0, f"dry-run should succeed with manifest present: {stdout.strip()}")
    expect(str(canonical_cpp) in stdout, "dry-run should compile the canonical emitted source")
    expect(str(shard_cpp) in stdout, "dry-run should compile manifest-listed shards")
    expect(str(stale_cpp) not in stdout, "dry-run should ignore stale unrelated .cpp files in the model directory")


def test_fails_without_manifest(model_dir: pathlib.Path) -> None:
    write_file(model_dir / "fixture.hpp", "#pragma once\n")
    write_file(model_dir / "fixture.cpp", "int fixture_model() { return 0; }\n")

    result = run_make(model_dir)
    stderr = result.stdout + result.stderr
    expect(result.returncode != 0, "dry-run should fail when the Wolvrix gsim manifest is missing")
    expect("manifest" in stderr.lower(), f"missing-manifest diagnostic should mention manifest: {stderr.strip()}")


def test_top_make_forwards_manifest(model_dir: pathlib.Path) -> None:
    canonical_cpp = model_dir / "fixture.cpp"
    hpp_path = model_dir / "fixture.hpp"
    manifest_path = model_dir / "fixture.manifest"
    write_file(hpp_path, "#pragma once\n")
    write_file(canonical_cpp, "int fixture_model() { return 0; }\n")
    write_file(manifest_path, "fixture.cpp\n")

    result = run_top_make(model_dir)
    stdout = result.stdout + result.stderr
    expect(result.returncode == 0, f"top-level dry-run should succeed: {stdout.strip()}")
    expect(
        f'WOLVRIX_GSIM_MANIFEST="{manifest_path}"' in stdout,
        f"top-level XiangShan make should forward the manifest to difftest: {stdout.strip()}",
    )


def test_root_make_normalizes_relative_gsim_artifact_paths(model_dir: pathlib.Path) -> None:
    canonical_cpp = model_dir / "fixture.cpp"
    hpp_path = model_dir / "fixture.hpp"
    manifest_path = model_dir / "fixture.manifest"
    write_file(hpp_path, "#pragma once\n")
    write_file(canonical_cpp, "int fixture_model() { return 0; }\n")
    write_file(manifest_path, "fixture.cpp\n")

    result = run_root_path_print(model_dir)
    stdout = result.stdout + result.stderr
    expect(result.returncode == 0, f"root-level path print should succeed: {stdout.strip()}")
    expect(
        f"CPP={canonical_cpp}" in stdout,
        f"root Makefile should normalize the emitted cpp path from XS_WOLF_GSIM_BASE: {stdout.strip()}",
    )
    expect(
        f"HPP={hpp_path}" in stdout,
        f"root Makefile should normalize the emitted header path from XS_WOLF_GSIM_BASE: {stdout.strip()}",
    )
    expect(
        f"MANIFEST={manifest_path}" in stdout,
        f"root Makefile should normalize the emitted manifest path from XS_WOLF_GSIM_BASE: {stdout.strip()}",
    )


def test_root_make_uses_safe_default_behavior_shard_cap() -> None:
    result = run_root_shard_config_print()
    stdout = result.stdout + result.stderr
    expect(result.returncode == 0, f"root-level shard config print should succeed: {stdout.strip()}")
    expect(
        "BYTES=16777216" in stdout,
        f"root Makefile should default XiangShan behavior shard cap to 16 MiB: {stdout.strip()}",
    )
    expect(
        "FRAGMENT=WOLVRIX_XS_GSIM_BEHAVIOR_SHARD_MAX_BYTES=16777216" in stdout,
        f"root Makefile should forward the default shard cap into the XiangShan gsim script env: {stdout.strip()}",
    )


def test_root_make_allows_behavior_shard_cap_override() -> None:
    result = run_root_shard_config_print("1048576")
    stdout = result.stdout + result.stderr
    expect(result.returncode == 0, f"root-level shard override print should succeed: {stdout.strip()}")
    expect(
        "BYTES=1048576" in stdout,
        f"root Makefile should allow overriding the XiangShan behavior shard cap: {stdout.strip()}",
    )
    expect(
        "FRAGMENT=WOLVRIX_XS_GSIM_BEHAVIOR_SHARD_MAX_BYTES=1048576" in stdout,
        f"root Makefile should forward the override shard cap into the XiangShan gsim script env: {stdout.strip()}",
    )


def main() -> int:
    try:
        reset_dir(ARTIFACT_ROOT)
        test_uses_manifest_instead_of_globbing(ARTIFACT_ROOT / "case_manifest" / "model")
        test_fails_without_manifest(ARTIFACT_ROOT / "case_missing_manifest" / "model")
        test_top_make_forwards_manifest(ARTIFACT_ROOT / "case_top_manifest" / "model")
        test_root_make_normalizes_relative_gsim_artifact_paths(ARTIFACT_ROOT / "case_root_relative" / "model")
        test_root_make_uses_safe_default_behavior_shard_cap()
        test_root_make_allows_behavior_shard_cap_override()
    except Exception as ex:
        return fail(str(ex))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
