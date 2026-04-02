# Supported OperationKind Matrix

## EmitGsimCpp Operation Coverage

The table below shows which GRH `OperationKind` values are supported by the C++ code generator.

### Fully Supported

| OperationKind | C++ Expression | Notes |
|---------------|---------------|-------|
| `kConstant` | Literal value | Converts Verilog format (e.g., `8'hff` -> `0xff`) |
| `kAssign` | Direct assignment | `result = operand` |
| `kAdd` | `a + b` | |
| `kSub` | `a - b` | |
| `kMul` | `a * b` | |
| `kDiv` | `a / b` | |
| `kMod` | `a % b` | |
| `kAnd` | `a & b` | Bitwise AND |
| `kOr` | `a \| b` | Bitwise OR |
| `kXor` | `a ^ b` | Bitwise XOR |
| `kXnor` | `~(a ^ b) & mask` | Masked to operand width |
| `kNot` | `~a & mask` | Masked to operand width |
| `kLogicNot` | `!a` | |
| `kLogicAnd` | `a && b` | |
| `kLogicOr` | `a \|\| b` | |
| `kEq` | `a == b` | |
| `kNe` | `a != b` | |
| `kLt` | `a < b` | Unsigned comparison |
| `kLe` | `a <= b` | |
| `kGt` | `a > b` | |
| `kGe` | `a >= b` | |
| `kShl` | `a << b` | |
| `kLShr` | `a >> b` | Logical shift right |
| `kAShr` | Signed `a >> b` | Arithmetic shift right |
| `kMux` | `cond ? a : b` | |
| `kConcat` | Shift + OR | Operands concatenated MSB-first |
| `kReplicate` | Shift + OR | Repeated N times |
| `kSliceStatic` | `(a >> start) & mask` | Uses `sliceStart`, `sliceEnd` attrs |
| `kSliceDynamic` | `(a >> idx) & mask` | Uses `sliceWidth` attr |
| `kReduceAnd` | All-ones check | `(a & mask) == mask` |
| `kReduceOr` | Non-zero check | `a != 0` |
| `kReduceXor` | Parity | `__builtin_parityll(a)` |
| `kReduceNand` | NOT reduce-AND | |
| `kReduceNor` | NOT reduce-OR | |
| `kReduceXnor` | NOT reduce-XOR | |
| `kRegister` | State variable | Declares member storage |
| `kRegisterReadPort` | Read from register | Uses `regSymbol` attr |
| `kRegisterWritePort` | Conditional write | Uses `regSymbol` attr, supports mask |
| `kLatch` | State variable | Level-sensitive storage |
| `kLatchReadPort` | Read from latch | Uses `latchSymbol` attr |
| `kLatchWritePort` | Transparent write | Uses `latchSymbol` attr |
| `kSystemTask` | Ignored | Debug constructs |
| `kSystemFunction` | Ignored | Debug constructs |

### Not Supported

| OperationKind | Reason |
|---------------|--------|
| `kMemory` | Multi-port memory lowering not implemented |
| `kMemoryReadPort` | Depends on kMemory |
| `kMemoryWritePort` | Depends on kMemory |
| `kSliceArray` | Array indexing not implemented |
| `kInstance` | Requires flattened design (use `hier-flatten` pass) |
| `kBlackbox` | External modules cannot be simulated |
| `kDpicImport` | DPI-C not supported in generated simulator |
| `kDpicCall` | DPI-C not supported |
| `kXMRRead` | Cross-module references not supported |
| `kXMRWrite` | Cross-module references not supported |
| `kCaseEq` | Four-state comparison not applicable |
| `kCaseNe` | Four-state comparison not applicable |
| `kWildcardEq` | Wildcard comparison not applicable |
| `kWildcardNe` | Wildcard comparison not applicable |

### Known Limitations

- **Width > 64 bits**: All values are truncated to `uint64_t`. Designs with signals wider than 64 bits may produce incorrect results or compile warnings.
- **Concat/Replicate > 64 bits**: Shift amounts >= 64 are clamped to 0 (high bits discarded).
