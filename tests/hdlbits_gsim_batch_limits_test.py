from __future__ import annotations

import importlib.util
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
                isolated_launch.python_args == ["-S"],
                f"present build-tree bindings should force -S isolation: {isolated_launch.python_args!r}",
            )
            expect(
                isolated_launch.env.get("PYTHONPATH") == str(build_root),
                f"present build-tree bindings should export PYTHONPATH to the build tree: {isolated_launch.env!r}",
            )
            expect(
                isolated_launch.env.get("PYTHONNOUSERSITE") == "1",
                f"present build-tree bindings should disable user site imports: {isolated_launch.env!r}",
            )
    except Exception as ex:
        return fail(str(ex))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
