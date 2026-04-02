# Port Order API

## Overview

The `EmitGsimCpp` emitter supports configurable port ordering in the generated simulator class. This controls the order in which `set_*()` and `get_*()` methods appear in the generated header.

## Configuration

### C++ API (`EmitOptions`)

```cpp
enum class PortOrderStrategy { Decl, Alpha, Custom };

struct EmitOptions {
    PortOrderStrategy portOrderStrategy = PortOrderStrategy::Decl;
    std::vector<std::string> portOrderNames;  // For Custom strategy
};
```

### Python API

```python
design.write_gsim_cpp(
    output="path/to/output",
    top=["top_module"],
    port_order="decl",           # "decl" | "alpha" | "custom"
    port_order_names=None,       # Required for "custom"
)
```

## Strategies

### `decl` (Default)

Ports appear in the order they were declared in the original RTL source (i.e., the order they appear in the GRH graph's port registry).

```python
# Input: module top(input a, input b, input clk, output y);
design.write_gsim_cpp(out, top=["top"], port_order="decl")
# Generated order: set_a, set_b, set_clk, get_y
```

### `alpha`

Ports are sorted alphabetically by name.

```python
design.write_gsim_cpp(out, top=["top"], port_order="alpha")
# Generated order: set_a, set_b, set_clk, get_y
```

### `custom`

Ports are ordered according to the user-provided name list. Ports not in the list are appended alphabetically.

```python
design.write_gsim_cpp(out, top=["top"],
    port_order="custom",
    port_order_names=["clk", "b", "a", "y"])
# Generated order: set_clk, set_b, set_a, get_y
```

## Error Handling

| Condition | Behavior |
|-----------|----------|
| Invalid `port_order` value | Python raises `ValueError`; C++ returns error |
| `port_order_names` contains nonexistent port | Emit fails with "port_order_names contains nonexistent port" |
| `port_order='custom'` without `port_order_names` | Falls back to declaration order |

## Notes

- Input and output ports are sorted independently using the same strategy.
- The `set_reset()` method always appears first, before any user port setters.
- Difftest compatibility stubs always appear after user port accessors.
