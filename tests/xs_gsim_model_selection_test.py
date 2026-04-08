from __future__ import annotations

import os
import pathlib
import shlex
import shutil
import subprocess
import sys
import time


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


def run_make_target(model_dir: pathlib.Path, build_dir: pathlib.Path, target: pathlib.Path) -> subprocess.CompletedProcess[str]:
    cpp_path = model_dir / "fixture.cpp"
    hpp_path = model_dir / "fixture.hpp"
    manifest_path = model_dir / "fixture.manifest"
    return subprocess.run(
        [
            "make",
            "-C",
            str(DIFFTEST_DIR),
            "-n",
            str(target),
            "GSIM=1",
            "WOLVRIX_GSIM=1",
            f"DESIGN_DIR={model_dir}",
            f"BUILD_DIR={build_dir}",
            f"WOLVRIX_GSIM_CPP={cpp_path}",
            f"WOLVRIX_GSIM_HPP={hpp_path}",
            f"WOLVRIX_GSIM_MANIFEST={manifest_path}",
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


def run_root_emu_stack_print(override: str | None = None) -> subprocess.CompletedProcess[str]:
    print_rule = (
        "print-xs-emu-stack-config:\n"
        "\t@printf 'STACK=%s\\nRUN_FRAGMENT=%s\\n' "
        "'$(XS_EMU_STACK)' "
        "'ulimit -s $(XS_EMU_STACK) && $(XS_EMU_PREFIX)'"
    )
    command = f"source env.sh && make --eval {shlex.quote(print_rule)} print-xs-emu-stack-config"
    if override is not None:
        command += f" XS_EMU_STACK={shlex.quote(override)}"
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


def run_root_emit_metadata_print(override: str | None = None) -> subprocess.CompletedProcess[str]:
    print_rule = (
        "print-xs-gsim-emit-metadata:\n"
        "\t@printf 'EMIT=%s\\nFRAGMENT=%s\\n' "
        "'$(XS_GSIM_EMIT_METADATA)' "
        "'$(if $(strip $(XS_GSIM_EMIT_METADATA)),WOLVRIX_XS_GSIM_EMIT_METADATA=$(XS_GSIM_EMIT_METADATA),)'"
    )
    command = f"source env.sh && make --eval {shlex.quote(print_rule)} print-xs-gsim-emit-metadata"
    if override is not None:
        command += f" XS_GSIM_EMIT_METADATA={shlex.quote(override)}"
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


def run_root_python_launch_print(build_dir: pathlib.Path) -> subprocess.CompletedProcess[str]:
    print_rule = (
        "print-xs-gsim-python-launch:\n"
        "\t@printf 'ARGS=%s\\nENV=%s\\n' "
        "'$(XS_WOLVRIX_PYTHON_ARGS)' "
        "'$(XS_WOLVRIX_PYTHON_ENV)'"
    )
    command = (
        f"source env.sh && make --eval {shlex.quote(print_rule)} print-xs-gsim-python-launch "
        f"XS_WOLVRIX_PYTHON_BUILD_DIR={shlex.quote(str(build_dir))}"
    )
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


def run_root_python_launch_print_with_wolvrix_build_dir(build_dir: pathlib.Path) -> subprocess.CompletedProcess[str]:
    print_rule = (
        "print-xs-gsim-python-launch:\n"
        "\t@printf 'ARGS=%s\\nENV=%s\\nDIR=%s\\n' "
        "'$(XS_WOLVRIX_PYTHON_ARGS)' "
        "'$(XS_WOLVRIX_PYTHON_ENV)' "
        "'$(XS_WOLVRIX_PYTHON_BUILD_DIR)'"
    )
    command = (
        f"source env.sh && make --eval {shlex.quote(print_rule)} print-xs-gsim-python-launch "
        f"WOLVRIX_BUILD_DIR={shlex.quote(str(build_dir))}"
    )
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


def run_root_global_python_env_print(build_dir: pathlib.Path) -> subprocess.CompletedProcess[str]:
    print_rule = (
        "print-root-python-env:\n"
        "\t@printf 'PYTHONPATH=%s\\nHAS_BUILD=%s\\n' "
        "'$(PYTHONPATH)' "
        "'$(WOLVRIX_HAS_BUILD_PYTHON)'"
    )
    command = (
        f"source env.sh && make --eval {shlex.quote(print_rule)} print-root-python-env "
        f"WOLVRIX_PYTHON_DIR={shlex.quote(str(build_dir))}"
    )
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


def run_root_gsim_artifact_dir_print(model_dir: pathlib.Path) -> subprocess.CompletedProcess[str]:
    base_rel = (model_dir / "fixture").relative_to(REPO_ROOT)
    print_rule = (
        "print-xs-gsim-artifact-dir:\n"
        "\t@printf 'DIR=%s\\n' '$(XS_WOLF_GSIM_DIR_ABS)'"
    )
    command = (
        f"source env.sh && make --eval {shlex.quote(print_rule)} print-xs-gsim-artifact-dir "
        f"XS_WOLF_GSIM_BASE={shlex.quote(str(base_rel))}"
    )
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


def run_gsim_link_flags_print(model_dir: pathlib.Path) -> subprocess.CompletedProcess[str]:
    build_dir = model_dir.parent / "build_link_flags"
    cpp_path = model_dir / "fixture.cpp"
    hpp_path = model_dir / "fixture.hpp"
    manifest_path = model_dir / "fixture.manifest"
    print_rule = (
        "print-gsim-link-flags:\n"
        "\t@printf 'FLAGS=%s\\n' '$(GSIM_LDFLAGS)'"
    )
    return subprocess.run(
        [
            "make",
            "-C",
            str(DIFFTEST_DIR),
            "--eval",
            print_rule,
            "print-gsim-link-flags",
            "GSIM=1",
            "WOLVRIX_GSIM=1",
            f"DESIGN_DIR={model_dir}",
            f"BUILD_DIR={build_dir}",
            f"WOLVRIX_GSIM_CPP={cpp_path}",
            f"WOLVRIX_GSIM_HPP={hpp_path}",
            f"WOLVRIX_GSIM_MANIFEST={manifest_path}",
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


def run_gsim_compile_flags_print(model_dir: pathlib.Path) -> subprocess.CompletedProcess[str]:
    build_dir = model_dir.parent / "build_compile_flags"
    cpp_path = model_dir / "fixture.cpp"
    hpp_path = model_dir / "fixture.hpp"
    manifest_path = model_dir / "fixture.manifest"
    print_rule = (
        "print-gsim-compile-flags:\n"
        "\t@printf 'FLAGS=%s\\n' '$(GSIM_CXXFLAGS)'"
    )
    return subprocess.run(
        [
            "make",
            "-C",
            str(DIFFTEST_DIR),
            "--eval",
            print_rule,
            "print-gsim-compile-flags",
            "GSIM=1",
            "WOLVRIX_GSIM=1",
            f"DESIGN_DIR={model_dir}",
            f"BUILD_DIR={build_dir}",
            f"WOLVRIX_GSIM_CPP={cpp_path}",
            f"WOLVRIX_GSIM_HPP={hpp_path}",
            f"WOLVRIX_GSIM_MANIFEST={manifest_path}",
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


def test_gsim_wrapper_rebuilds_when_emitted_header_changes(model_dir: pathlib.Path) -> None:
    build_dir = model_dir.parent / "build_wrapper_dep"
    hpp_path = model_dir / "fixture.hpp"
    write_file(hpp_path, "#pragma once\nclass SSimTop {};\n")
    write_file(model_dir / "fixture.cpp", "int fixture_model() { return 0; }\n")
    write_file(model_dir / "fixture.manifest", "fixture.cpp\n")

    wrapper_obj = build_dir / "gsim-compile" / "other" / "gsim.o"
    wrapper_obj.parent.mkdir(parents=True, exist_ok=True)
    wrapper_obj.write_bytes(b"")

    stale_time = time.time() - 10.0
    os.utime(wrapper_obj, (stale_time, stale_time))
    fresh_time = stale_time + 5.0
    os.utime(hpp_path, (fresh_time, fresh_time))

    result = run_make_target(model_dir, build_dir, wrapper_obj)
    stdout = result.stdout + result.stderr
    expect(result.returncode == 0, f"wrapper dependency dry-run should succeed: {stdout.strip()}")
    expect(
        str(DIFFTEST_DIR / "src" / "test" / "csrc" / "gsim" / "gsim.cpp") in stdout,
        "changing the emitted GSIM header should force a rebuild of the XiangShan gsim wrapper object",
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
        "BYTES=4194304" in stdout,
        f"root Makefile should default XiangShan behavior shard cap to 4 MiB: {stdout.strip()}",
    )
    expect(
        "FRAGMENT=WOLVRIX_XS_GSIM_BEHAVIOR_SHARD_MAX_BYTES=4194304" in stdout,
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


def test_root_make_uses_finite_emu_stack_by_default() -> None:
    result = run_root_emu_stack_print()
    stdout = result.stdout + result.stderr
    expect(result.returncode == 0, f"root-level emu stack print should succeed: {stdout.strip()}")
    expect(
        "STACK=65536" in stdout,
        f"root Makefile should default XiangShan emu stack to a finite value that works under capped hard limits: {stdout.strip()}",
    )
    expect(
        "RUN_FRAGMENT=ulimit -s 65536 &&" in stdout,
        f"root Makefile should still raise the XiangShan emu stack limit before launching emu: {stdout.strip()}",
    )


def test_root_make_disables_xiangshan_metadata_by_default() -> None:
    result = run_root_emit_metadata_print()
    stdout = result.stdout + result.stderr
    expect(result.returncode == 0, f"root-level emit-metadata print should succeed: {stdout.strip()}")
    expect(
        "EMIT=0" in stdout,
        f"root Makefile should default XiangShan gsim emission to runtime-only mode: {stdout.strip()}",
    )
    expect(
        "FRAGMENT=WOLVRIX_XS_GSIM_EMIT_METADATA=0" in stdout,
        f"root Makefile should forward runtime-only XiangShan emission into the gsim script env: {stdout.strip()}",
    )


def test_root_make_allows_xiangshan_metadata_override() -> None:
    result = run_root_emit_metadata_print("1")
    stdout = result.stdout + result.stderr
    expect(result.returncode == 0, f"root-level emit-metadata override print should succeed: {stdout.strip()}")
    expect(
        "EMIT=1" in stdout,
        f"root Makefile should allow forcing XiangShan metadata emission back on: {stdout.strip()}",
    )
    expect(
        "FRAGMENT=WOLVRIX_XS_GSIM_EMIT_METADATA=1" in stdout,
        f"root Makefile should forward the XiangShan metadata override into the gsim script env: {stdout.strip()}",
    )


def test_root_make_only_forces_build_tree_python_when_bindings_exist() -> None:
    missing_result = run_root_python_launch_print(ARTIFACT_ROOT / "missing_build_python")
    missing_stdout = missing_result.stdout + missing_result.stderr
    expect(missing_result.returncode == 0, f"root-level python launch print should succeed: {missing_stdout.strip()}")
    expect(
        "ARGS=" in missing_stdout and "ARGS=-S" not in missing_stdout,
        f"missing build-tree bindings should not force -S in run_xs_gsim: {missing_stdout.strip()}",
    )
    expect(
        "ENV=WOLVRIX_PYTHON_BUILD_DIR=" in missing_stdout,
        f"run_xs_gsim should still pass the build dir hint for load_wolvrix(): {missing_stdout.strip()}",
    )
    expect(
        "PYTHONPATH=" not in missing_stdout and "PYTHONNOUSERSITE=1" not in missing_stdout,
        f"missing build-tree bindings should not shadow an installed wolvrix package: {missing_stdout.strip()}",
    )

    build_dir = ARTIFACT_ROOT / "present_build_python"
    pkg_dir = build_dir / "wolvrix"
    pkg_dir.mkdir(parents=True, exist_ok=True)
    write_file(pkg_dir / "__init__.py", "_native = object()\n")
    (pkg_dir / "_wolvrix.so").write_bytes(b"")

    present_result = run_root_python_launch_print(build_dir)
    present_stdout = present_result.stdout + present_result.stderr
    expect(present_result.returncode == 0, f"root-level python launch print should succeed: {present_stdout.strip()}")
    expect(
        "ARGS=-S" not in present_stdout,
        f"unstamped build-tree bindings should not keep the isolated -S launch path: {present_stdout.strip()}",
    )
    expect(
        f"PYTHONPATH={build_dir}" not in present_stdout and "PYTHONNOUSERSITE=1" not in present_stdout,
        f"unstamped build-tree bindings should not pin imports to the build tree: {present_stdout.strip()}",
    )
    expect(
        "WOLVRIX_PYTHON_BUILD_DIR=" not in present_stdout,
        f"unstamped build-tree bindings should not hint XiangShan helpers toward the build tree: {present_stdout.strip()}",
    )

    mismatched_dir = ARTIFACT_ROOT / "mismatched_build_python"
    pkg_dir = mismatched_dir / "wolvrix"
    pkg_dir.mkdir(parents=True, exist_ok=True)
    write_file(pkg_dir / "__init__.py", "_native = object()\n")
    (pkg_dir / "_wolvrix.so").write_bytes(b"")
    write_file(mismatched_dir / ".python-executable", "/tmp/other-python\n")

    mismatched_result = run_root_python_launch_print(mismatched_dir)
    mismatched_stdout = mismatched_result.stdout + mismatched_result.stderr
    expect(mismatched_result.returncode == 0, f"root-level python launch print should succeed: {mismatched_stdout.strip()}")
    expect(
        "ARGS=-S" not in mismatched_stdout,
        f"mismatched build-tree bindings should not force the isolated -S launch path: {mismatched_stdout.strip()}",
    )
    expect(
        f"PYTHONPATH={mismatched_dir}" not in mismatched_stdout and "PYTHONNOUSERSITE=1" not in mismatched_stdout,
        f"mismatched build-tree bindings should not shadow the editable install for XiangShan flows: {mismatched_stdout.strip()}",
    )
    expect(
        "WOLVRIX_PYTHON_BUILD_DIR=" not in mismatched_stdout,
        f"mismatched build-tree bindings should not hint XiangShan helpers toward the build tree: {mismatched_stdout.strip()}",
    )

    stamped_dir = ARTIFACT_ROOT / "stamped_build_python"
    pkg_dir = stamped_dir / "wolvrix"
    pkg_dir.mkdir(parents=True, exist_ok=True)
    write_file(pkg_dir / "__init__.py", "_native = object()\n")
    (pkg_dir / "_wolvrix.so").write_bytes(b"")
    write_file(stamped_dir / ".python-executable", f"{sys.executable}\n")

    stamped_result = run_root_python_launch_print(stamped_dir)
    stamped_stdout = stamped_result.stdout + stamped_result.stderr
    expect(stamped_result.returncode == 0, f"root-level python launch print should succeed: {stamped_stdout.strip()}")
    expect(
        "ARGS=-S" in stamped_stdout,
        f"matching stamped build-tree bindings should keep the isolated -S launch path: {stamped_stdout.strip()}",
    )
    expect(
        f"PYTHONPATH={stamped_dir}" in stamped_stdout and "PYTHONNOUSERSITE=1" in stamped_stdout,
        f"matching stamped build-tree bindings should still pin imports to the build tree: {stamped_stdout.strip()}",
    )
    expect(
        f"ENV=WOLVRIX_PYTHON_BUILD_DIR={stamped_dir}" in stamped_stdout,
        f"matching stamped build-tree bindings should still hint XiangShan helpers toward the build tree: {stamped_stdout.strip()}",
    )


def test_xs_python_build_dir_follows_custom_wolvrix_build_dir() -> None:
    custom_build_dir = ARTIFACT_ROOT / "custom_wolvrix_build"
    python_build_dir = custom_build_dir / "python"
    pkg_dir = python_build_dir / "wolvrix"
    pkg_dir.mkdir(parents=True, exist_ok=True)
    write_file(pkg_dir / "__init__.py", "_native = object()\n")
    (pkg_dir / "_wolvrix.so").write_bytes(b"")
    write_file(python_build_dir / ".python-executable", f"{sys.executable}\n")

    result = run_root_python_launch_print_with_wolvrix_build_dir(custom_build_dir)
    stdout = result.stdout + result.stderr
    expect(result.returncode == 0, f"custom WOLVRIX_BUILD_DIR python launch print should succeed: {stdout.strip()}")
    expect(
        f"DIR={python_build_dir}" in stdout,
        f"XS helper python build dir should derive from custom WOLVRIX_BUILD_DIR by default: {stdout.strip()}",
    )
    expect(
        f"ENV=WOLVRIX_PYTHON_BUILD_DIR={python_build_dir}" in stdout,
        f"XS helper python env should point at the custom WOLVRIX_BUILD_DIR/python tree: {stdout.strip()}",
    )


def test_root_make_only_exports_global_pythonpath_for_complete_build_tree() -> None:
    missing_dir = ARTIFACT_ROOT / "global_py_missing"
    missing_dir.mkdir(parents=True, exist_ok=True)
    missing_result = run_root_global_python_env_print(missing_dir)
    missing_stdout = missing_result.stdout + missing_result.stderr
    expect(missing_result.returncode == 0, f"root-level global python env print should succeed: {missing_stdout.strip()}")
    expect(
        f"HAS_BUILD=" in missing_stdout and f"HAS_BUILD={missing_dir}" not in missing_stdout,
        f"missing build-tree bindings should not mark the global build python as complete: {missing_stdout.strip()}",
    )
    expect(
        f"PYTHONPATH={missing_dir}" not in missing_stdout,
        f"missing build-tree bindings should not prepend the half-built python dir globally: {missing_stdout.strip()}",
    )

    present_dir = ARTIFACT_ROOT / "global_py_present"
    pkg_dir = present_dir / "wolvrix"
    pkg_dir.mkdir(parents=True, exist_ok=True)
    write_file(pkg_dir / "__init__.py", "_native = object()\n")
    (pkg_dir / "_wolvrix.so").write_bytes(b"")
    present_result = run_root_global_python_env_print(present_dir)
    present_stdout = present_result.stdout + present_result.stderr
    expect(present_result.returncode == 0, f"root-level global python env print should succeed: {present_stdout.strip()}")
    expect(
        f"HAS_BUILD={present_dir}/wolvrix/_wolvrix.so" in present_stdout,
        f"complete build-tree bindings should mark the global build python as available: {present_stdout.strip()}",
    )
    expect(
        f"PYTHONPATH={present_dir}" not in present_stdout,
        f"unstamped build-tree bindings should not prepend the build python dir globally: {present_stdout.strip()}",
    )

    stamped_dir = ARTIFACT_ROOT / "global_py_stamped"
    pkg_dir = stamped_dir / "wolvrix"
    pkg_dir.mkdir(parents=True, exist_ok=True)
    write_file(pkg_dir / "__init__.py", "_native = object()\n")
    (pkg_dir / "_wolvrix.so").write_bytes(b"")
    write_file(stamped_dir / ".python-executable", f"{sys.executable}\n")
    stamped_result = run_root_global_python_env_print(stamped_dir)
    stamped_stdout = stamped_result.stdout + stamped_result.stderr
    expect(stamped_result.returncode == 0, f"root-level global python env print should succeed: {stamped_stdout.strip()}")
    expect(
        f"HAS_BUILD={stamped_dir}/wolvrix/_wolvrix.so" in stamped_stdout,
        f"matching stamped build-tree bindings should still mark the global build python as available: {stamped_stdout.strip()}",
    )
    expect(
        f"PYTHONPATH={stamped_dir}" in stamped_stdout,
        f"matching stamped build-tree bindings should prepend the build python dir globally: {stamped_stdout.strip()}",
    )


def test_py_install_tracks_active_python_interpreter() -> None:
    makefile_text = (REPO_ROOT / "Makefile").read_text(encoding="utf-8")
    start = makefile_text.index(".PHONY: py_install")
    end = makefile_text.index("\n\n$(HDLBITS_EMITTED_DUT)", start)
    body = makefile_text[start:end]
    expect(
        "WOLVRIX_PYTHON_STAMP" in body,
        "py_install should record the active Python interpreter in a stamp file",
    )
    expect(
        "import sys; print(sys.executable)" in body and "CURRENT_PYTHON" in body,
        "py_install should compare the current Python interpreter against the recorded stamp",
    )
    expect(
        "$(PYTHON) -m pip install -e $(WOLVRIX_DIR)" in body,
        "py_install should still reinstall the editable package when the interpreter changes",
    )
    expect(
        "WOLVRIX_PYTHON_STAMP" in makefile_text and "CURRENT_PYTHON" in makefile_text,
        "the Makefile should track and compare the active Python interpreter globally, not just inside py_install reuse logic",
    )
    expect(
        'else \\\n\t\techo "[PY] Installing editable wolvrix package"; \\\n\t\t$(PYTHON) -m pip install -e $(WOLVRIX_DIR);'
        not in body,
        "py_install should not limit editable installation to the mismatched-build branch",
    )
    expect(
        '-DPython_EXECUTABLE="$(CURRENT_PYTHON)"' in makefile_text,
        "Makefile should reconfigure CMake with the active Python interpreter before refreshing the build-tree stamp",
    )


def test_root_make_global_pythonpath_depends_on_matching_python_stamp() -> None:
    makefile_text = (REPO_ROOT / "Makefile").read_text(encoding="utf-8")
    expect(
        "WOLVRIX_ACTIVE_PYTHON_MATCHES" in makefile_text,
        "global PYTHONPATH export should depend on whether the build-tree binding matches the active Python interpreter",
    )
    expect(
        "WOLVRIX_PYTHON_STAMP" in makefile_text and "CURRENT_PYTHON" in makefile_text,
        "global PYTHONPATH guard should compare the build-tree stamp against the active interpreter",
    )


def test_root_make_forwards_build_dir_to_xs_gsim_downstream() -> None:
    makefile_text = (REPO_ROOT / "Makefile").read_text(encoding="utf-8")
    run_start = makefile_text.index("run_xs_gsim:")
    run_end = makefile_text.index("\nrun_xs_gsim_smoke:", run_start)
    run_body = makefile_text[run_start:run_end]
    expect(
        run_body.count("BUILD_DIR=$(BUILD_DIR)") >= 2,
        "run_xs_gsim should forward BUILD_DIR through both the logged and executed downstream XiangShan gsim invocations",
    )


def test_root_make_forwards_vm_build_jobs_to_xiangshan_gsim() -> None:
    makefile_text = (REPO_ROOT / "Makefile").read_text(encoding="utf-8")
    start = makefile_text.index("run_xs_gsim:")
    end = makefile_text.index("\nrun_xs_gsim_smoke:", start)
    body = makefile_text[start:end]

    expect(
        body.count("VM_BUILD_JOBS=$(XS_VM_BUILD_JOBS)") >= 2,
        "root Makefile should forward XS_VM_BUILD_JOBS through both the logged and executed downstream XiangShan gsim invocations",
    )


def test_root_make_forwards_xiangshan_feature_flags_to_gsim() -> None:
    makefile_text = (REPO_ROOT / "Makefile").read_text(encoding="utf-8")
    start = makefile_text.index("run_xs_gsim:")
    end = makefile_text.index("\nrun_xs_gsim_smoke:", start)
    body = makefile_text[start:end]

    expect(
        body.count("WITH_CHISELDB=$(XS_WITH_CHISELDB)") >= 2,
        "run_xs_gsim should forward XS_WITH_CHISELDB through both the logged and executed downstream XiangShan gsim invocations",
    )
    expect(
        body.count("WITH_CONSTANTIN=$(XS_WITH_CONSTANTIN)") >= 2,
        "run_xs_gsim should forward XS_WITH_CONSTANTIN through both the logged and executed downstream XiangShan gsim invocations",
    )
    expect(
        "WITH_CHISELDB=0" not in body and "WITH_CONSTANTIN=0" not in body,
        "run_xs_gsim should not hardcode XiangShan feature flags in the downstream gsim invocation",
    )


def test_xs_repcut_targets_depend_on_xs_wolf_emit() -> None:
    makefile_text = (REPO_ROOT / "Makefile").read_text(encoding="utf-8")
    for target in ("run_xs_repcut", "run_xs_repcut_partitioned_smoke", "build_xs_repcut_verilator"):
        marker = f"{target}:"
        start = makefile_text.index(marker)
        line_end = makefile_text.index("\n", start)
        header = makefile_text[start:line_end]
        expect(
            "xs_wolf_emit" in header,
            f"{target} should depend on xs_wolf_emit so the required Wolvrix JSON is generated on a clean checkout: {header}",
        )


def test_repcut_package_targets_forward_custom_wolvrix_build_dir() -> None:
    makefile_text = (REPO_ROOT / "Makefile").read_text(encoding="utf-8")
    for target in ("run_xs_repcut_partitioned_smoke", "build_xs_repcut_verilator"):
        start = makefile_text.index(f"{target}:")
        end = makefile_text.find("\n\n", start)
        body = makefile_text[start:end]
        expect(
            "WOLVRIX_BUILD_DIR=$(WOLVRIX_BUILD_DIR)" in body,
            f"{target} should log the custom WOLVRIX_BUILD_DIR when invoking the repcut packaging script",
        )
        expect(
            'WOLVRIX_BUILD_DIR="$(WOLVRIX_BUILD_DIR)"' in body,
            f"{target} should export the custom WOLVRIX_BUILD_DIR into the repcut packaging script process",
        )


def test_build_xs_repcut_verilator_honors_python_stamp_guard() -> None:
    makefile_text = (REPO_ROOT / "Makefile").read_text(encoding="utf-8")
    start = makefile_text.index("build_xs_repcut_verilator:")
    end = makefile_text.find("\n\n", start)
    body = makefile_text[start:end]
    expect(
        "WOLVRIX_MATCHING_BUILD_PYTHON" in body,
        "build_xs_repcut_verilator should honor the same matching-build-python guard as the rest of the Makefile",
    )


def test_root_make_forwards_simulator_build_options_to_gsim() -> None:
    makefile_text = (REPO_ROOT / "Makefile").read_text(encoding="utf-8")
    start = makefile_text.index("run_xs_gsim:")
    end = makefile_text.index("\nrun_xs_gsim_smoke:", start)
    body = makefile_text[start:end]

    expect(
        body.count("EMU_THREADS=$(XS_EMU_THREADS)") >= 2,
        "run_xs_gsim should forward XS_EMU_THREADS through both the logged and executed downstream XiangShan gsim invocations",
    )
    expect(
        body.count('SIM_VFLAGS="$(XS_SIM_VFLAGS)"') + body.count('SIM_VFLAGS=\\"$(XS_SIM_VFLAGS)\\"') >= 2,
        "run_xs_gsim should forward XS_SIM_VFLAGS through both the logged and executed downstream XiangShan gsim invocations",
    )
    expect(
        body.count("$(if $(filter 1,$(XS_WAVEFORM)),EMU_TRACE=fst,)") >= 2,
        "run_xs_gsim should forward the conditional EMU_TRACE=fst fragment through both downstream XiangShan gsim invocations",
    )


def test_root_make_derives_actual_gsim_artifact_dir_from_base_override(model_dir: pathlib.Path) -> None:
    result = run_root_gsim_artifact_dir_print(model_dir)
    stdout = result.stdout + result.stderr
    expect(result.returncode == 0, f"root-level gsim artifact dir print should succeed: {stdout.strip()}")
    expect(
        f"DIR={model_dir}" in stdout,
        f"root Makefile should derive the GSIM artifact directory from XS_WOLF_GSIM_BASE: {stdout.strip()}",
    )


def test_root_make_uses_actual_gsim_artifact_dir_for_budget_and_downstream() -> None:
    makefile_text = (REPO_ROOT / "Makefile").read_text(encoding="utf-8")

    budget_start = makefile_text.index("check_xs_gsim_budget:")
    budget_end = makefile_text.index("\n\nrun_xs_gsim:", budget_start)
    budget_body = makefile_text[budget_start:budget_end]
    expect(
        '--base-dir "$(abspath $(XS_WOLF_GSIM_DIR))"' in budget_body,
        "check_xs_gsim_budget should resolve manifest shards from the actual GSIM artifact directory",
    )
    expect(
        '$(XS_WOLF_EMIT_DIR)' not in budget_body,
        "check_xs_gsim_budget should not resolve manifest shards relative to the SV emit directory",
    )

    run_start = makefile_text.index("run_xs_gsim:")
    run_end = makefile_text.index("\nrun_xs_gsim_smoke:", run_start)
    run_body = makefile_text[run_start:run_end]
    expect(
        run_body.count("WOLVRIX_GSIM_INCLUDE_DIR=$(XS_WOLF_GSIM_DIR_ABS)") >= 1,
        "run_xs_gsim should pass the actual GSIM artifact directory to XiangShan downstream builds",
    )
    expect(
        "WOLVRIX_GSIM_INCLUDE_DIR=$(XS_WOLF_EMIT_DIR_ABS)" not in run_body,
        "run_xs_gsim should not hardcode XS_WOLF_EMIT_DIR as the downstream GSIM artifact directory",
    )


def test_root_make_resolves_xs_gsim_emu_from_actual_build_dir() -> None:
    makefile_text = (REPO_ROOT / "Makefile").read_text(encoding="utf-8")
    run_start = makefile_text.index("run_xs_gsim:")
    run_end = makefile_text.index("\nrun_xs_gsim_smoke:", run_start)
    run_body = makefile_text[run_start:run_end]
    expect(
        'XS_GSIM_BUILD_DIR="$(if $(filter /%,$(BUILD_DIR)),$(BUILD_DIR),$(XS_ROOT)/$(BUILD_DIR))"' in run_body,
        "run_xs_gsim should preserve absolute BUILD_DIR values while still resolving relative ones under XiangShan",
    )
    expect(
        'XS_GSIM_BUILD_DIR="$(XS_ROOT)/build"' not in run_body,
        "run_xs_gsim should not hardcode $(XS_ROOT)/build when locating the downstream gsim emulator",
    )


def test_root_make_honors_custom_waveform_path_for_xs_gsim() -> None:
    makefile_text = (REPO_ROOT / "Makefile").read_text(encoding="utf-8")
    run_start = makefile_text.index("run_xs_gsim:")
    run_end = makefile_text.index("\nrun_xs_gsim_smoke:", run_start)
    run_body = makefile_text[run_start:run_end]
    expect(
        run_body.count('$(if $(filter 1,$(XS_WAVEFORM))$(XS_WAVEFORM_PATH),--wave-path $(XS_WAVEFORM_PATH_ABS),$(if $(filter 1,$(XS_WAVEFORM)),--wave-path $$WAVEFORM,))') >= 2,
        "run_xs_gsim should honor XS_WAVEFORM_PATH in both the logged and executed emulator invocations",
    )
    expect(
        '--wave-path $(XS_WAVEFORM_PATH_ABS)' in run_body,
        "run_xs_gsim should prefer XS_WAVEFORM_PATH when a custom waveform location is provided",
    )


def test_gsim_reset_sequence_holds_reset_until_loop_end() -> None:
    emu_cpp = XIANGSHAN_DIR / "difftest" / "src" / "test" / "csrc" / "emu" / "emu.cpp"
    text = emu_cpp.read_text(encoding="utf-8")
    start = text.index("inline void Emulator::reset_ncycles")
    end = text.index("inline void Emulator::single_cycle()")
    body = text[start:end]
    expect(
        "    dut_ptr->set_reset(0);\n\n#ifdef GSIM\n    dut_ptr->step();\n#endif // GSIM\n" not in body,
        "GSIM reset_ncycles should not execute an extra low-reset step inside each reset-cycle iteration",
    )
    expect(
        "#ifdef GSIM\n    dut_ptr->set_clock(1);\n    dut_ptr->step();\n    dut_ptr->set_clock(0);\n#endif" in body,
        "GSIM reset_ncycles should drive clock high, step once, and restore clock low during each reset iteration",
    )
    expect(
        "    dut_ptr->set_reset(0);\n  }\n" in body,
        "GSIM reset_ncycles should still deassert reset before the next iteration begins",
    )


def test_gsim_wrapper_forwards_clock_and_emu_toggles_it() -> None:
    gsim_header = (XIANGSHAN_DIR / "difftest" / "src" / "test" / "csrc" / "gsim" / "gsim.h").read_text(encoding="utf-8")
    emu_cpp = (XIANGSHAN_DIR / "difftest" / "src" / "test" / "csrc" / "emu" / "emu.cpp").read_text(encoding="utf-8")

    expect(
        "dut->set_clock(static_cast<uint8_t>(clock));" in gsim_header or "dut->set_clock(clock);" in gsim_header,
        "GSIM simulator wrapper should forward set_clock() into the emitted model",
    )
    expect(
        "// Gsim does not use explicit clock. Simply call step() instead." not in gsim_header,
        "GSIM simulator wrapper should no longer treat the emitted clock input as a no-op",
    )

    single_cycle_start = emu_cpp.index("inline void Emulator::single_cycle()")
    tick_start = emu_cpp.index("int Emulator::tick()")
    single_cycle_body = emu_cpp[single_cycle_start:tick_start]
    expect(
        "#ifdef GSIM\n  dut_ptr->set_clock(1);\n  dut_ptr->step();\n  dut_ptr->set_clock(0);\n#endif // GSIM\n" in single_cycle_body,
        "GSIM single_cycle should drive clock high before stepping the emitted model",
    )
    expect(
        "  dut_ptr->set_clock(0);\n" in single_cycle_body,
        "GSIM single_cycle should restore clock low after stepping the emitted model",
    )


def test_gsim_link_disables_relax_and_pie(model_dir: pathlib.Path) -> None:
    write_file(model_dir / "fixture.hpp", "#pragma once\n")
    write_file(model_dir / "fixture.cpp", "int fixture_model() { return 0; }\n")
    write_file(model_dir / "fixture.manifest", "fixture.cpp\n")

    result = run_gsim_link_flags_print(model_dir)
    stdout = result.stdout + result.stderr
    expect(result.returncode == 0, f"gsim link flags print should succeed: {stdout.strip()}")
    expect(
        "-Wl,--no-relax" in stdout,
        f"XiangShan gsim link should disable linker relaxation for huge emu binaries: {stdout.strip()}",
    )
    expect(
        "-no-pie" in stdout,
        f"XiangShan gsim link should disable PIE for huge emu binaries: {stdout.strip()}",
    )


def test_gsim_uses_large_code_model_for_wolvrix_model(model_dir: pathlib.Path) -> None:
    write_file(model_dir / "fixture.hpp", "#pragma once\n")
    write_file(model_dir / "fixture.cpp", "int fixture_model() { return 0; }\n")
    write_file(model_dir / "fixture.manifest", "fixture.cpp\n")

    compile_result = run_gsim_compile_flags_print(model_dir)
    compile_stdout = compile_result.stdout + compile_result.stderr
    expect(compile_result.returncode == 0, f"gsim compile flags print should succeed: {compile_stdout.strip()}")
    expect(
        "-mcmodel=large" in compile_stdout,
        f"Wolvrix-backed XiangShan gsim compile flags should opt into a large code model: {compile_stdout.strip()}",
    )

    link_result = run_gsim_link_flags_print(model_dir)
    link_stdout = link_result.stdout + link_result.stderr
    expect(link_result.returncode == 0, f"gsim link flags print should succeed: {link_stdout.strip()}")
    expect(
        "-mcmodel=large" in link_stdout,
        f"Wolvrix-backed XiangShan gsim link flags should opt into a large code model: {link_stdout.strip()}",
    )


def main() -> int:
    try:
        reset_dir(ARTIFACT_ROOT)
        test_uses_manifest_instead_of_globbing(ARTIFACT_ROOT / "case_manifest" / "model")
        test_fails_without_manifest(ARTIFACT_ROOT / "case_missing_manifest" / "model")
        test_top_make_forwards_manifest(ARTIFACT_ROOT / "case_top_manifest" / "model")
        test_gsim_wrapper_rebuilds_when_emitted_header_changes(ARTIFACT_ROOT / "case_wrapper_dep" / "model")
        test_root_make_normalizes_relative_gsim_artifact_paths(ARTIFACT_ROOT / "case_root_relative" / "model")
        test_root_make_uses_safe_default_behavior_shard_cap()
        test_root_make_allows_behavior_shard_cap_override()
        test_root_make_uses_finite_emu_stack_by_default()
        test_root_make_disables_xiangshan_metadata_by_default()
        test_root_make_allows_xiangshan_metadata_override()
        test_root_make_only_forces_build_tree_python_when_bindings_exist()
        test_xs_python_build_dir_follows_custom_wolvrix_build_dir()
        test_root_make_only_exports_global_pythonpath_for_complete_build_tree()
        test_py_install_tracks_active_python_interpreter()
        test_root_make_global_pythonpath_depends_on_matching_python_stamp()
        test_root_make_forwards_vm_build_jobs_to_xiangshan_gsim()
        test_root_make_forwards_xiangshan_feature_flags_to_gsim()
        test_root_make_forwards_simulator_build_options_to_gsim()
        test_xs_repcut_targets_depend_on_xs_wolf_emit()
        test_repcut_package_targets_forward_custom_wolvrix_build_dir()
        test_build_xs_repcut_verilator_honors_python_stamp_guard()
        test_root_make_derives_actual_gsim_artifact_dir_from_base_override(ARTIFACT_ROOT / "case_gsim_artifact_dir" / "model")
        test_root_make_uses_actual_gsim_artifact_dir_for_budget_and_downstream()
        test_root_make_resolves_xs_gsim_emu_from_actual_build_dir()
        test_root_make_forwards_build_dir_to_xs_gsim_downstream()
        test_root_make_honors_custom_waveform_path_for_xs_gsim()
        test_gsim_reset_sequence_holds_reset_until_loop_end()
        test_gsim_wrapper_forwards_clock_and_emu_toggles_it()
        test_gsim_link_disables_relax_and_pie(ARTIFACT_ROOT / "case_link_flags" / "model")
        test_gsim_uses_large_code_model_for_wolvrix_model(ARTIFACT_ROOT / "case_large_code_model" / "model")
    except Exception as ex:
        return fail(str(ex))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
