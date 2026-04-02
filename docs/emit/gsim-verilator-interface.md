# GSim Verilator-Compatible Interface

## Overview

The `EmitGsimCpp` emitter generates a C++ class `SSimTop` that provides an interface compatible with Verilator-style simulation workflows. This document maps the generated API to the expected Verilator usage pattern.

## Generated Class: `SSimTop`

```cpp
class SSimTop {
public:
    SSimTop();          // Calls reset()
    ~SSimTop() = default;

    void set_reset(unsigned reset);  // Assert/deassert reset
    void reset();                    // Initialize all state to zero
    void step();                     // Advance simulation by one cycle

    // Per-port accessors (generated from GRH ports)
    void set_<portname>(<type> value);       // Input port setter
    <type> get_<portname>() const;           // Output port getter
};
```

## Verilator Mapping

| Verilator Pattern | GSim Equivalent | Notes |
|-------------------|----------------|-------|
| `new VSimTop()` | `SSimTop sim;` | Constructor calls `reset()` automatically |
| `top->reset = 1; top->eval()` | `sim.set_reset(1); sim.step();` | Reset cycle |
| `top->clk = 1; top->eval()` | `sim.set_clk(1); sim.step();` | Clock edge + evaluate |
| `top->a = val` | `sim.set_a(val)` | Set input port |
| `result = top->y` | `result = sim.get_y()` | Read output port |
| `top->final()` | (destructor) | No explicit finalization needed |

## Simulation Flow

```cpp
SSimTop sim;

// Reset phase
sim.set_reset(1);
sim.step();           // Reset cycle: all state zeroed

// Normal operation
sim.set_a(3);
sim.set_b(5);
sim.step();           // Combinational outputs updated; registers capture inputs

auto y = sim.get_y(); // Read output
```

## step() Semantics

Each call to `step()`:
1. Increments the internal step counter
2. If reset is active: clears state and returns
3. Evaluates all combinational logic (drives output ports)
4. Applies sequential updates (register writes)

Combinational outputs reflect the **current** cycle's inputs. Register values visible through outputs reflect the **previous** cycle's captured state (1-cycle latency).

## Generated Source Set

`EmitGsimCpp` keeps the public API stable even when the implementation is sharded for large designs.

- The canonical entry points remain `<base>.hpp` and `<base>.cpp`.
- The emitter also writes `<base>.manifest`, which lists every managed implementation source in build order.
- Large metadata payloads may spill into `<base>__meta_*.cpp`.
- Large `step()` bodies may spill into `<base>__step_*.cpp`.

Downstream builds should compile the canonical `<base>.cpp` together with every file named in `<base>.manifest`. The public header does not change when sharding is enabled.

## Difftest Compatibility Stubs

The generated class includes stubs for downstream difftest integration:

```cpp
// UART stubs
unsigned get_difftest__DOT__uart__DOT__out__DOT__valid() const;
uint8_t get_difftest__DOT__uart__DOT__out__DOT__ch() const;
unsigned get_difftest__DOT__uart__DOT__in__DOT__valid() const;
void set_difftest__DOT__uart__DOT__in__DOT__ch(uint8_t);

// Control stubs
uint64_t get_difftest__DOT__exit() const;
uint64_t get_difftest__DOT__step() const;
void set_difftest__DOT__perfCtrl__DOT__clean(unsigned);
void set_difftest__DOT__perfCtrl__DOT__dump(unsigned);
void set_difftest__DOT__logCtrl__DOT__begin(uint64_t);
void set_difftest__DOT__logCtrl__DOT__end(uint64_t);
```

## Type Mapping

| RTL Width | C++ Type |
|-----------|----------|
| 1-8 bits | `std::uint8_t` |
| 9-16 bits | `std::uint16_t` |
| 17-32 bits | `std::uint32_t` |
| 33+ bits | `std::uint64_t` (truncated) |

## Limitations

- Signals wider than 64 bits are truncated to `uint64_t`
- No multi-clock domain support (single implicit clock)
- DPI-C support is limited to side-effect-only input-only calls, integral return values, integral `output` args, and `input string` args; `inout`, string return/output, and other unsupported signatures still raise unsupported-op diagnostics
- External module integration still requires flattening; `kInstance` / `kBlackbox` remain unsupported in emitted simulators
- Memory lowering is limited to the current vector-backed model emitted for `kMemory`, `kMemoryReadPort`, and `kMemoryWritePort`
- Four-state constant digits (`x/z/?`) are coerced to zero when lowered into the emitted 2-state C++ runtime
