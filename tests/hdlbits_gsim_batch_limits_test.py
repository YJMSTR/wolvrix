from __future__ import annotations

import importlib.util
import pathlib
import sys


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
    except Exception as ex:
        return fail(str(ex))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
