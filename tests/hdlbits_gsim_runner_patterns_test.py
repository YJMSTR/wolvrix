from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
ARTIFACT_ROOT = REPO_ROOT / "build" / "artifacts" / "hdlbits_gsim_runner_patterns"
SCRIPT_PATH = REPO_ROOT / "scripts" / "wolvrix_hdlbits_gsim_run.py"
BUILD_PYTHON_DIR = pathlib.Path(
    os.environ.get("WOLVRIX_PYTHON_BUILD_DIR", str(REPO_ROOT / "wolvrix" / "build" / "python"))
)


def fail(message: str) -> int:
    print(f"[hdlbits-gsim-runner-patterns] {message}", file=sys.stderr)
    return 1


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def reset_dir(path: pathlib.Path) -> None:
    shutil.rmtree(path, ignore_errors=True)
    path.mkdir(parents=True, exist_ok=True)


def run_dut(
    dut_id: str,
    expect_tb_parity: bool = False,
    expect_no_tb_parity: bool = False,
) -> pathlib.Path:
    env = os.environ.copy()
    env["PYTHONNOUSERSITE"] = "1"
    env["WOLVRIX_PYTHON_BUILD_DIR"] = str(BUILD_PYTHON_DIR)
    env["PYTHONPATH"] = str(BUILD_PYTHON_DIR)
    dut_dir = ARTIFACT_ROOT / dut_id
    dut_dir.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [
            sys.executable,
            "-S",
            str(SCRIPT_PATH),
            "--dut",
            dut_id,
            "--out-dir",
            str(dut_dir),
        ],
        capture_output=True,
        text=True,
        env=env,
        check=False,
    )
    expect(
        result.returncode == 0,
        f"dut_{dut_id} runtime failed\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}",
    )
    expect((dut_dir / f"dut_{dut_id}_runner").exists(), f"dut_{dut_id} missing runtime binary")
    if expect_tb_parity:
        expect((dut_dir / "verilated.h").exists(), f"dut_{dut_id} missing verilated.h parity shim")
        expect((dut_dir / "verilated_cov.h").exists(), f"dut_{dut_id} missing verilated_cov.h parity shim")
        expect((dut_dir / f"Vdut_{dut_id}.h").exists(), f"dut_{dut_id} missing Verilator-compatible wrapper")
    if expect_no_tb_parity:
        expect(not (dut_dir / "verilated.h").exists(), f"dut_{dut_id} unexpectedly used TB parity shim")
        expect(not (dut_dir / "verilated_cov.h").exists(), f"dut_{dut_id} unexpectedly used TB parity cov shim")
        expect(not (dut_dir / f"Vdut_{dut_id}.h").exists(), f"dut_{dut_id} unexpectedly used Verilator-compatible wrapper")
    return dut_dir


def main() -> int:
    try:
        reset_dir(ARTIFACT_ROOT)
        expect(SCRIPT_PATH.exists(), f"missing runtime helper: {SCRIPT_PATH}")
        for dut_id in (
            "001",
            "002",
            "004",
            "005",
            "062",
            "064",
            "065",
            "008",
            "011",
            "014",
            "018",
            "021",
            "023",
            "024",
            "025",
            "029",
            "031",
            "033",
            "038",
            "030",
            "010",
            "013",
            "016",
            "034",
            "035",
            "036",
            "039",
            "049",
            "050",
            "051",
            "052",
            "044",
            "045",
            "046",
            "047",
            "048",
            "053",
            "054",
            "055",
            "056",
            "057",
            "058",
            "059",
            "060",
            "061",
            "063",
            "066",
            "067",
            "068",
            "069",
            "070",
            "072",
            "073",
            "074",
            "075",
            "076",
            "077",
            "078",
            "079",
            "080",
            "081",
            "082",
            "083",
            "084",
            "085",
            "086",
            "087",
            "088",
            "089",
            "090",
            "091",
            "092",
            "093",
            "094",
            "097",
            "099",
            "100",
            "101",
            "102",
            "103",
            "104",
            "105",
            "106",
            "107",
            "110",
            "111",
            "112",
            "113",
            "114",
            "115",
            "116",
            "117",
            "119",
            "120",
            "121",
            "122",
            "123",
            "124",
            "127",
            "128",
            "129",
            "132",
            "133",
            "134",
            "140",
            "142",
            "145",
            "146",
            "147",
            "148",
            "149",
            "150",
            "152",
            "153",
            "158",
            "159",
            "160",
            "161",
            "096",
            "098",
        ):
            run_dut(dut_id)
        run_dut("001", expect_tb_parity=True)
        run_dut("084", expect_tb_parity=True)
        run_dut("085", expect_tb_parity=True)
        run_dut("088", expect_tb_parity=True)
        run_dut("098", expect_tb_parity=True)
        run_dut("111", expect_tb_parity=True)
        run_dut("114", expect_tb_parity=True)
        run_dut("030", expect_no_tb_parity=True)
        run_dut("060", expect_tb_parity=True)
        run_dut("095", expect_tb_parity=True)
        run_dut("106", expect_tb_parity=True)
        run_dut("116", expect_tb_parity=True)
        run_dut("118", expect_tb_parity=True)

        wrapper_095 = (run_dut("095", expect_tb_parity=True) / "Vdut_095.h").read_text(encoding="utf-8")
        expect(
            "sim_.settle();" in wrapper_095 and "sim_.commit_step();" in wrapper_095,
            "TB parity wrappers should route their eval flow through the fine-grained settle/commit_step API instead of calling step() directly",
        )

        runner_030 = (run_dut("030", expect_no_tb_parity=True) / "dut_030_runner.cpp").read_text(encoding="utf-8")
        expect(
            "comb and ff XOR outputs correct across edges" in runner_030,
            "dut_030 runner should mirror the original TB's combined combinational + ff XOR workload instead of the weaker generic fallback",
        )

        runner_093 = (run_dut("093", expect_no_tb_parity=True) / "dut_093_runner.cpp").read_text(encoding="utf-8")
        expect(
            "3-bit state machine output" in runner_093 and "q0" in runner_093 and "q1" in runner_093 and "q2" in runner_093,
            "dut_093 runner should mirror the original TB's full 3-bit state-machine sequence instead of the reduced generic fallback",
        )

        runner_115 = (run_dut("115", expect_no_tb_parity=True) / "dut_115_runner.cpp").read_text(encoding="utf-8")
        expect(
            "serial load + muxed readout" in runner_115 and "check_Z" in runner_115,
            "dut_115 runner should mirror the original TB's serial-load plus 8-way mux readout checks instead of the reduced generic fallback",
        )

        runner_162 = (run_dut("162", expect_no_tb_parity=True) / "dut_162_runner.cpp").read_text(encoding="utf-8")
        expect(
            "for (uint8_t idx = 0; idx < 128U; ++idx)" in runner_162 and "passed all prediction and training scenarios" in runner_162,
            "dut_162 runner should preserve the original TB's full PHT sweep and prediction/training workload instead of the reduced fallback sequence",
        )

        runner_141 = (run_dut("141", expect_no_tb_parity=True) / "dut_141_runner.cpp").read_text(encoding="utf-8")
        expect(
            "simple 2-state FSM with z behavior" in runner_141 and 'const std::array<std::uint8_t, 6> sequence' in runner_141,
            "dut_141 runner should preserve the original TB's reset-and-sequence FSM workload instead of a weaker fallback",
        )
    except Exception as ex:
        return fail(str(ex))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
