# HDLBits GSim Runtime Baseline

## Scope

This document records the HDLBits end-to-end runtime validation baseline for the `EmitGsimCpp` backend.

A DUT counts as **runtime-validated** only if it completes the full path below:

1. `read_sv`
2. normalize / simplify pipeline
3. `gsim`
4. `write_gsim_cpp`
5. compile the emitted model together with a checker runner using `-std=c++17 -Wall -Wextra -Werror`
6. execute the runner and check outputs

This is stricter than "code generation succeeded", but it is still not a Verilator difftest. The goal is to prove that the emitted C++ model is runnable and behaviorally checked through the public `reset()`, `step()`, `get_*()`, and `set_*()` interface.

## Latest Baseline

The branch-local runtime baseline is now complete for the full HDLBits suite:

- Report: `build/artifacts/hdlbits_gsim_runtime_full162/batch_report.json`
- Result: `162/162 success`
- Success rate: `100.0%`
- Average elapsed time: `1518.16 ms` per DUT
- Max elapsed time: `22019.92 ms` (`dut_118`)
- Total elapsed time: `245974.31 ms`

Fresh repo-local regression evidence was re-run after landing the runtime coverage expansion:

- `python3 -S wolvrix/tests/hdlbits_gsim_runtime_full_test.py`
  - report: `build/artifacts/hdlbits_gsim_runtime_full_test/batch_report.json`
  - result: `162/162 success`
  - average elapsed time: `1506.03 ms`
  - max elapsed time: `21377.75 ms`
- `python3 -S wolvrix/tests/hdlbits_gsim_runtime_remaining_test.py`
  - report: `build/artifacts/hdlbits_gsim_runtime_remaining_test/batch_report.json`
  - result: `28/28 success`
- `python3 -S wolvrix/tests/hdlbits_gsim_runner_patterns_test.py`
  - result: exit `0`
- `python3 -S wolvrix/tests/hdlbits_gsim_batch_runtime_test.py`
  - report: `build/artifacts/hdlbits_gsim_batch_runtime_test/batch_report.json`
  - result: `117/117 success`
  - also verifies forced `sim-failure` classification and tiny-memory failure handling

## Coverage Structure

The HDLBits runtime helper is no longer a small hand-written smoke set. It now covers all `162` DUTs through a mix of descriptor-driven and custom runner paths in [`scripts/wolvrix_hdlbits_gsim_run.py`](/home/zhangyangjie/corvusitor/wolvrix-playground/scripts/wolvrix_hdlbits_gsim_run.py):

- `COMB_DESCRIPTORS`
  - pure combinational probes with direct input/output checks
- `SEQ_DESCRIPTORS`
  - sequential cases where outputs are sampled from the current state interface
- `SEQ_LAG_DESCRIPTORS`
  - sequential cases whose observable outputs lag one `step()`
- `SEQ_OBSERVABLE_DESCRIPTORS`
  - sequential probes that validate edge/event observability
- `CUSTOM_RUNNERS`
  - `18` DUT-specific runners for the cases that need wide-value helpers, FSM-specific reference models, serial protocols, or intentionally different output-sampling rules

The custom runner set is:

- `040 041 042 043 060 071 098 105 106 108 114 116 117 118 139 141 142 162`

This split matters because HDLBits contains several cases that are too awkward to validate honestly with a single generic runner template. Examples include:

- very wide arithmetic and BCD cases: `041`, `042`, `043`
- very wide state or packed vectors: `060`, `116`, `117`, `118`
- protocol / FSM-specific reference models: `098`, `105`, `106`, `114`, `142`, `162`
- cases whose emitted observable behavior is latch-delayed relative to the original HDLBits testbench sampling point: `139`, `141`

## Historically Problematic DUTs

The earlier compile-only batch had five notable problem cases:

- `026`
- `041`
- `042`
- `043`
- `095`

All five now pass the runtime flow in the `162/162` run. That means the previous blocker is no longer the end-to-end simulation path itself. The remaining cost is mostly generated-C++ size plus compiler work, not `gsim` correctness.

## Compiler And Safety Rules

Two different compile paths now exist on purpose:

- compile-only batch validation still uses `g++ -fsyntax-only`
  - this keeps the "generated code must compile cleanly under GCC with `-Werror`" contract for emitted model artifacts
- runtime runner compilation prefers `clang++`, with `g++` as fallback
  - this was introduced because GCC was pathologically slow on some generated runtime runners, especially `dut_043`

The batch harness also now kills the full subprocess group on timeout instead of only terminating the parent helper process. This prevents orphaned `g++` / `cc1plus` compiler processes from surviving timeouts and exhausting host memory.

That timeout cleanup lives in [`scripts/wolvrix_hdlbits_gsim_batch.py`](/home/zhangyangjie/corvusitor/wolvrix-playground/scripts/wolvrix_hdlbits_gsim_batch.py).

## Performance Notes

The HDLBits runtime numbers are dominated by setup and compilation overhead:

- Python process startup
- `read_sv`
- normalization passes
- `gsim`
- `write_gsim_cpp`
- emitted-model compile
- runner compile
- runner execution

They do not isolate pure simulation-step throughput. For HDLBits-scale DUTs, compile cost is still a large part of the wall clock.

The slowest DUTs in `build/artifacts/hdlbits_gsim_runtime_full162/batch_report.json` were:

- `118`: `22019.92 ms`
- `043`: `13317.43 ms`
- `041`: `4988.32 ms`
- `042`: `2979.31 ms`
- `104`: `2144.44 ms`
- `102`: `2037.93 ms`
- `105`: `1899.07 ms`
- `103`: `1896.21 ms`
- `026`: `1827.32 ms`
- `162`: `1702.39 ms`

Those timings are useful as a correctness-and-usability baseline for the emitted backend. They should not be treated as a final throughput benchmark for larger designs such as XiangShan.

## Interpretation Rules

- "runtime-validated" means the full emitted-model flow finished and the runner checked outputs successfully.
- "162/162 success" means all HDLBits DUTs now complete the runnable C++ simulation path.
- This does not imply waveform-level equivalence against Verilator on every HDLBits testbench.
- It does establish that the current backend can generate, compile, run, and behavior-check the entire HDLBits suite through its own emitted C++ interface.

## Next Step

With HDLBits `162/162` runtime validation complete, the next downstream target is to return to XiangShan end-to-end smoke and timing-baseline work without blocking on HDLBits runner coverage.
