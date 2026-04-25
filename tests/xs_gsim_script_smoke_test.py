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
        resume_out_base = out_dir / "xs_fixture_gsim_resume"
        checkpoint_path = out_dir / "xs_fixture_post_pipeline.json"

        sv_path.write_text(MODULE_TEXT, encoding="utf-8")
        filelist_path.write_text(f"{sv_path}\n", encoding="utf-8")
        read_args_path.write_text("\n", encoding="utf-8")

        env = dict(os.environ)
        env["PYTHONNOUSERSITE"] = "1"
        env["WOLVRIX_PYTHON_BUILD_DIR"] = str(REPO_ROOT / "wolvrix" / "build" / "python")
        env["PYTHONPATH"] = str(REPO_ROOT / "wolvrix" / "build" / "python")
        env["WOLVRIX_XS_GSIM_EMIT_METADATA"] = "1"
        env["WOLVRIX_XS_GSIM_POST_PIPELINE_JSON"] = str(checkpoint_path)
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
        manifest = out_base.with_suffix(".manifest")
        expect(header.exists(), f"missing header artifact: {header}")
        expect(source.exists(), f"missing source artifact: {source}")
        expect(manifest.exists(), f"missing manifest artifact: {manifest}")
        expect(checkpoint_path.exists(), f"missing post-pipeline checkpoint: {checkpoint_path}")
        expect(
            checkpoint_path.with_name(f"{checkpoint_path.name}.metadata.json").exists(),
            "missing checkpoint metadata sidecar",
        )

        header_text = header.read_text(encoding="utf-8")
        source_text = source.read_text(encoding="utf-8")
        manifest_text = manifest.read_text(encoding="utf-8")
        expect('class SSimTop' in header_text, 'missing downstream simulator-facing SSimTop API')
        expect('void set_reset(unsigned reset)' in header_text, 'missing set_reset API')
        expect('void step()' in header_text, 'missing step API')
        expect('get_difftest__DOT__exit()' in header_text, 'missing difftest exit accessor')
        expect('metadata.graph_symbol = "SimTop";' in source_text, "missing graph symbol metadata")
        expect('metadata.scratchpad_namespace = "gsim.SimTop";' in source_text, "missing scratchpad namespace metadata")
        expect("xs_fixture_gsim.cpp" in manifest_text, "manifest should list canonical source")
        expect("emit attrs" in result.stderr, "script should report emit attribute passthrough in logs")
        expect("write_post_pipeline_json done" in result.stderr, "script should report checkpoint write timing")
        expect("artifact stats files=" in result.stderr, "script should report artifact file/byte stats")

        resume_env = dict(env)
        resume_env["WOLVRIX_XS_GSIM_RESUME_FROM_POST_PIPELINE_JSON"] = "1"
        resume_env["WOLVRIX_XS_GSIM_EMIT_REPETITIONS"] = "2"
        resume_result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                str(filelist_path),
                "SimTop",
                str(resume_out_base),
                str(read_args_path),
                "info",
            ],
            cwd=str(REPO_ROOT),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=resume_env,
            check=False,
        )
        expect(
            resume_result.returncode == 0,
            f"resume script failed: {resume_result.stderr.strip() or resume_result.stdout.strip()}",
        )
        expect(resume_out_base.with_suffix(".hpp").exists(), "resume mode should emit header artifact")
        expect(resume_out_base.with_suffix(".cpp").exists(), "resume mode should emit source artifact")
        expect("read_json start" in resume_result.stderr, "resume mode should load the checkpoint JSON")
        expect(
            "resume note: rebuilding gsim metadata because scratchpad metadata is not JSON-persisted"
            in resume_result.stderr,
            "resume mode should explain metadata rebuild semantics",
        )
        expect(
            "write_gsim_cpp repeat 1/2 done" in resume_result.stderr,
            "resume mode should support repeated emit timing",
        )
        expect(
            "write_gsim_cpp repeat 2/2 done" in resume_result.stderr,
            "resume mode should support repeated emit timing",
        )
    except Exception as ex:
        return fail(str(ex))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
