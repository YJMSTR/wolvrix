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


def test_root_make_uses_unlimited_emu_stack_by_default() -> None:
    result = run_root_emu_stack_print()
    stdout = result.stdout + result.stderr
    expect(result.returncode == 0, f"root-level emu stack print should succeed: {stdout.strip()}")
    expect(
        "STACK=unlimited" in stdout,
        f"root Makefile should default XiangShan emu stack to unlimited for large gsim shards: {stdout.strip()}",
    )
    expect(
        "RUN_FRAGMENT=ulimit -s unlimited &&" in stdout,
        f"root Makefile should raise the XiangShan emu stack limit before launching emu: {stdout.strip()}",
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
        test_root_make_normalizes_relative_gsim_artifact_paths(ARTIFACT_ROOT / "case_root_relative" / "model")
        test_root_make_uses_safe_default_behavior_shard_cap()
        test_root_make_allows_behavior_shard_cap_override()
        test_root_make_uses_unlimited_emu_stack_by_default()
        test_root_make_disables_xiangshan_metadata_by_default()
        test_root_make_allows_xiangshan_metadata_override()
        test_gsim_reset_sequence_holds_reset_until_loop_end()
        test_gsim_wrapper_forwards_clock_and_emu_toggles_it()
        test_gsim_link_disables_relax_and_pie(ARTIFACT_ROOT / "case_link_flags" / "model")
        test_gsim_uses_large_code_model_for_wolvrix_model(ARTIFACT_ROOT / "case_large_code_model" / "model")
    except Exception as ex:
        return fail(str(ex))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
