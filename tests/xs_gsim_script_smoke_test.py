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

RESET_NAMED_MODULE_TEXT = """module SimTop(
    input logic reset,
    output logic y
);
    assign y = reset;
endmodule
"""


def make_shard_module_text(length: int) -> str:
    signals = "\n".join(f"    logic [7:0] s{i};" for i in range(length))
    assigns: list[str] = []
    for i in range(length):
        src = "a" if i == 0 else f"s{i - 1}"
        assigns.append(f"    assign s{i} = {src} + 8'd1;")
    assigns.append(f"    assign y = s{length - 1};")
    return (
        "module SimTop(\n"
        "    input logic [7:0] a,\n"
        "    output logic [7:0] y\n"
        ");\n"
        f"{signals}\n\n"
        f"{chr(10).join(assigns)}\n"
        "endmodule\n"
    )


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

        reset_named_src_dir = ARTIFACT_ROOT / "reset_named_src"
        reset_named_out_dir = ARTIFACT_ROOT / "reset_named_out"
        reset_named_src_dir.mkdir(parents=True, exist_ok=True)
        reset_named_out_dir.mkdir(parents=True, exist_ok=True)
        reset_named_sv_path = reset_named_src_dir / "SimTop.sv"
        reset_named_filelist_path = reset_named_src_dir / "fixture.f"
        reset_named_read_args_path = reset_named_src_dir / "read_args.txt"
        reset_named_out_base = reset_named_out_dir / "xs_fixture_reset_named"

        reset_named_sv_path.write_text(RESET_NAMED_MODULE_TEXT, encoding="utf-8")
        reset_named_filelist_path.write_text(f"{reset_named_sv_path}\n", encoding="utf-8")
        reset_named_read_args_path.write_text("\n", encoding="utf-8")

        reset_named_result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                str(reset_named_filelist_path),
                "SimTop",
                str(reset_named_out_base),
                str(reset_named_read_args_path),
                "info",
            ],
            cwd=str(REPO_ROOT),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
            check=False,
        )
        expect(
            reset_named_result.returncode == 0,
            f"script reset-named smoke failed: {reset_named_result.stderr.strip() or reset_named_result.stdout.strip()}",
        )
        reset_named_header_text = reset_named_out_base.with_suffix(".hpp").read_text(encoding="utf-8")
        expect(reset_named_header_text.count("void set_reset(") == 1, "reset-named model should expose exactly one set_reset overload")
        expect(
            "void set_reset(unsigned reset) { input_reset_ = static_cast<std::uint8_t>(reset); }" in reset_named_header_text,
            "reset-named model should route set_reset(unsigned) into the emitted reset input storage",
        )

        shard_src_dir = ARTIFACT_ROOT / "shard_src"
        shard_out_dir = ARTIFACT_ROOT / "shard_out"
        shard_src_dir.mkdir(parents=True, exist_ok=True)
        shard_out_dir.mkdir(parents=True, exist_ok=True)
        shard_sv_path = shard_src_dir / "SimTop.sv"
        shard_filelist_path = shard_src_dir / "fixture.f"
        shard_read_args_path = shard_src_dir / "read_args.txt"
        shard_out_base = shard_out_dir / "xs_fixture_gsim_shards"
        shard_sv_path.write_text(make_shard_module_text(128), encoding="utf-8")
        shard_filelist_path.write_text(f"{shard_sv_path}\n", encoding="utf-8")
        shard_read_args_path.write_text("\n", encoding="utf-8")

        shard_env = dict(env)
        shard_env["WOLVRIX_XS_GSIM_BEHAVIOR_SHARD_MAX_BYTES"] = "256"
        shard_result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                str(shard_filelist_path),
                "SimTop",
                str(shard_out_base),
                str(shard_read_args_path),
                "info",
            ],
            cwd=str(REPO_ROOT),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=shard_env,
            check=False,
        )
        expect(shard_result.returncode == 0,
               f"script shard-cap override failed: {shard_result.stderr.strip() or shard_result.stdout.strip()}")
        shard_manifest_lines = [
            line.strip()
            for line in shard_out_base.with_suffix(".manifest").read_text(encoding="utf-8").splitlines()
            if line.strip()
        ]
        shard_paths = [shard_out_dir / line for line in shard_manifest_lines if "__step_" in line]
        expect(shard_paths, "script should forward env-controlled shard cap and produce step shards")
        for shard_path in shard_paths:
            expect(shard_path.stat().st_size <= 256,
                   f"script-forwarded shard cap should bound emitted shard size: {shard_path}")

        runtime_src_dir = ARTIFACT_ROOT / "runtime_only_src"
        runtime_out_dir = ARTIFACT_ROOT / "runtime_only_out"
        runtime_src_dir.mkdir(parents=True, exist_ok=True)
        runtime_out_dir.mkdir(parents=True, exist_ok=True)
        runtime_sv_path = runtime_src_dir / "SimTop.sv"
        runtime_filelist_path = runtime_src_dir / "fixture.f"
        runtime_read_args_path = runtime_src_dir / "read_args.txt"
        runtime_out_base = runtime_out_dir / "xs_fixture_gsim_runtime_only"
        runtime_sv_path.write_text(MODULE_TEXT, encoding="utf-8")
        runtime_filelist_path.write_text(f"{runtime_sv_path}\n", encoding="utf-8")
        runtime_read_args_path.write_text("\n", encoding="utf-8")

        runtime_env = dict(env)
        runtime_env["WOLVRIX_XS_GSIM_EMIT_METADATA"] = "0"
        runtime_result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                str(runtime_filelist_path),
                "SimTop",
                str(runtime_out_base),
                str(runtime_read_args_path),
                "info",
            ],
            cwd=str(REPO_ROOT),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=runtime_env,
            check=False,
        )
        expect(runtime_result.returncode == 0,
               f"script runtime-only metadata suppression failed: {runtime_result.stderr.strip() or runtime_result.stdout.strip()}")
        runtime_header_text = runtime_out_base.with_suffix(".hpp").read_text(encoding="utf-8")
        runtime_source_text = runtime_out_base.with_suffix(".cpp").read_text(encoding="utf-8")
        runtime_manifest_lines = [
            line.strip()
            for line in runtime_out_base.with_suffix(".manifest").read_text(encoding="utf-8").splitlines()
            if line.strip()
        ]
        expect("GsimMetadata_" not in runtime_header_text,
               "runtime-only XiangShan script mode should omit metadata structs from the public header")
        expect("make_SimTop_metadata" not in runtime_source_text,
               "runtime-only XiangShan script mode should omit metadata constructors from emitted source")
        expect("validate_SimTop_metadata" not in runtime_source_text,
               "runtime-only XiangShan script mode should omit metadata validators from emitted source")
        expect(
            all("__meta_" not in line for line in runtime_manifest_lines),
            "runtime-only XiangShan script mode should not emit metadata shard sources",
        )
    except Exception as ex:
        return fail(str(ex))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
