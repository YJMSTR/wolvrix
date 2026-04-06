from __future__ import annotations

import importlib.util
import os
import pathlib
import shutil
import subprocess
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
ARTIFACT_ROOT = REPO_ROOT / "build" / "artifacts" / "pybind_gsim"
BUILD_PYTHON_DIR = pathlib.Path(
    os.environ.get("WOLVRIX_PYTHON_BUILD_DIR", str(REPO_ROOT / "build" / "python"))
)


def fail(message: str) -> int:
    print(f"[pybind-gsim] {message}", file=sys.stderr)
    return 1


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


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
    native_module = getattr(module, "_native", None)
    expect(native_module is not None, "build-tree wolvrix should expose a native module")
    native_path = pathlib.Path(native_module.__file__).resolve()
    expect(str(candidate.resolve()).startswith(str(BUILD_PYTHON_DIR.resolve())),
           "build-tree wolvrix __init__ should live under the configured build dir")
    expect(str(native_path).startswith(str(BUILD_PYTHON_DIR.resolve())),
           "build-tree wolvrix native module should live under the configured build dir")
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


def make_chain_module_text(length: int) -> str:
    signals = "\n".join(f"    logic [7:0] s{i};" for i in range(length))
    assigns: list[str] = []
    for i in range(length):
        src = "a" if i == 0 else f"s{i - 1}"
        assigns.append(f"    assign s{i} = {src} + 8'd1;")
    assigns.append(f"    assign y = s{length - 1};")
    return (
        "module top(\n"
        "    input logic [7:0] a,\n"
        "    output logic [7:0] y\n"
        ");\n"
        f"{signals}\n\n"
        f"{chr(10).join(assigns)}\n"
        "endmodule\n"
    )

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


def test_port_order_alpha() -> None:
    root = ARTIFACT_ROOT / "port_order_alpha"
    source_dir = root / "src"
    out_dir = root / "out"
    reset_dir(source_dir)
    reset_dir(out_dir)

    design = create_design(source_dir)
    design.run_pipeline([["gsim", ["-path", "top"]]], print_diagnostics_level="off")
    base = out_dir / "alpha_test"
    wolvrix.write_gsim_cpp(design, str(base), top=["top"], port_order="alpha")
    header = base.with_suffix(".hpp").read_text(encoding="utf-8")
    pos_a = header.find("set_a(")
    pos_b = header.find("set_b(")
    pos_clk = header.find("set_clk(")
    expect(pos_a < pos_b < pos_clk, "alpha order should put a < b < clk")


def test_port_order_custom() -> None:
    root = ARTIFACT_ROOT / "port_order_custom"
    source_dir = root / "src"
    out_dir = root / "out"
    reset_dir(source_dir)
    reset_dir(out_dir)

    design = create_design(source_dir)
    design.run_pipeline([["gsim", ["-path", "top"]]], print_diagnostics_level="off")
    base = out_dir / "custom_test"
    wolvrix.write_gsim_cpp(design, str(base), top=["top"],
                           port_order="custom", port_order_names=["clk", "b", "a", "y"])
    header = base.with_suffix(".hpp").read_text(encoding="utf-8")
    pos_clk = header.find("set_clk(")
    pos_b = header.find("set_b(")
    pos_a = header.find("set_a(")
    expect(pos_clk < pos_b < pos_a, "custom order should put clk < b < a")


def test_port_order_invalid_strategy() -> None:
    root = ARTIFACT_ROOT / "port_order_invalid"
    source_dir = root / "src"
    out_dir = root / "out"
    reset_dir(source_dir)
    reset_dir(out_dir)

    design = create_design(source_dir)
    design.run_pipeline([["gsim", ["-path", "top"]]], print_diagnostics_level="off")
    base = out_dir / "invalid_test"
    try:
        wolvrix.write_gsim_cpp(design, str(base), top=["top"], port_order="bogus")
        raise RuntimeError("expected error for invalid strategy")
    except (ValueError, RuntimeError) as ex:
        expect("port_order must be one of" in str(ex),
               f"expected port_order error, got: {ex}")


def test_port_order_nonexistent_name() -> None:
    root = ARTIFACT_ROOT / "port_order_noname"
    source_dir = root / "src"
    out_dir = root / "out"
    reset_dir(source_dir)
    reset_dir(out_dir)

    design = create_design(source_dir)
    design.run_pipeline([["gsim", ["-path", "top"]]], print_diagnostics_level="off")
    base = out_dir / "noname_test"
    expect_runtime_error_contains(
        lambda: wolvrix.write_gsim_cpp(design, str(base), top=["top"],
                                       port_order="custom", port_order_names=["fake_port"]),
        "nonexistent port")


def test_port_order_duplicate_name() -> None:
    root = ARTIFACT_ROOT / "port_order_dup"
    source_dir = root / "src"
    out_dir = root / "out"
    reset_dir(source_dir)
    reset_dir(out_dir)

    design = create_design(source_dir)
    design.run_pipeline([["gsim", ["-path", "top"]]], print_diagnostics_level="off")
    base = out_dir / "dup_test"
    expect_runtime_error_contains(
        lambda: wolvrix.write_gsim_cpp(design, str(base), top=["top"],
                                       port_order="custom", port_order_names=["clk", "b", "b", "a"]),
        "duplicate port")


def test_emit_attributes_forward_behavior_shard_cap() -> None:
    root = ARTIFACT_ROOT / "emit_attributes_behavior_shards"
    source_dir = root / "src"
    out_dir = root / "out"
    reset_dir(source_dir)
    reset_dir(out_dir)

    design = create_design(source_dir, module_text=make_chain_module_text(128))
    design.run_pipeline([["gsim", ["-path", "top"]]], print_diagnostics_level="off")
    base = out_dir / "chain_shards"
    wolvrix.write_gsim_cpp(
        design,
        str(base),
        top=["top"],
        emit_attributes={"behavior_shard_max_bytes": "256"},
    )

    manifest_lines = [
        line.strip()
        for line in base.with_suffix(".manifest").read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    step_paths = [out_dir / line for line in manifest_lines if "__step_" in line]
    expect(step_paths, "emit_attributes should allow Python callers to request behavior shards")
    for step_path in step_paths:
        expect(step_path.stat().st_size <= 256,
               f"behavior shard should respect forwarded byte cap: {step_path}")


def test_read_sv_frontend_failure_respects_quiet_mode() -> None:
    root = ARTIFACT_ROOT / "frontend_failure_quiet"
    source_dir = root / "src"
    reset_dir(source_dir)

    broken_sv = source_dir / "broken.sv"
    broken_sv.write_text("module top(input logic a output logic y); endmodule\n", encoding="utf-8")

    probe = f"""
import importlib.util
import pathlib
import sys

candidate = pathlib.Path({str((BUILD_PYTHON_DIR / 'wolvrix' / '__init__.py'))!r})
spec = importlib.util.spec_from_file_location("wolvrix", candidate, submodule_search_locations=[str(candidate.parent)])
if spec is None or spec.loader is None:
    raise RuntimeError(f"failed to load wolvrix module spec from {{candidate}}")
module = importlib.util.module_from_spec(spec)
sys.modules["wolvrix"] = module
spec.loader.exec_module(module)
design, diagnostics = module.read_sv(
    {str(broken_sv)!r},
    print_diagnostics_level="off",
    raise_diagnostics_level="off",
)
print("design_is_none", design is None)
print("diag_count", len(diagnostics))
"""

    result = subprocess.run(
        [sys.executable, "-S", "-c", probe],
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
        check=False,
    )
    expect(result.returncode == 0, f"quiet frontend probe should succeed: {result.stderr or result.stdout}")
    expect("design_is_none True" in result.stdout, f"broken source should not produce a design: {result.stdout}")
    expect("diag_count" in result.stdout, f"probe should report diagnostic count: {result.stdout}")
    expect(result.stderr.strip() == "", f"quiet frontend failure should not print raw Slang diagnostics: {result.stderr!r}")


def main() -> int:
    try:
        test_same_design_pipeline_flow()
        test_failure_without_prior_gsim_metadata()
        test_failure_after_json_roundtrip()
        test_cross_root_target_paths_stay_distinct()
        test_port_order_alpha()
        test_port_order_custom()
        test_port_order_invalid_strategy()
        test_port_order_nonexistent_name()
        test_port_order_duplicate_name()
        test_emit_attributes_forward_behavior_shard_cap()
        test_read_sv_frontend_failure_respects_quiet_mode()
    except Exception as ex:
        return fail(str(ex))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
