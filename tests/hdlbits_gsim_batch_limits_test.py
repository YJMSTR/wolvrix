from __future__ import annotations

import importlib.util
import os
import pathlib
import sys
import tempfile


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "wolvrix_hdlbits_gsim_batch.py"


def fail(message: str) -> int:
    print(f"[hdlbits-gsim-batch-limits] {message}", file=sys.stderr)
    return 1


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def load_module():
    spec = importlib.util.spec_from_file_location("wolvrix_hdlbits_gsim_batch", SCRIPT)
    expect(spec is not None and spec.loader is not None, f"failed to load module from {SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def main() -> int:
    try:
        module = load_module()

        original_which = module.shutil.which
        module.shutil.which = lambda name: {  # type: ignore[assignment]
            "clang++": "/usr/bin/clang++",
            "g++": "/usr/bin/g++",
        }.get(name)
        expect(module.pick_cxx() == "/usr/bin/clang++", "clang++ should be preferred when available")
        module.shutil.which = lambda name: {  # type: ignore[assignment]
            "g++": "/usr/bin/g++",
        }.get(name)
        expect(module.pick_cxx() == "/usr/bin/g++", "g++ should be used when clang++ is unavailable")
        module.shutil.which = lambda _name: None  # type: ignore[assignment]
        try:
            module.pick_cxx()
            raise RuntimeError("missing compiler should raise RuntimeError")
        except RuntimeError as ex:
            expect("need clang++ or g++" in str(ex), f"unexpected compiler error: {ex}")
        module.shutil.which = original_which  # type: ignore[assignment]

        ok = module.run_subprocess_limited(
            [sys.executable, "-c", "print('ok')"],
            timeout_sec=2,
            memory_limit_mb=64,
        )
        expect(ok.returncode == 0, f"expected success, got {ok.returncode}: {ok.stderr}")
        expect(ok.stdout.strip() == "ok", f"missing success stdout: {ok.stdout!r}")

        timeout_result = module.run_subprocess_limited(
            [sys.executable, "-c", "import time; time.sleep(5)"],
            timeout_sec=1,
            memory_limit_mb=64,
        )
        expect(timeout_result.timed_out, "expected timeout marker")
        expect("Timeout after 1s" in timeout_result.stderr, f"missing timeout message: {timeout_result.stderr!r}")

        oom_result = module.run_subprocess_limited(
            [sys.executable, "-c", "x = bytearray(256 * 1024 * 1024); print(len(x))"],
            timeout_sec=5,
            memory_limit_mb=64,
        )
        expect(oom_result.returncode != 0, "expected non-zero return for capped allocation")
        expect(
            oom_result.memory_limited,
            f"expected memory-limited marker, got returncode={oom_result.returncode}, stderr={oom_result.stderr!r}",
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            build_root = pathlib.Path(tmpdir) / "build_python"
            build_root.mkdir(parents=True, exist_ok=True)
            build_launch = module.resolve_wolvrix_python_launch(str(build_root))
            expect(
                build_launch.python_args == [],
                f"missing build-tree bindings should not force -S isolation: {build_launch.python_args!r}",
            )
            expect(
                build_launch.env["WOLVRIX_PYTHON_BUILD_DIR"] == str(build_root),
                f"launch config should still forward build dir hint: {build_launch.env!r}",
            )
            expect(
                "PYTHONPATH" not in build_launch.env,
                f"missing build-tree bindings should not force PYTHONPATH: {build_launch.env!r}",
            )
            expect(
                "PYTHONNOUSERSITE" not in build_launch.env,
                f"missing build-tree bindings should not force PYTHONNOUSERSITE: {build_launch.env!r}",
            )

            pkg_dir = build_root / "wolvrix"
            pkg_dir.mkdir(parents=True, exist_ok=True)
            (pkg_dir / "__init__.py").write_text("_native = object()\n", encoding="utf-8")
            (pkg_dir / "_wolvrix.so").write_bytes(b"")
            isolated_launch = module.resolve_wolvrix_python_launch(str(build_root))
            expect(
                isolated_launch.python_args == [],
                f"unstamped build-tree bindings should not force -S isolation: {isolated_launch.python_args!r}",
            )
            expect(
                "PYTHONPATH" not in isolated_launch.env,
                f"unstamped build-tree bindings should not export PYTHONPATH to the build tree: {isolated_launch.env!r}",
            )
            expect(
                "PYTHONNOUSERSITE" not in isolated_launch.env,
                f"unstamped build-tree bindings should not disable user site imports: {isolated_launch.env!r}",
            )

            (build_root / ".python-executable").write_text(f"{sys.executable}\n", encoding="utf-8")
            stamped_launch = module.resolve_wolvrix_python_launch(str(build_root))
            expect(
                stamped_launch.python_args == ["-S"],
                f"matching stamped build-tree bindings should force -S isolation: {stamped_launch.python_args!r}",
            )
            expect(
                stamped_launch.env.get("PYTHONPATH") == str(build_root),
                f"matching stamped build-tree bindings should export PYTHONPATH to the build tree: {stamped_launch.env!r}",
            )
            expect(
                stamped_launch.env.get("PYTHONNOUSERSITE") == "1",
                f"matching stamped build-tree bindings should disable user site imports: {stamped_launch.env!r}",
            )

            (build_root / ".python-executable").write_text("/tmp/other-python\n", encoding="utf-8")
            mismatched_launch = module.resolve_wolvrix_python_launch(str(build_root))
            expect(
                mismatched_launch.python_args == [],
                f"mismatched build-tree bindings should not force -S isolation: {mismatched_launch.python_args!r}",
            )
            expect(
                "PYTHONPATH" not in mismatched_launch.env,
                f"mismatched build-tree bindings should not export PYTHONPATH to the build tree: {mismatched_launch.env!r}",
            )
            expect(
                "PYTHONNOUSERSITE" not in mismatched_launch.env,
                f"mismatched build-tree bindings should not disable user site imports: {mismatched_launch.env!r}",
            )

        with tempfile.TemporaryDirectory() as tmpdir:
            dut_path = pathlib.Path(tmpdir) / "dut_001.v"
            dut_path.write_text("module top_module; endmodule\n", encoding="utf-8")
            output_dir = pathlib.Path(tmpdir) / "out"
            output_dir.mkdir(parents=True, exist_ok=True)
            repo_root = pathlib.Path(tmpdir)

            original_runner = module.run_subprocess_limited
            captured: dict[str, object] = {}

            def fake_runner(argv, timeout_sec, memory_limit_mb, env=None):  # type: ignore[override]
                captured["argv"] = list(argv)
                captured["timeout_sec"] = timeout_sec
                captured["memory_limit_mb"] = memory_limit_mb
                captured["env"] = dict(env or {})
                return module.LimitedProcessResult(returncode=0, stdout="", stderr="")

            module.run_subprocess_limited = fake_runner  # type: ignore[assignment]
            try:
                result = module.run_single_dut_subprocess(
                    dut_path,
                    output_dir,
                    str(repo_root / "wolvrix" / "build" / "python"),
                    repo_root,
                    "runtime",
                    timeout_sec=17,
                    memory_limit_mb=321,
                )
            finally:
                module.run_subprocess_limited = original_runner  # type: ignore[assignment]

            expect(result.result == "success", f"runtime helper probe should succeed: {result}")
            expect(
                captured.get("memory_limit_mb") is None,
                f"runtime mode should not apply the configured memory cap to the outer helper process itself: {captured!r}",
            )
            expect(
                "--memory-limit-mb" in captured.get("argv", []),
                f"runtime mode should still forward the memory-limit flag to the helper argv: {captured!r}",
            )
            argv = captured.get("argv", [])
            expect(
                "--memory-limit-mb" in argv and argv[argv.index("--memory-limit-mb") + 1] == "321",
                f"runtime mode should forward the configured memory cap to the inner helper argv: {captured!r}",
            )

        with tempfile.TemporaryDirectory() as tmpdir:
            dut_path = pathlib.Path(tmpdir) / "dut_001.v"
            dut_path.write_text("module top_module; endmodule\n", encoding="utf-8")
            output_dir = pathlib.Path(tmpdir) / "out"
            output_dir.mkdir(parents=True, exist_ok=True)
            repo_root = pathlib.Path(tmpdir)

            original_runner = module.run_subprocess_limited
            captured_compile: dict[str, object] = {}

            def fake_compile_runner(argv, timeout_sec, memory_limit_mb, env=None):  # type: ignore[override]
                captured_compile["argv"] = list(argv)
                captured_compile["timeout_sec"] = timeout_sec
                captured_compile["memory_limit_mb"] = memory_limit_mb
                captured_compile["env"] = dict(env or {})
                return module.LimitedProcessResult(
                    returncode=0,
                    stdout='{"result": "success", "elapsed_ms": 0.0}\n',
                    stderr="",
                )

            module.run_subprocess_limited = fake_compile_runner  # type: ignore[assignment]
            try:
                result = module.run_single_dut_subprocess(
                    dut_path,
                    output_dir,
                    str(repo_root / "wolvrix" / "build" / "python"),
                    repo_root,
                    "compile",
                    timeout_sec=19,
                    memory_limit_mb=654,
                )
            finally:
                module.run_subprocess_limited = original_runner  # type: ignore[assignment]

            expect(result.result == "success", f"compile helper probe should succeed: {result}")
            expect(
                captured_compile.get("memory_limit_mb") is None,
                f"compile mode should not apply the configured memory cap to the outer helper process itself: {captured_compile!r}",
            )

        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = pathlib.Path(tmpdir)
            dut_dir = tmp / "dut"
            dut_dir.mkdir(parents=True, exist_ok=True)
            dut_path = dut_dir / "dut_001.v"
            dut_path.write_text("module top_module; endmodule\n", encoding="utf-8")
            custom_build_root = tmp / "custom_build"
            custom_python_dir = custom_build_root / "python"
            custom_python_dir.mkdir(parents=True, exist_ok=True)
            output_dir = tmp / "out"

            original_get_dut_files = module.get_dut_files
            original_run_single = module.run_single_dut_subprocess
            old_argv = sys.argv[:]
            old_env = os.environ.copy()
            captured_main: dict[str, object] = {}

            module.get_dut_files = lambda _dut_dir: [dut_path]  # type: ignore[assignment]

            def fake_run_single_main(dut_path_arg, output_dir_arg, python_path_arg, repo_root_arg,
                                     execution_mode_arg, timeout_sec=60, memory_limit_mb=None):  # type: ignore[override]
                captured_main["python_path"] = python_path_arg
                captured_main["repo_root"] = repo_root_arg
                captured_main["execution_mode"] = execution_mode_arg
                captured_main["timeout_sec"] = timeout_sec
                captured_main["memory_limit_mb"] = memory_limit_mb
                return module.DUTResult(
                    dut_id=module.extract_dut_id(dut_path_arg),
                    result="success",
                    elapsed_ms=0.0,
                )

            module.run_single_dut_subprocess = fake_run_single_main  # type: ignore[assignment]
            try:
                os.environ.clear()
                os.environ.update(old_env)
                os.environ["WOLVRIX_BUILD_DIR"] = str(custom_build_root)
                sys.argv = [
                    "wolvrix_hdlbits_gsim_batch.py",
                    "--dut-dir",
                    str(dut_dir),
                    "--output-dir",
                    str(output_dir),
                    "--max-duts",
                    "1",
                ]
                status = module.main()
            finally:
                module.get_dut_files = original_get_dut_files  # type: ignore[assignment]
                module.run_single_dut_subprocess = original_run_single  # type: ignore[assignment]
                sys.argv[:] = old_argv
                os.environ.clear()
                os.environ.update(old_env)

            expect(status == 0, f"batch main should succeed with mocked subprocess runner: {status}")
            expect(
                captured_main.get("python_path") == str(custom_python_dir),
                f"batch main should honor custom WOLVRIX_BUILD_DIR/python paths: {captured_main!r}",
            )
    except Exception as ex:
        return fail(str(ex))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
