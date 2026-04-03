from __future__ import annotations

import json
import os
import pathlib
import shutil
import subprocess
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
ARTIFACT_ROOT = REPO_ROOT / "build" / "artifacts" / "hdlbits_gsim_batch_runtime_test"
SCRIPT_PATH = REPO_ROOT / "scripts" / "wolvrix_hdlbits_gsim_batch.py"
BUILD_PYTHON_DIR = pathlib.Path(
    os.environ.get("WOLVRIX_PYTHON_BUILD_DIR", str(REPO_ROOT / "wolvrix" / "build" / "python"))
)


def fail(message: str) -> int:
    print(f"[hdlbits-gsim-batch-runtime] {message}", file=sys.stderr)
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
        env = os.environ.copy()
        env["PYTHONNOUSERSITE"] = "1"
        env["WOLVRIX_PYTHON_BUILD_DIR"] = str(BUILD_PYTHON_DIR)
        env["PYTHONPATH"] = str(BUILD_PYTHON_DIR)

        result = subprocess.run(
            [
                sys.executable,
                "-S",
                str(SCRIPT_PATH),
                "--output-dir",
                str(ARTIFACT_ROOT),
                "--dut-ids",
                "001,004,005,010,011,013,014,016,021,023,024,025,029,030,031,033,034,035,036,038,039,044,045,046,047,048,049,050,051,052,053,054,055,056,057,058,059,061,062,063,064,065,066,067,068,069,070,072,073,074,075,076,077,078,079,080,081,082,083,084,085,086,087,088,089,090,091,092,093,094,096,097,099,100,101,102,103,104,107,110,111,112,113,115,119,120,121,122,123,124,127,128,129,132,133,134,140,145,146,147,148,149,150,152,153,158,159,160,161",
                "--execution-mode",
                "runtime",
                "--timeout",
                "60",
                "--memory-limit-mb",
                "32768",
            ],
            capture_output=True,
            text=True,
            env=env,
            check=False,
        )
        expect(
            result.returncode == 0,
            "batch runtime harness failed\nstdout:\n"
            + (result.stdout or "")
            + "\nstderr:\n"
            + (result.stderr or ""),
        )

        report_path = ARTIFACT_ROOT / "batch_report.json"
        expect(report_path.exists(), "missing batch runtime report")
        report = json.loads(report_path.read_text(encoding="utf-8"))
        summary = report["summary"]
        expect(summary["total_duts"] == 109, f"expected 109 DUTs, got {summary['total_duts']}")
        expect(summary["success"] == 109, f"expected 109 successes, got {summary['success']}")
        expect(summary["tool_failure"] == 0, f"expected no tool failures, got {summary['tool_failure']}")
        results = {entry["dut_id"]: entry["result"] for entry in report["dut_results"]}
        expect(results.get("001") == "success", f"unexpected dut_001 result: {results.get('001')}")
        expect(results.get("004") == "success", f"unexpected dut_004 result: {results.get('004')}")
        expect(results.get("005") == "success", f"unexpected dut_005 result: {results.get('005')}")
        expect(results.get("010") == "success", f"unexpected dut_010 result: {results.get('010')}")
        expect(results.get("011") == "success", f"unexpected dut_011 result: {results.get('011')}")
        expect(results.get("013") == "success", f"unexpected dut_013 result: {results.get('013')}")
        expect(results.get("014") == "success", f"unexpected dut_014 result: {results.get('014')}")
        expect(results.get("016") == "success", f"unexpected dut_016 result: {results.get('016')}")
        expect(results.get("021") == "success", f"unexpected dut_021 result: {results.get('021')}")
        expect(results.get("023") == "success", f"unexpected dut_023 result: {results.get('023')}")
        expect(results.get("024") == "success", f"unexpected dut_024 result: {results.get('024')}")
        expect(results.get("025") == "success", f"unexpected dut_025 result: {results.get('025')}")
        expect(results.get("029") == "success", f"unexpected dut_029 result: {results.get('029')}")
        expect(results.get("030") == "success", f"unexpected dut_030 result: {results.get('030')}")
        expect(results.get("031") == "success", f"unexpected dut_031 result: {results.get('031')}")
        expect(results.get("033") == "success", f"unexpected dut_033 result: {results.get('033')}")
        expect(results.get("034") == "success", f"unexpected dut_034 result: {results.get('034')}")
        expect(results.get("035") == "success", f"unexpected dut_035 result: {results.get('035')}")
        expect(results.get("036") == "success", f"unexpected dut_036 result: {results.get('036')}")
        expect(results.get("038") == "success", f"unexpected dut_038 result: {results.get('038')}")
        expect(results.get("039") == "success", f"unexpected dut_039 result: {results.get('039')}")
        expect(results.get("044") == "success", f"unexpected dut_044 result: {results.get('044')}")
        expect(results.get("045") == "success", f"unexpected dut_045 result: {results.get('045')}")
        expect(results.get("046") == "success", f"unexpected dut_046 result: {results.get('046')}")
        expect(results.get("047") == "success", f"unexpected dut_047 result: {results.get('047')}")
        expect(results.get("048") == "success", f"unexpected dut_048 result: {results.get('048')}")
        expect(results.get("049") == "success", f"unexpected dut_049 result: {results.get('049')}")
        expect(results.get("050") == "success", f"unexpected dut_050 result: {results.get('050')}")
        expect(results.get("051") == "success", f"unexpected dut_051 result: {results.get('051')}")
        expect(results.get("052") == "success", f"unexpected dut_052 result: {results.get('052')}")
        expect(results.get("053") == "success", f"unexpected dut_053 result: {results.get('053')}")
        expect(results.get("054") == "success", f"unexpected dut_054 result: {results.get('054')}")
        expect(results.get("055") == "success", f"unexpected dut_055 result: {results.get('055')}")
        expect(results.get("056") == "success", f"unexpected dut_056 result: {results.get('056')}")
        expect(results.get("057") == "success", f"unexpected dut_057 result: {results.get('057')}")
        expect(results.get("058") == "success", f"unexpected dut_058 result: {results.get('058')}")
        expect(results.get("059") == "success", f"unexpected dut_059 result: {results.get('059')}")
        expect(results.get("061") == "success", f"unexpected dut_061 result: {results.get('061')}")
        expect(results.get("062") == "success", f"unexpected dut_062 result: {results.get('062')}")
        expect(results.get("063") == "success", f"unexpected dut_063 result: {results.get('063')}")
        expect(results.get("064") == "success", f"unexpected dut_064 result: {results.get('064')}")
        expect(results.get("065") == "success", f"unexpected dut_065 result: {results.get('065')}")
        expect(results.get("066") == "success", f"unexpected dut_066 result: {results.get('066')}")
        expect(results.get("067") == "success", f"unexpected dut_067 result: {results.get('067')}")
        expect(results.get("068") == "success", f"unexpected dut_068 result: {results.get('068')}")
        expect(results.get("069") == "success", f"unexpected dut_069 result: {results.get('069')}")
        expect(results.get("070") == "success", f"unexpected dut_070 result: {results.get('070')}")
        expect(results.get("072") == "success", f"unexpected dut_072 result: {results.get('072')}")
        expect(results.get("073") == "success", f"unexpected dut_073 result: {results.get('073')}")
        expect(results.get("074") == "success", f"unexpected dut_074 result: {results.get('074')}")
        expect(results.get("075") == "success", f"unexpected dut_075 result: {results.get('075')}")
        expect(results.get("076") == "success", f"unexpected dut_076 result: {results.get('076')}")
        expect(results.get("077") == "success", f"unexpected dut_077 result: {results.get('077')}")
        expect(results.get("078") == "success", f"unexpected dut_078 result: {results.get('078')}")
        expect(results.get("079") == "success", f"unexpected dut_079 result: {results.get('079')}")
        expect(results.get("080") == "success", f"unexpected dut_080 result: {results.get('080')}")
        expect(results.get("081") == "success", f"unexpected dut_081 result: {results.get('081')}")
        expect(results.get("082") == "success", f"unexpected dut_082 result: {results.get('082')}")
        expect(results.get("083") == "success", f"unexpected dut_083 result: {results.get('083')}")
        expect(results.get("084") == "success", f"unexpected dut_084 result: {results.get('084')}")
        expect(results.get("085") == "success", f"unexpected dut_085 result: {results.get('085')}")
        expect(results.get("086") == "success", f"unexpected dut_086 result: {results.get('086')}")
        expect(results.get("087") == "success", f"unexpected dut_087 result: {results.get('087')}")
        expect(results.get("088") == "success", f"unexpected dut_088 result: {results.get('088')}")
        expect(results.get("089") == "success", f"unexpected dut_089 result: {results.get('089')}")
        expect(results.get("090") == "success", f"unexpected dut_090 result: {results.get('090')}")
        expect(results.get("091") == "success", f"unexpected dut_091 result: {results.get('091')}")
        expect(results.get("092") == "success", f"unexpected dut_092 result: {results.get('092')}")
        expect(results.get("093") == "success", f"unexpected dut_093 result: {results.get('093')}")
        expect(results.get("094") == "success", f"unexpected dut_094 result: {results.get('094')}")
        expect(results.get("096") == "success", f"unexpected dut_096 result: {results.get('096')}")
        expect(results.get("097") == "success", f"unexpected dut_097 result: {results.get('097')}")
        expect(results.get("099") == "success", f"unexpected dut_099 result: {results.get('099')}")
        expect(results.get("100") == "success", f"unexpected dut_100 result: {results.get('100')}")
        expect(results.get("101") == "success", f"unexpected dut_101 result: {results.get('101')}")
        expect(results.get("102") == "success", f"unexpected dut_102 result: {results.get('102')}")
        expect(results.get("103") == "success", f"unexpected dut_103 result: {results.get('103')}")
        expect(results.get("104") == "success", f"unexpected dut_104 result: {results.get('104')}")
        expect(results.get("107") == "success", f"unexpected dut_107 result: {results.get('107')}")
        expect(results.get("110") == "success", f"unexpected dut_110 result: {results.get('110')}")
        expect(results.get("111") == "success", f"unexpected dut_111 result: {results.get('111')}")
        expect(results.get("112") == "success", f"unexpected dut_112 result: {results.get('112')}")
        expect(results.get("113") == "success", f"unexpected dut_113 result: {results.get('113')}")
        expect(results.get("115") == "success", f"unexpected dut_115 result: {results.get('115')}")
        expect(results.get("119") == "success", f"unexpected dut_119 result: {results.get('119')}")
        expect(results.get("120") == "success", f"unexpected dut_120 result: {results.get('120')}")
        expect(results.get("121") == "success", f"unexpected dut_121 result: {results.get('121')}")
        expect(results.get("122") == "success", f"unexpected dut_122 result: {results.get('122')}")
        expect(results.get("123") == "success", f"unexpected dut_123 result: {results.get('123')}")
        expect(results.get("124") == "success", f"unexpected dut_124 result: {results.get('124')}")
        expect(results.get("127") == "success", f"unexpected dut_127 result: {results.get('127')}")
        expect(results.get("128") == "success", f"unexpected dut_128 result: {results.get('128')}")
        expect(results.get("129") == "success", f"unexpected dut_129 result: {results.get('129')}")
        expect(results.get("132") == "success", f"unexpected dut_132 result: {results.get('132')}")
        expect(results.get("133") == "success", f"unexpected dut_133 result: {results.get('133')}")
        expect(results.get("134") == "success", f"unexpected dut_134 result: {results.get('134')}")
        expect(results.get("140") == "success", f"unexpected dut_140 result: {results.get('140')}")
        expect(results.get("145") == "success", f"unexpected dut_145 result: {results.get('145')}")
        expect(results.get("146") == "success", f"unexpected dut_146 result: {results.get('146')}")
        expect(results.get("147") == "success", f"unexpected dut_147 result: {results.get('147')}")
        expect(results.get("148") == "success", f"unexpected dut_148 result: {results.get('148')}")
        expect(results.get("149") == "success", f"unexpected dut_149 result: {results.get('149')}")
        expect(results.get("150") == "success", f"unexpected dut_150 result: {results.get('150')}")
        expect(results.get("152") == "success", f"unexpected dut_152 result: {results.get('152')}")
        expect(results.get("153") == "success", f"unexpected dut_153 result: {results.get('153')}")
        expect(results.get("158") == "success", f"unexpected dut_158 result: {results.get('158')}")
        expect(results.get("159") == "success", f"unexpected dut_159 result: {results.get('159')}")
        expect(results.get("160") == "success", f"unexpected dut_160 result: {results.get('160')}")
        expect(results.get("161") == "success", f"unexpected dut_161 result: {results.get('161')}")

        fail_env = env.copy()
        fail_env["WOLVRIX_GSIM_RUNTIME_FORCE_FAIL"] = "1"
        fail_out = ARTIFACT_ROOT / "forced_fail"
        fail_out.mkdir(parents=True, exist_ok=True)
        fail_result = subprocess.run(
            [
                sys.executable,
                "-S",
                str(SCRIPT_PATH),
                "--output-dir",
                str(fail_out),
                "--dut-ids",
                "001",
                "--execution-mode",
                "runtime",
                "--timeout",
                "60",
                "--memory-limit-mb",
                "32768",
            ],
            capture_output=True,
            text=True,
            env=fail_env,
            check=False,
        )
        expect(fail_result.returncode != 0, "forced runtime failure should fail the batch harness")
        fail_report = json.loads((fail_out / "batch_report.json").read_text(encoding="utf-8"))
        fail_entry = fail_report["dut_results"][0]
        expect(fail_entry["result"] == "sim-failure", f"expected sim-failure, got {fail_entry['result']}")

        mem_out = ARTIFACT_ROOT / "tiny_mem"
        mem_out.mkdir(parents=True, exist_ok=True)
        mem_result = subprocess.run(
            [
                sys.executable,
                "-S",
                str(SCRIPT_PATH),
                "--output-dir",
                str(mem_out),
                "--dut-ids",
                "001",
                "--execution-mode",
                "runtime",
                "--timeout",
                "60",
                "--memory-limit-mb",
                "1",
            ],
            capture_output=True,
            text=True,
            env=env,
            check=False,
        )
        expect(mem_result.returncode != 0, "tiny-memory runtime batch should not succeed")
        mem_report = json.loads((mem_out / "batch_report.json").read_text(encoding="utf-8"))
        mem_entry = mem_report["dut_results"][0]
        expect(mem_entry["result"] == "tool-failure", f"expected tool-failure under tiny memory, got {mem_entry['result']}")
    except Exception as ex:
        return fail(str(ex))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
