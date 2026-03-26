from __future__ import annotations

import pathlib
import shutil
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
ARTIFACT_ROOT = REPO_ROOT / "build" / "artifacts" / "pybind_gsim"

import wolvrix


MODULE_TEXT = """module top(
    input logic [7:0] a,
    input logic [7:0] b,
    input logic clk,
    output logic [7:0] y
);
    logic [7:0] state;

    always_ff @(posedge clk) begin
        state <= a + b;
    end

    assign y = state;
endmodule
"""


def fail(message: str) -> int:
    print(f"[pybind-gsim] {message}", file=sys.stderr)
    return 1


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def reset_dir(path: pathlib.Path) -> None:
    shutil.rmtree(path, ignore_errors=True)
    path.mkdir(parents=True, exist_ok=True)


def create_design(source_dir: pathlib.Path) -> wolvrix.Design:
    sv_path = source_dir / "top.sv"
    sv_path.write_text(MODULE_TEXT, encoding="utf-8")
    design, diagnostics = wolvrix.read_sv(str(sv_path), print_diagnostics_level="off")
    expect(design is not None, "read_sv should return a Design for the fixture")
    expect(not diagnostics, "fixture should parse without diagnostics")
    return design


def expect_runtime_error_contains(fn, needle: str) -> None:
    try:
        fn()
    except RuntimeError as ex:
        text = str(ex)
        expect(needle in text, f"expected RuntimeError containing '{needle}', got: {text}")
        return
    raise RuntimeError(f"expected RuntimeError containing '{needle}'")


def test_same_design_pipeline_flow() -> None:
    root = ARTIFACT_ROOT / "same_design"
    source_dir = root / "src"
    out_dir = root / "out"
    reset_dir(source_dir)
    reset_dir(out_dir)

    design = create_design(source_dir)
    changed, diagnostics = design.run_pipeline(
        [["gsim", ["-path", "top"]]],
        print_diagnostics_level="off",
    )
    expect(not changed, "gsim should remain scratchpad-only from Python")
    expect(not diagnostics, "gsim Python pipeline should not emit diagnostics on fixture")

    base = out_dir / "same_design_metadata"
    wolvrix.write_gsim_cpp(design, str(base), top=["top"])

    header = base.with_suffix(".hpp")
    source = base.with_suffix(".cpp")
    expect(header.exists(), "write_gsim_cpp should create header artifact")
    expect(source.exists(), "write_gsim_cpp should create source artifact")
    source_text = source.read_text(encoding="utf-8")
    expect('metadata.graph_symbol = "top";' in source_text, "source should embed graph metadata")
    expect('metadata.scratchpad_namespace = "gsim.top";' in source_text, "source should preserve scratchpad namespace")

    dryrun_base = out_dir / "dryrun_metadata"
    dryrun_design = wolvrix.from_json_string(design.to_json())
    expect_runtime_error_contains(
        lambda: dryrun_design.write_gsim_cpp(str(dryrun_base), top=["top"]),
        "missing required gsim scratchpad metadata",
    )


def test_failure_without_prior_gsim_metadata() -> None:
    root = ARTIFACT_ROOT / "missing_metadata"
    source_dir = root / "src"
    out_dir = root / "out"
    reset_dir(source_dir)
    reset_dir(out_dir)

    design = create_design(source_dir)
    target = out_dir / "missing_metadata"
    expect_runtime_error_contains(
        lambda: design.write_gsim_cpp(str(target), top=["top"]),
        "missing required gsim scratchpad metadata",
    )


def test_failure_after_json_roundtrip() -> None:
    root = ARTIFACT_ROOT / "json_roundtrip"
    source_dir = root / "src"
    out_dir = root / "out"
    reset_dir(source_dir)
    reset_dir(out_dir)

    design = create_design(source_dir)
    design.run_pipeline([["gsim", ["-path", "top"]]], print_diagnostics_level="off")
    roundtrip = wolvrix.from_json_string(design.to_json())
    target = out_dir / "roundtrip_metadata"
    expect_runtime_error_contains(
        lambda: roundtrip.write_gsim_cpp(str(target), top=["top"]),
        "missing required gsim scratchpad metadata",
    )


def main() -> int:
    try:
        test_same_design_pipeline_flow()
        test_failure_without_prior_gsim_metadata()
        test_failure_after_json_roundtrip()
    except Exception as ex:
        return fail(str(ex))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
