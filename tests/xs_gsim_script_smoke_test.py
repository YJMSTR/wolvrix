from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "wolvrix_xs_gsim.py"
ARTIFACT_ROOT = REPO_ROOT / "build" / "artifacts" / "xs_gsim_script_smoke"

MODULE_TEXT = """module SimTop(
    input logic clk,
    input logic rst,
    input logic [7:0] a,
    output logic [7:0] y
);
    logic [7:0] state;

    always_ff @(posedge clk) begin
        if (rst) begin
            state <= '0;
        end else begin
            state <= a + 8'd1;
        end
    end

    assign y = state;
endmodule
"""


def fail(message: str) -> int:
    print(f"[xs-gsim-smoke] {message}", file=sys.stderr)
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
        src_dir = ARTIFACT_ROOT / "src"
        out_dir = ARTIFACT_ROOT / "out"
        src_dir.mkdir(parents=True, exist_ok=True)
        out_dir.mkdir(parents=True, exist_ok=True)

        sv_path = src_dir / "SimTop.sv"
        filelist_path = src_dir / "fixture.f"
        read_args_path = src_dir / "read_args.txt"
        out_base = out_dir / "xs_fixture_gsim"

        sv_path.write_text(MODULE_TEXT, encoding="utf-8")
        filelist_path.write_text(f"{sv_path}\n", encoding="utf-8")
        read_args_path.write_text("\n", encoding="utf-8")

        env = dict(os.environ)
        env["PYTHONNOUSERSITE"] = "1"
        env["WOLVRIX_PYTHON_BUILD_DIR"] = str(REPO_ROOT / "wolvrix" / "build" / "python")
        env["PYTHONPATH"] = str(REPO_ROOT / "wolvrix" / "build" / "python")
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                str(filelist_path),
                "SimTop",
                str(out_base),
                str(read_args_path),
                "info",
            ],
            cwd=str(REPO_ROOT),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
            check=False,
        )
        expect(result.returncode == 0, f"script failed: {result.stderr.strip() or result.stdout.strip()}")

        header = out_base.with_suffix(".hpp")
        source = out_base.with_suffix(".cpp")
        compat_header = out_dir / "SimTop.h"
        expect(header.exists(), f"missing header artifact: {header}")
        expect(source.exists(), f"missing source artifact: {source}")
        expect(compat_header.exists(), f"missing downstream compatibility header: {compat_header}")

        header_text = header.read_text(encoding="utf-8")
        source_text = source.read_text(encoding="utf-8")
        compat_text = compat_header.read_text(encoding="utf-8")
        expect('class SSimTop' in header_text, 'missing downstream simulator-facing SSimTop API')
        expect('void set_reset(unsigned reset)' in header_text, 'missing set_reset API')
        expect('void step()' in header_text, 'missing step API')
        expect('get_difftest__DOT__exit()' in header_text, 'missing difftest exit accessor')
        expect('#include "xs_fixture_gsim.hpp"' in compat_text, 'missing compatibility include for emitted header')
        expect('metadata.graph_symbol = "SimTop";' in source_text, "missing graph symbol metadata")
        expect('metadata.scratchpad_namespace = "gsim.SimTop";' in source_text, "missing scratchpad namespace metadata")
    except Exception as ex:
        return fail(str(ex))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
