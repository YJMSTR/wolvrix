from __future__ import annotations

import importlib.util
import os
import pathlib
import shutil
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
ARTIFACT_ROOT = REPO_ROOT / "build" / "artifacts" / "pybind_gsim"
BUILD_PYTHON_DIR = REPO_ROOT / "build" / "python"


def load_wolvrix_from_build() -> object:
    candidate = BUILD_PYTHON_DIR / "wolvrix" / "__init__.py"
    native = BUILD_PYTHON_DIR / "wolvrix" / "_wolvrix.so"
    if not candidate.exists() or not native.exists():
        raise RuntimeError(
            f"could not locate build-tree wolvrix package at {candidate} / {native}"
        )
    spec = importlib.util.spec_from_file_location(
        "wolvrix", candidate, submodule_search_locations=[str(candidate.parent)]
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load wolvrix module spec from {candidate}")
    module = importlib.util.module_from_spec(spec)
    sys.modules.pop("wolvrix", None)
    sys.modules["wolvrix"] = module
    spec.loader.exec_module(module)
    return module


wolvrix = load_wolvrix_from_build()


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

DUAL_ROOT_MODULE_TEXT = """module leaf(
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

module top0(
    input logic [7:0] a,
    input logic [7:0] b,
    input logic clk,
    output logic [7:0] y
);
    leaf u_leaf(.a(a), .b(b), .clk(clk), .y(y));
endmodule

module top1(
    input logic [7:0] a,
    input logic [7:0] b,
    input logic clk,
    output logic [7:0] y
);
    leaf u_leaf(.a(a), .b(b), .clk(clk), .y(y));
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


def create_design(source_dir: pathlib.Path, module_text: str = MODULE_TEXT, top: str | None = None) -> wolvrix.Design:
    sv_path = source_dir / "top.sv"
    sv_path.write_text(module_text, encoding="utf-8")
    slang_args = ["--top", top] if top else None
    design, diagnostics = wolvrix.read_sv(str(sv_path), slang_args=slang_args, print_diagnostics_level="off")
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

    expect_runtime_error_contains(
        lambda: design.write_gsim_cpp(str(out_dir / "bad_path"), top=["top"], target_path="top..bad"),
        "failed to resolve gsim emit target path",
    )

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


def test_cross_root_target_paths_stay_distinct() -> None:
    root = ARTIFACT_ROOT / "cross_root"
    source_dir = root / "src"
    out_dir = root / "out"
    reset_dir(source_dir)
    reset_dir(out_dir)

    design = create_design(source_dir, module_text=DUAL_ROOT_MODULE_TEXT)
    changed0, diags0 = design.run_pipeline([["gsim", ["-path", "top0.u_leaf"]]], print_diagnostics_level="off")
    changed1, diags1 = design.run_pipeline([["gsim", ["-path", "top1.u_leaf"]]], print_diagnostics_level="off")
    expect(not changed0 and not changed1, "gsim should stay scratchpad-only for cross-root fixture")
    expect(not diags0 and not diags1, "cross-root gsim fixture should not emit diagnostics")

    top0_base = out_dir / "top0_leaf"
    top1_base = out_dir / "top1_leaf"
    design.write_gsim_cpp(str(top0_base), top=["top0", "top1"], target_path="top0.u_leaf")
    design.write_gsim_cpp(str(top1_base), top=["top0", "top1"], target_path="top1.u_leaf")

    top0_source = top0_base.with_suffix(".cpp").read_text(encoding="utf-8")
    top1_source = top1_base.with_suffix(".cpp").read_text(encoding="utf-8")
    expect('metadata.scratchpad_namespace = "gsim.leaf.path.top0$u_leaf";' in top0_source,
           "top0 cross-root emit should preserve its root-qualified namespace")
    expect('metadata.scratchpad_namespace = "gsim.leaf.path.top1$u_leaf";' in top1_source,
           "top1 cross-root emit should preserve its root-qualified namespace")


def main() -> int:
    try:
        test_same_design_pipeline_flow()
        test_failure_without_prior_gsim_metadata()
        test_failure_after_json_roundtrip()
        test_cross_root_target_paths_stay_distinct()
    except Exception as ex:
        return fail(str(ex))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
