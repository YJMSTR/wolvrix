# HDLBits GSim Runtime Status

## Scope

This document tracks the HDLBits end-to-end runtime validation status for the `EmitGsimCpp` backend.

The goal here is stricter than "codegen succeeded". A DUT is counted as **runtime-validated** only when it completes the full path below:

1. `read_sv`
2. normalize / simplify pipeline
3. `gsim`
4. `write_gsim_cpp`
5. C++ compilation of the emitted model plus a DUT-specific runner with `-std=c++17 -Wall -Wextra -Werror`
6. execution of the runner with checked outputs

This is still **not** a Verilator difftest. It is an emitted-model smoke/behavior validation pass over selected HDLBits patterns.

## Latest Verified Baseline

Latest full runtime probe:

- Report: `build/artifacts/hdlbits_gsim_runtime_probe10/batch_report.json`
- Result: `126/126 success`
- Average elapsed time: `1348.74 ms` per DUT
- Max elapsed time: `1848.48 ms` (`dut_072`)
- Total elapsed time: `169947.93 ms`

Latest repo-local regression entry points:

- `python3 -S wolvrix/tests/hdlbits_gsim_runner_patterns_test.py`
- `python3 -S wolvrix/tests/hdlbits_gsim_batch_runtime_test.py`

## Current Coverage

As of the `hdlbits_gsim_runtime_probe10` run:

- Runtime-validated DUTs: `126`
- Compile-success DUTs from the compile-only batch: `134`
- Full HDLBits suite size: `162`

That means:

- `126 / 134 = 94.03%` of compile-success DUTs now complete the runtime path
- `126 / 162 = 77.78%` of the full HDLBits suite now complete the runtime path

The validated runtime set is the current `SUPPORTED_DUTS` set from `scripts/wolvrix_hdlbits_gsim_run.py`, excluding `dut_060`.

## What Was Added In This Round

This round extended runtime coverage from the earlier smaller subset to include:

- additional counters and event-capture logic: `097 099 100 101 102 103 104 107`
- additional LFSR / shift-register style DUTs: `110 111 112 113 115`
- more FSM and decoder-style DUTs: `119 120 121 122 123 124 127 128 129 132 140 145 146 147 148 149 150 158 160 161`
- serial-receiver style DUTs: `133 134`
- additional medium-width vector DUTs that still fit the current scalar runtime interface: `062 064 065`
- remaining runtime-safe arithmetic / combinational smoke coverage now rolls into the 126-DUT probe

## Remaining Compile-Success But Not Runtime-Validated

There are `8` compile-success DUTs that are still outside the runtime-validated set:

- `060`
- `098`
- `105`
- `106`
- `114`
- `116`
- `117`
- `142`

### Current blockers

- `dut_060`
  - The emitted C++ model currently triggers `shift-count-overflow` under `-Werror` during runner compilation.
  - This needs a codegen fix, not just a new runner descriptor.

- `dut_098`
  - The source uses both `posedge clk` and `negedge clk`.
  - The current runtime helper does not yet have an honest validation pattern for this dual-edge behavior.

- `dut_105`, `dut_106`, `dut_114`, `dut_142`
  - These are still missing dedicated runtime models / runner patterns.
  - They look feasible with the current backend and should be the next batch.

- `dut_116`, `dut_117`
  - These designs rely on very wide state (`[511:0]`).
  - The current emitted public interface still truncates widths above 64 bits, so counting them as runtime-validated today would be misleading.

## Important Interpretation Rules

- "compile-success" means the backend emitted C++ that compiled as a model artifact.
- "runtime-validated" means the emitted model was compiled together with a checker runner and executed successfully.
- A DUT should not be counted as "fully running simulation" unless it reaches the runtime-validated bar above.
- The current runtime numbers are intentionally conservative for very wide or dual-edge cases.

## Performance Notes

The current `avg_time_ms` is dominated by per-DUT subprocess startup, emitted-model compilation, and runner compilation.

The runtime probe therefore measures:

- backend usability for repeated small DUTs
- emitted-model compile/run stability

It does **not** isolate pure simulation-step throughput. For small HDLBits DUTs, code generation and compile overhead still dominate the wall-clock number.

The slowest DUTs in `hdlbits_gsim_runtime_probe10` were:

- `072`: `1848.48 ms`
- `034`: `1526.53 ms`
- `145`: `1501.84 ms`
- `127`: `1496.16 ms`
- `063`: `1456.26 ms`

## Recommended Next Batch

If continuing HDLBits runtime expansion before moving back to XiangShan, the next honest targets should be:

1. `105`
2. `106`
3. `114`
4. `142`

After that, the next structural items are:

1. fix `dut_060` codegen so the emitted model compiles cleanly under `-Werror`
2. decide whether dual-edge validation for `dut_098` needs runtime-helper support or emitter changes
3. add a non-truncating wide-value runtime interface before claiming runtime validation for `dut_116` and `dut_117`
