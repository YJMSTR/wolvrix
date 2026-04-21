#include "emit/gsim_cpp.hpp"

#include "core/transform.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unordered_map>
#include <memory>

namespace wolvrix::lib::emit
{

    namespace
    {
        // Forward declaration
        std::string sanitizeIdentifier(std::string_view text);

        struct MemoryInfo
        {
            std::string storageName;
            std::string rowType;
            std::string zeroExpr;
            std::int64_t rows = 0;
            std::int32_t width = 0;
        };

        // Code generation state for lowering GRH operations to C++
        struct CodegenState
        {
            // Value variables: maps ValueId to C++ variable name (materialized for sharding)
            std::unordered_map<wolvrix::lib::grh::ValueId, std::string, wolvrix::lib::grh::ValueIdHash> valueVars;
            std::unordered_map<wolvrix::lib::grh::ValueId, std::pair<std::string, std::string>, wolvrix::lib::grh::ValueIdHash> outputPortValues;

            // Storage declarations: register/memory state variables
            std::vector<std::string> storageDecls;
            std::vector<std::string> storageResetStmts;

            // Sequential update statements (grouped by clock domain)
            std::map<std::string, std::vector<std::string>> sequentialStmts;
            std::map<std::string, std::vector<std::string>> sequentialRegs;
            std::map<std::string, std::string> sequentialClockExprs;

            // Non-sharded combinational statements for small designs
            std::vector<std::string> combinationalStmts;
            std::vector<std::string> latchStmts;

            // Port declarations and accessors
            std::vector<std::pair<std::string, std::string>> inputPorts;  // (name, type)
            std::vector<std::pair<std::string, std::string>> outputPorts; // (name, type)

            // Track unsupported operations for error reporting
            std::vector<std::string> unsupportedOps;

            // Memory declarations/read lowering support
            std::unordered_map<std::string, MemoryInfo> memories;

            // Sharding support: output streams for different shards
            std::vector<std::unique_ptr<std::ostringstream>> shardStreams;
            int currentShard = 0;
            int maxShardSize = 2097152; // 2MB per shard
            int currentShardSize = 0;

            // Flag to determine if sharding should be enabled based on operation count
            bool enableSharding = false;

            // Helper to get next available shard stream
            std::ostringstream* getCurrentShardStream() {
                if (!enableSharding) {
                    // For small designs, don't enable sharding
                    if (shardStreams.empty()) {
                        shardStreams.push_back(std::make_unique<std::ostringstream>());
                    }
                    return shardStreams[0].get();
                }

                if (shardStreams.empty() || currentShard >= static_cast<int>(shardStreams.size())) {
                    shardStreams.push_back(std::make_unique<std::ostringstream>());
                    currentShard = static_cast<int>(shardStreams.size()) - 1;
                    currentShardSize = 0;
                }
                return shardStreams[currentShard].get();
            }

            // Helper to create a new shard when current one gets too large
            void ensureShardSpace(int estimatedSize) {
                if (!enableSharding) {
                    return; // Don't shard for small designs
                }

                if (currentShardSize + estimatedSize > maxShardSize) {
                    currentShard++;
                    currentShardSize = 0;
                    if (currentShard >= static_cast<int>(shardStreams.size())) {
                        shardStreams.push_back(std::make_unique<std::ostringstream>());
                    }
                }
            }

            // Set result for a value ID
            void setResult(const wolvrix::lib::grh::ValueId& valueId, const std::string& expr, const std::string& cppType) {
                if (!enableSharding)
                {
                    valueVars[valueId] = expr;
                    return;
                }

                // Create a unique variable name for this result
                std::string varName = "var_" + std::to_string(valueId.index) + "_" + std::to_string(valueId.generation);
                valueVars[valueId] = varName;

                // GRH result ValueIds are unique and each defining operation is lowered once,
                // so sharded temporaries do not need a duplicate-declaration scan here.
                storageDecls.push_back(cppType + " " + varName + " = 0;");

                std::string assignment = varName + " = " + expr + ";";
                ensureShardSpace(static_cast<int>(assignment.length()));
                *getCurrentShardStream() << assignment << "\n";
                currentShardSize += static_cast<int>(assignment.length());
            }
        };

        // Convert Verilog-style constant to C++ constant
        // e.g., "1'b1" -> "1", "8'hff" -> "0xff"
        std::string convertVerilogConstant(const std::string& verilogConst)
        {
            // Check if it looks like a Verilog constant (contains ')
            size_t apostrophe = verilogConst.find('\'');
            if (apostrophe == std::string::npos) {
                // Not a Verilog constant, return as-is (might be plain number)
                return verilogConst;
            }

            // Parse Verilog constant format: <width>'<base><value>
            // e.g., "8'hff", "1'b1", "32'd123"
            if (apostrophe + 2 >= verilogConst.size()) {
                return "0"; // Invalid format
            }

            char base = verilogConst[apostrophe + 1];
            std::string value = verilogConst.substr(apostrophe + 2);

            // Convert based on base
            switch (base) {
                case 'h': // Hexadecimal
                    return "0x" + value;
                case 'b': // Binary
                    // Convert binary to hex for readability
                    try {
                        unsigned long val = std::stoul(value, nullptr, 2);
                        return std::to_string(val);
                    } catch (...) {
                        return "0";
                    }
                case 'd': // Decimal
                    return value;
                case 'o': // Octal
                    try {
                        unsigned long val = std::stoul(value, nullptr, 8);
                        return std::to_string(val);
                    } catch (...) {
                        return "0";
                    }
                default:
                    return "0";
            }
        }

        // Get C++ type for a value based on its width
        std::string getCppTypeForWidth(int32_t width)
        {
            if (width <= 8)
                return "std::uint8_t";
            if (width <= 16)
                return "std::uint16_t";
            if (width <= 32)
                return "std::uint32_t";
            if (width <= 64)
                return "std::uint64_t";
            return "std::vector<std::uint64_t>"; // For very wide values
        }

        // Generate mask for given bit width
        std::string generateMask(int32_t width)
        {
            if (width >= 64)
                return "~0ULL";
            return std::to_string((1ULL << width) - 1);
        }

        std::string maskExprForWidth(const std::string &expr, int32_t width)
        {
            if (width <= 0 || width >= 64)
            {
                return expr;
            }
            return "((" + expr + ") & " + generateMask(width) + ")";
        }

        std::string zeroInitializerForType(std::string_view cppType)
        {
            if (cppType == "std::vector<std::uint64_t>")
            {
                return "{}";
            }
            return "0";
        }

        std::string zeroInitializerForWidth(int32_t width)
        {
            if (width > 64)
            {
                const auto chunkCount = static_cast<int32_t>((width + 63) / 64);
                return "std::vector<std::uint64_t>(" + std::to_string(chunkCount) + ", 0ULL)";
            }
            return zeroInitializerForType(getCppTypeForWidth(width));
        }

        std::string castScalarExprForWidth(const std::string &expr, int32_t width)
        {
            if (width > 64)
            {
                return expr;
            }
            return "static_cast<" + getCppTypeForWidth(width) + ">(" + expr + ")";
        }

        std::optional<std::string> findClockLikeInputName(const std::vector<std::pair<std::string, std::string>> &inputPorts)
        {
            auto tryMatch = [&](auto predicate) -> std::optional<std::string>
            {
                for (const auto &[name, type] : inputPorts)
                {
                    (void)type;
                    if (predicate(sanitizeIdentifier(name)))
                    {
                        return sanitizeIdentifier(name);
                    }
                }
                return std::nullopt;
            };

            if (auto exactClk = tryMatch([](const std::string &name) { return name == "clk"; }))
            {
                return exactClk;
            }
            if (auto exactClock = tryMatch([](const std::string &name) { return name == "clock"; }))
            {
                return exactClock;
            }
            if (auto suffixClk = tryMatch([](const std::string &name) { return name.size() > 4 && name.ends_with("_clk"); }))
            {
                return suffixClk;
            }
            if (auto containsClock = tryMatch([](const std::string &name) { return name.find("clock") != std::string::npos; }))
            {
                return containsClock;
            }
            return std::nullopt;
        }

        bool hasInputPortNamed(const std::vector<std::pair<std::string, std::string>> &inputPorts,
                               std::string_view candidate)
        {
            for (const auto &[name, type] : inputPorts)
            {
                (void)type;
                if (sanitizeIdentifier(name) == candidate)
                {
                    return true;
                }
            }
            return false;
        }

        std::string resolveSequentialClockStateName(
            std::string_view clockSymbol,
            const std::vector<std::pair<std::string, std::string>> &inputPorts)
        {
            std::string resolvedClock = sanitizeIdentifier(clockSymbol);
            if (hasInputPortNamed(inputPorts, resolvedClock))
            {
                return resolvedClock;
            }
            if (auto fallbackClock = findClockLikeInputName(inputPorts))
            {
                return *fallbackClock;
            }
            return resolvedClock;
        }

        // Forward declaration
        struct GsimScratchpadMetadata;

        // Lower a single operation to C++
        void lowerOperation(
            const wolvrix::lib::grh::Graph& graph,
            const wolvrix::lib::grh::Operation& op,
            const GsimScratchpadMetadata& /*metadata*/,
            CodegenState& state,
            EmitDiagnostics* /*diagnostics*/)
        {
            using namespace wolvrix::lib::grh;

            const auto kind = op.kind();
            const auto opId = op.id();

            // Helper to get operand variable name (now returns var name from valueVars)
            auto getOperandExpr = [&](size_t idx) -> std::string {
                if (idx >= op.operands().size()) {
                    return "0";
                }
                auto it = state.valueVars.find(op.operands()[idx]);
                if (it != state.valueVars.end()) {
                    return it->second;
                }
                return "0";
            };

            // Helper to set result: materialize expression as variable and write to current shard
            auto setResultExpr = [&](size_t idx, const std::string& expr) {
                if (idx < op.results().size()) {
                    const auto resultValue = graph.getValue(op.results()[idx]);
                    state.setResult(op.results()[idx], expr, getCppTypeForWidth(resultValue.width()));
                }
            };

            switch (kind) {
                case OperationKind::kConstant: {
                    // Try "constValue" first (real GRH contract), fallback to "value"
                    auto valueAttr = op.attr("constValue");
                    if (!valueAttr) {
                        valueAttr = op.attr("value");
                    }
                    if (valueAttr) {
                        // constValue is stored as string, parse it
                        if (auto* strVal = std::get_if<std::string>(&*valueAttr)) {
                            // Convert Verilog constant to C++ constant
                            setResultExpr(0, convertVerilogConstant(*strVal));
                        } else if (auto* intVal = std::get_if<int64_t>(&*valueAttr)) {
                            setResultExpr(0, std::to_string(*intVal));
                        } else {
                            setResultExpr(0, "0");
                        }
                    } else {
                        setResultExpr(0, "0");
                    }
                    break;
                }

                case OperationKind::kAssign: {
                    setResultExpr(0, getOperandExpr(0));
                    break;
                }

                case OperationKind::kAdd: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " + " + getOperandExpr(1) + ")");
                    break;
                }

                case OperationKind::kSub: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " - " + getOperandExpr(1) + ")");
                    break;
                }

                case OperationKind::kMul: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " * " + getOperandExpr(1) + ")");
                    break;
                }

                case OperationKind::kDiv: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " / " + getOperandExpr(1) + ")");
                    break;
                }

                case OperationKind::kAnd: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " & " + getOperandExpr(1) + ")");
                    break;
                }

                case OperationKind::kOr: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " | " + getOperandExpr(1) + ")");
                    break;
                }

                case OperationKind::kXor: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " ^ " + getOperandExpr(1) + ")");
                    break;
                }

                case OperationKind::kXnor: {
                    if (!op.results().empty())
                    {
                        const auto resultValue = graph.getValue(op.results()[0]);
                        setResultExpr(0, maskExprForWidth("(~(" + getOperandExpr(0) + " ^ " + getOperandExpr(1) + "))",
                                                          resultValue.width()));
                    }
                    break;
                }

                case OperationKind::kEq: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " == " + getOperandExpr(1) + ")");
                    break;
                }

                case OperationKind::kCaseEq: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " == " + getOperandExpr(1) + ")");
                    break;
                }

                case OperationKind::kCaseNe: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " != " + getOperandExpr(1) + ")");
                    break;
                }

                case OperationKind::kReduceAnd: {
                    setResultExpr(0, "((" + getOperandExpr(0) + " == " + generateMask(graph.getValue(op.operands()[0]).width()) + ") ? 1U : 0U)");
                    break;
                }

                case OperationKind::kReduceOr: {
                    setResultExpr(0, "((" + getOperandExpr(0) + " != 0) ? 1U : 0U)");
                    break;
                }

                case OperationKind::kReduceXor: {
                    setResultExpr(0, "(static_cast<unsigned>(__builtin_parityll(static_cast<unsigned long long>(" + getOperandExpr(0) + "))))");
                    break;
                }

                case OperationKind::kLt: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " < " + getOperandExpr(1) + ")");
                    break;
                }

                case OperationKind::kLe: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " <= " + getOperandExpr(1) + ")");
                    break;
                }

                case OperationKind::kGt: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " > " + getOperandExpr(1) + ")");
                    break;
                }

                case OperationKind::kGe: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " >= " + getOperandExpr(1) + ")");
                    break;
                }

                case OperationKind::kNe: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " != " + getOperandExpr(1) + ")");
                    break;
                }

                case OperationKind::kNot: {
                    if (!op.results().empty())
                    {
                        const auto resultValue = graph.getValue(op.results()[0]);
                        setResultExpr(0, maskExprForWidth("(~" + getOperandExpr(0) + ")", resultValue.width()));
                    }
                    break;
                }

                case OperationKind::kLogicNot: {
                    setResultExpr(0, "(!" + getOperandExpr(0) + ")");
                    break;
                }

                case OperationKind::kLogicAnd: {
                    setResultExpr(0, "((" + getOperandExpr(0) + " && " + getOperandExpr(1) + ") ? 1U : 0U)");
                    break;
                }

                case OperationKind::kLogicOr: {
                    setResultExpr(0, "((" + getOperandExpr(0) + " || " + getOperandExpr(1) + ") ? 1U : 0U)");
                    break;
                }

                case OperationKind::kMux: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " ? " + getOperandExpr(1) + " : " + getOperandExpr(2) + ")");
                    break;
                }

                case OperationKind::kConcat: {
                    if (op.operands().empty()) {
                        break;
                    }
                    if (op.operands().size() == 1) {
                        setResultExpr(0, getOperandExpr(0));
                        break;
                    }
                    std::int64_t totalWidth = 0;
                    bool widthKnown = true;
                    bool hasWideOperand = false;
                    for (const auto operand : op.operands()) {
                        const auto value = graph.getValue(operand);
                        if (value.width() <= 0) {
                            widthKnown = false;
                            break;
                        }
                        if (value.width() > 64) {
                            hasWideOperand = true;
                        }
                        totalWidth += value.width();
                    }
                    if (!widthKnown) {
                        std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                        state.unsupportedOps.push_back("kConcat-wide (" + opName + ")");
                        break;
                    }

                    if (totalWidth > 64 || hasWideOperand) {
                        std::string expr = getOperandExpr(0);
                        std::int64_t exprWidth = graph.getValue(op.operands()[0]).width();
                        for (std::size_t i = 1; i < op.operands().size(); ++i) {
                            const auto value = graph.getValue(op.operands()[i]);
                            expr = "wolvrix_gsim_concat(" + expr + ", " + std::to_string(exprWidth) + ", " +
                                   getOperandExpr(i) + ", " + std::to_string(value.width()) + ")";
                            exprWidth += value.width();
                        }
                        setResultExpr(0, expr);
                        break;
                    }

                    std::string expr = getOperandExpr(0);
                    for (std::size_t i = 1; i < op.operands().size(); ++i) {
                        const auto value = graph.getValue(op.operands()[i]);
                        expr = "((" + expr + " << " + std::to_string(value.width()) + ") | (" +
                               getOperandExpr(i) + " & " + generateMask(value.width()) + "))";
                    }
                    setResultExpr(0, expr);
                    break;
                }

                case OperationKind::kSliceDynamic: {
                    if (op.operands().size() < 2 || op.results().empty()) {
                        break;
                    }

                    auto sliceWidthAttr = op.attr("sliceWidth");
                    auto *sliceWidth = sliceWidthAttr ? std::get_if<int64_t>(&*sliceWidthAttr) : nullptr;
                    if (sliceWidth == nullptr || *sliceWidth <= 0 || *sliceWidth > 64) {
                        std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                        state.unsupportedOps.push_back("kSliceDynamic-width (" + opName + ")");
                        break;
                    }

                    const auto baseValue = graph.getValue(op.operands()[0]);
                    const auto operandWidth = baseValue.width();
                    if (operandWidth <= 0 || *sliceWidth > operandWidth) {
                        std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                        state.unsupportedOps.push_back("kSliceDynamic-wide (" + opName + ")");
                        break;
                    }

                    if (operandWidth == 1) {
                        setResultExpr(0, maskExprForWidth(getOperandExpr(0), static_cast<int32_t>(*sliceWidth)));
                        break;
                    }

                    const std::string rawIndexExpr = getOperandExpr(1);
                    const std::string indexExpr =
                        "static_cast<std::uint64_t>(" + rawIndexExpr + ")";
                    if (operandWidth > 64) {
                        const std::string slicedExpr =
                            "wolvrix_gsim_slice_dynamic_to_u64(" + getOperandExpr(0) + ", " + indexExpr + ", " +
                            std::to_string(*sliceWidth) + ", " + std::to_string(operandWidth) + ")";
                        setResultExpr(0, maskExprForWidth(slicedExpr, static_cast<int32_t>(*sliceWidth)));
                        break;
                    }

                    const std::string slicedExpr =
                        "((" + indexExpr + " >= " + std::to_string(operandWidth) + ") ? 0ULL : ((" +
                        getOperandExpr(0) + " >> " + indexExpr + ") & " +
                        generateMask(static_cast<int32_t>(*sliceWidth)) + "))";
                    setResultExpr(0, maskExprForWidth(slicedExpr, static_cast<int32_t>(*sliceWidth)));
                    break;
                }

                case OperationKind::kSliceStatic: {
                    if (op.operands().empty() || op.results().empty()) {
                        break;
                    }

                    auto sliceStartAttr = op.attr("sliceStart");
                    auto sliceEndAttr = op.attr("sliceEnd");
                    auto *sliceStart = sliceStartAttr ? std::get_if<int64_t>(&*sliceStartAttr) : nullptr;
                    auto *sliceEnd = sliceEndAttr ? std::get_if<int64_t>(&*sliceEndAttr) : nullptr;
                    if (sliceStart == nullptr || sliceEnd == nullptr || *sliceStart < 0 || *sliceEnd < *sliceStart) {
                        std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                        state.unsupportedOps.push_back("kSliceStatic-attr (" + opName + ")");
                        break;
                    }

                    const auto baseValue = graph.getValue(op.operands()[0]);
                    const auto operandWidth = baseValue.width();
                    const auto sliceWidth = *sliceEnd - *sliceStart + 1;
                    if (operandWidth <= 0 || operandWidth > 64 || *sliceEnd >= operandWidth || sliceWidth <= 0 || sliceWidth > 64) {
                        std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                        state.unsupportedOps.push_back("kSliceStatic-wide (" + opName + ")");
                        break;
                    }

                    const std::string slicedExpr =
                        "((" + getOperandExpr(0) + " >> " + std::to_string(*sliceStart) + ") & " +
                        generateMask(static_cast<int32_t>(sliceWidth)) + ")";
                    setResultExpr(0, maskExprForWidth(slicedExpr, static_cast<int32_t>(sliceWidth)));
                    break;
                }

                case OperationKind::kReplicate: {
                    if (op.operands().empty() || op.results().empty()) {
                        break;
                    }
                    auto repAttr = op.attr("rep");
                    auto *rep = repAttr ? std::get_if<int64_t>(&*repAttr) : nullptr;
                    if (rep == nullptr || *rep <= 0) {
                        std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                        state.unsupportedOps.push_back("kReplicate-attr (" + opName + ")");
                        break;
                    }
                    const auto operandWidth = graph.getValue(op.operands()[0]).width();
                    const auto totalWidth = operandWidth * *rep;
                    if (operandWidth <= 0 || operandWidth > 64 || totalWidth > 64) {
                        std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                        state.unsupportedOps.push_back("kReplicate-wide (" + opName + ")");
                        break;
                    }
                    std::string expr = "0";
                    for (int64_t i = 0; i < *rep; ++i) {
                        expr = "((" + expr + " << " + std::to_string(operandWidth) + ") | (" +
                               getOperandExpr(0) + " & " + generateMask(operandWidth) + "))";
                    }
                    setResultExpr(0, maskExprForWidth(expr, static_cast<int32_t>(totalWidth)));
                    break;
                }

                case OperationKind::kShl: {
                    if (!op.results().empty()) {
                        const auto resultWidth = graph.getValue(op.results()[0]).width();
                        setResultExpr(0, maskExprForWidth("(" + getOperandExpr(0) + " << " + getOperandExpr(1) + ")",
                                                          resultWidth));
                    }
                    break;
                }

                case OperationKind::kLShr: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " >> " + getOperandExpr(1) + ")");
                    break;
                }

                case OperationKind::kRegister: {
                    // Register defines storage - handled in port collection
                    break;
                }

                case OperationKind::kLatch: {
                    // Latch declaration defines storage - handled in storage collection
                    break;
                }

                case OperationKind::kMemory: {
                    // Memory declarations are handled during storage collection.
                    break;
                }

                case OperationKind::kRegisterReadPort: {
                    auto regSymAttr = op.attr("regSymbol");
                    std::string sym;
                    if (regSymAttr) {
                        if (auto *attrSym = std::get_if<std::string>(&*regSymAttr)) {
                            sym = *attrSym;
                        }
                    }
                    if (sym.empty()) {
                        sym = std::string(op.symbolText());
                    }
                    if (!sym.empty()) {
                        std::string regName = "reg_" + sanitizeIdentifier(sym);
                        setResultExpr(0, regName);
                    }
                    break;
                }

                case OperationKind::kLatchReadPort: {
                    auto latchSymAttr = op.attr("latchSymbol");
                    std::string sym;
                    if (latchSymAttr) {
                        if (auto *attrSym = std::get_if<std::string>(&*latchSymAttr)) {
                            sym = *attrSym;
                        }
                    }
                    if (sym.empty()) {
                        sym = std::string(op.symbolText());
                    }
                    if (!sym.empty()) {
                        std::string latchName = "latch_" + sanitizeIdentifier(sym);
                        setResultExpr(0, latchName);
                    }
                    break;
                }

                case OperationKind::kMemoryReadPort: {
                    auto memSymAttr = op.attr("memSymbol");
                    std::string sym;
                    if (memSymAttr) {
                        if (auto *attrSym = std::get_if<std::string>(&*memSymAttr)) {
                            sym = *attrSym;
                        }
                    }
                    if (sym.empty()) {
                        sym = std::string(op.symbolText());
                    }
                    const auto memoryIt = state.memories.find(sym);
                    if (memoryIt == state.memories.end()) {
                        std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                        state.unsupportedOps.push_back("kMemoryReadPort-unresolved (" + opName + ")");
                        break;
                    }
                    const auto &memory = memoryIt->second;
                    const std::string indexExpr =
                        "static_cast<std::size_t>(static_cast<std::uint64_t>(" + getOperandExpr(0) + "))";
                    setResultExpr(
                        0,
                        "((" + indexExpr + " < " + memory.storageName + ".size()) ? " +
                            memory.storageName + "[" + indexExpr + "] : " + memory.zeroExpr + ")");
                    break;
                }

                case OperationKind::kLatchWritePort: {
                    if (op.operands().size() < 3) {
                        break;
                    }

                    auto latchSymAttr = op.attr("latchSymbol");
                    std::string sym;
                    if (latchSymAttr) {
                        if (auto *attrSym = std::get_if<std::string>(&*latchSymAttr)) {
                            sym = *attrSym;
                        }
                    }
                    if (sym.empty()) {
                        sym = std::string(op.symbolText());
                    }
                    if (sym.empty()) {
                        break;
                    }

                    const std::string latchName = "latch_" + sanitizeIdentifier(sym);
                    const std::string condition = getOperandExpr(0);
                    const std::string nextValue = getOperandExpr(1);
                    const std::string mask = getOperandExpr(2);

                    if (mask != "0") {
                        state.latchStmts.push_back(
                            "        if (" + condition + ") { " + latchName + " = (" + latchName +
                            " & ~" + mask + ") | (" + nextValue + " & " + mask + "); }");
                    } else {
                        state.latchStmts.push_back(
                            "        if (" + condition + ") { " + latchName + " = " + nextValue + "; }");
                    }
                    break;
                }

                case OperationKind::kRegisterWritePort: {
                    // Operands: [updateCond, nextValue, mask, ...]
                    std::string condition = getOperandExpr(0);
                    std::string nextValue = getOperandExpr(1);
                    std::string mask = getOperandExpr(2); // May be "0" if not present

                    // Find the target register by looking at the first operand's defining op
                    // Or use regSymbol attribute if available
                    auto regSymAttr = op.attr("regSymbol");
                    std::string regName;
                    if (regSymAttr) {
                        if (auto* sym = std::get_if<std::string>(&*regSymAttr)) {
                            regName = "reg_" + sanitizeIdentifier(*sym);
                        }
                    }

                    // If no regSymbol, try to infer from operand
                    if (regName.empty() && !op.operands().empty()) {
                        auto condVal = op.operands()[0];
                        auto defOp = graph.valueDef(condVal);
                        if (defOp.valid()) {
                            auto defOpObj = graph.getOperation(defOp);
                            if (defOpObj.kind() == OperationKind::kRegister) {
                                std::string sym = std::string(defOpObj.symbolText());
                                if (!sym.empty()) {
                                    regName = "reg_" + sanitizeIdentifier(sym);
                                }
                            }
                        }
                    }

                    std::string domainKey = "posedge:clock";
                    auto clockSymAttr = op.attr("clockSymbol");
                    std::string clockSymbol = "clock";
                    if (clockSymAttr) {
                        if (auto *sym = std::get_if<std::string>(&*clockSymAttr)) {
                            if (!sym->empty()) {
                                clockSymbol = *sym;
                            }
                        }
                    }
                    auto eventEdgeAttr = op.attr("eventEdge");
                    std::string eventEdge = "posedge";
                    if (eventEdgeAttr) {
                        if (auto *edges = std::get_if<std::vector<std::string>>(&*eventEdgeAttr)) {
                            if (!edges->empty() && !(*edges)[0].empty()) {
                                eventEdge = (*edges)[0];
                            }
                        }
                    }
                    domainKey = eventEdge + ":" + clockSymbol;

                    if (op.operands().size() > 3) {
                        const std::string clockExpr = getOperandExpr(3);
                        if (clockExpr != "0" && !clockExpr.empty()) {
                            state.sequentialClockExprs.try_emplace(domainKey, clockExpr);
                        }
                    }

                    if (!regName.empty()) {
                        auto &domainRegs = state.sequentialRegs[domainKey];
                        if (std::find(domainRegs.begin(), domainRegs.end(), regName) == domainRegs.end()) {
                            domainRegs.push_back(regName);
                        }
                        if (mask != "0") {
                            state.sequentialStmts[domainKey].push_back(
                                "        if (" + condition + ") { next_" + regName + " = (" + regName + " & ~" + mask + ") | (" + nextValue + " & " + mask + "); committed_ = true; }");
                        } else {
                            state.sequentialStmts[domainKey].push_back(
                                "        if (" + condition + ") { next_" + regName + " = " + nextValue + "; committed_ = true; }");
                        }
                    }
                    break;
                }

                case OperationKind::kDpicImport:
                case OperationKind::kSystemTask:
                case OperationKind::kSystemFunction: {
                    // DPI imports are declarations with no operands/results, so GSIM can
                    // safely ignore them at emit time until call lowering support exists.
                    // System tasks/functions are debug/diagnostic constructs and likewise
                    // do not generate simulation logic in the emitted C++ runtime.
                    break;
                }

                default: {
                    std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                    state.unsupportedOps.push_back(
                        std::string(toString(kind)) + " (" + opName + ")");
                    break;
                }
            }
        }

        // Sort ports according to strategy
        void sortPorts(std::vector<std::pair<std::string, std::string>>& ports,
                       PortOrderStrategy strategy,
                       const std::vector<std::string>& customOrder)
        {
            switch (strategy) {
                case PortOrderStrategy::Decl:
                    // Keep declaration order (no change)
                    break;
                case PortOrderStrategy::Alpha:
                    std::sort(ports.begin(), ports.end(),
                              [](const auto& a, const auto& b) { return a.first < b.first; });
                    break;
                case PortOrderStrategy::Custom:
                    // Sort by custom order, then append remaining ports
                    if (!customOrder.empty()) {
                        std::unordered_map<std::string, size_t> orderMap;
                        for (size_t i = 0; i < customOrder.size(); ++i) {
                            orderMap[customOrder[i]] = i;
                        }
                        std::sort(ports.begin(), ports.end(),
                                  [&orderMap](const auto& a, const auto& b) {
                                      auto itA = orderMap.find(a.first);
                                      auto itB = orderMap.find(b.first);
                                      bool hasA = itA != orderMap.end();
                                      bool hasB = itB != orderMap.end();
                                      if (hasA && hasB) return itA->second < itB->second;
                                      if (hasA) return true;
                                      if (hasB) return false;
                                      return a.first < b.first;
                                  });
                    }
                    break;
            }
        }

        // Collect port information from graph
        void collectPorts(
            const wolvrix::lib::grh::Graph& graph,
            CodegenState& state,
            PortOrderStrategy strategy = PortOrderStrategy::Decl,
            const std::vector<std::string>& customOrder = {})
        {
            for (const auto& port : graph.inputPorts()) {
                auto value = graph.getValue(port.value);
                std::string type = getCppTypeForWidth(value.width());
                state.inputPorts.push_back({port.name, type});
                // Seed input port values into CodegenState as variable names
                std::string portVar = "input_" + sanitizeIdentifier(port.name) + "_";
                state.valueVars[port.value] = portVar;
            }

            for (const auto& port : graph.outputPorts()) {
                auto value = graph.getValue(port.value);
                std::string type = getCppTypeForWidth(value.width());
                state.outputPorts.push_back({port.name, type});
                state.outputPortValues.emplace(port.value, std::make_pair(port.name, type));
            }

            // Apply port ordering
            sortPorts(state.inputPorts, strategy, customOrder);
            sortPorts(state.outputPorts, strategy, customOrder);
        }

        // Collect register storage declarations
        void collectRegisters(
            const wolvrix::lib::grh::Graph& graph,
            CodegenState& state)
        {
            for (const auto& opId : graph.operations()) {
                auto op = graph.getOperation(opId);
                if (op.kind() == wolvrix::lib::grh::OperationKind::kRegister) {
                    std::string sym;
                    auto regSymAttr = op.attr("regSymbol");
                    if (regSymAttr) {
                        if (auto* attrSym = std::get_if<std::string>(&*regSymAttr)) {
                            sym = *attrSym;
                        }
                    }
                    if (sym.empty()) {
                        sym = std::string(op.symbolText());
                    }
                    if (sym.empty()) {
                        sym = "unnamed_reg_" + std::to_string(opId.index);
                    }
                    std::string regName = "reg_" + sanitizeIdentifier(sym);

                    // Get width from result value
                    int32_t width = 32; // Default width
                    if (!op.results().empty()) {
                        auto val = graph.getValue(op.results()[0]);
                        width = val.width();
                    }
                    std::string type = getCppTypeForWidth(width);
                    const std::string zeroInit = zeroInitializerForType(type);
                    state.storageDecls.push_back(type + " " + regName + " = " + zeroInit + ";");
                    state.storageResetStmts.push_back(regName + " = " + zeroInit + ";");

                    // Also create a mapping from the register's result ValueId to the register name
                    if (!op.results().empty()) {
                        state.valueVars[op.results()[0]] = regName;
                    }
                }
            }
        }

        void collectLatches(
            const wolvrix::lib::grh::Graph& graph,
            CodegenState& state)
        {
            for (const auto& opId : graph.operations()) {
                auto op = graph.getOperation(opId);
                if (op.kind() == wolvrix::lib::grh::OperationKind::kLatch) {
                    std::string sym;
                    auto latchSymAttr = op.attr("latchSymbol");
                    if (latchSymAttr) {
                        if (auto* attrSym = std::get_if<std::string>(&*latchSymAttr)) {
                            sym = *attrSym;
                        }
                    }
                    if (sym.empty()) {
                        sym = std::string(op.symbolText());
                    }
                    if (sym.empty()) {
                        sym = "unnamed_latch_" + std::to_string(opId.index);
                    }
                    std::string latchName = "latch_" + sanitizeIdentifier(sym);

                    int32_t width = 32;
                    auto widthAttr = op.attr("width");
                    if (widthAttr) {
                        if (auto* attrWidth = std::get_if<int64_t>(&*widthAttr)) {
                            width = static_cast<int32_t>(*attrWidth);
                        }
                    }

                    std::string type = getCppTypeForWidth(width);
                    const std::string zeroInit = zeroInitializerForType(type);
                    state.storageDecls.push_back(type + " " + latchName + " = " + zeroInit + ";");
                    state.storageResetStmts.push_back(latchName + " = " + zeroInit + ";");
                }
            }
        }

        void collectMemories(
            const wolvrix::lib::grh::Graph &graph,
            CodegenState &state)
        {
            for (const auto &opId : graph.operations()) {
                const auto op = graph.getOperation(opId);
                if (op.kind() != wolvrix::lib::grh::OperationKind::kMemory) {
                    continue;
                }

                const std::string sym = op.symbolText().empty()
                                            ? "unnamed_mem_" + std::to_string(opId.index)
                                            : std::string(op.symbolText());
                const auto widthAttr = op.attr("width");
                const auto rowAttr = op.attr("row");
                const auto *widthPtr = widthAttr ? std::get_if<int64_t>(&*widthAttr) : nullptr;
                const auto *rowPtr = rowAttr ? std::get_if<int64_t>(&*rowAttr) : nullptr;
                if (widthPtr == nullptr || rowPtr == nullptr || *widthPtr <= 0 || *rowPtr <= 0) {
                    state.unsupportedOps.push_back("kMemory-metadata (" + sym + ")");
                    continue;
                }

                MemoryInfo memory;
                memory.storageName = "mem_" + sanitizeIdentifier(sym) + "_";
                memory.rowType = getCppTypeForWidth(static_cast<int32_t>(*widthPtr));
                memory.zeroExpr = zeroInitializerForWidth(static_cast<int32_t>(*widthPtr));
                memory.rows = *rowPtr;
                memory.width = static_cast<int32_t>(*widthPtr);

                const std::string storageType = "std::vector<" + memory.rowType + ">";
                const std::string initExpr =
                    storageType + "(" + std::to_string(memory.rows) + ", " + memory.zeroExpr + ")";
                state.storageDecls.push_back(storageType + " " + memory.storageName + " = " + initExpr + ";");
                state.storageResetStmts.push_back(memory.storageName + " = " + initExpr + ";");
                state.memories.emplace(sym, memory);

                auto initKindsAttr = op.attr("initKind");
                if (!initKindsAttr) {
                    continue;
                }
                auto initStartsAttr = op.attr("initStart");
                auto initLensAttr = op.attr("initLen");
                auto initValuesAttr = op.attr("initValue");
                const auto *initKinds = std::get_if<std::vector<std::string>>(&*initKindsAttr);
                const auto *initStarts =
                    initStartsAttr ? std::get_if<std::vector<int64_t>>(&*initStartsAttr) : nullptr;
                const auto *initLens =
                    initLensAttr ? std::get_if<std::vector<int64_t>>(&*initLensAttr) : nullptr;
                const auto *initValues =
                    initValuesAttr ? std::get_if<std::vector<std::string>>(&*initValuesAttr) : nullptr;
                if (initKinds == nullptr || initStarts == nullptr || initLens == nullptr ||
                    initKinds->size() != initStarts->size() || initKinds->size() != initLens->size()) {
                    state.unsupportedOps.push_back("kMemory-init-metadata (" + sym + ")");
                    continue;
                }

                for (std::size_t i = 0; i < initKinds->size(); ++i) {
                    if ((*initKinds)[i] != "literal") {
                        state.unsupportedOps.push_back("kMemory-init-kind (" + sym + ")");
                        continue;
                    }
                    if (memory.width > 64) {
                        state.unsupportedOps.push_back("kMemory-init-wide (" + sym + ")");
                        continue;
                    }

                    const std::string literal =
                        (initValues != nullptr && i < initValues->size()) ? (*initValues)[i] : "0";
                    const std::string valueExpr =
                        castScalarExprForWidth(convertVerilogConstant(literal), memory.width);
                    const auto start = (*initStarts)[i];
                    const auto len = (*initLens)[i];
                    if (start < 0) {
                        const std::string fillExpr =
                            storageType + "(" + std::to_string(memory.rows) + ", " + valueExpr + ")";
                        state.storageResetStmts.push_back(memory.storageName + " = " + fillExpr + ";");
                        continue;
                    }
                    if (len <= 0) {
                        state.unsupportedOps.push_back("kMemory-init-range (" + sym + ")");
                        continue;
                    }

                    const auto clampedStart = std::min<std::int64_t>(start, memory.rows);
                    const auto clampedEnd = std::min<std::int64_t>(start + len, memory.rows);
                    if (clampedStart >= clampedEnd) {
                        continue;
                    }
                    if (clampedEnd - clampedStart == 1) {
                        state.storageResetStmts.push_back(
                            memory.storageName + "[" + std::to_string(clampedStart) + "] = " + valueExpr + ";");
                        continue;
                    }
                    state.storageResetStmts.push_back(
                        "for (std::size_t __mem_idx = " + std::to_string(clampedStart) + "; __mem_idx < " +
                        std::to_string(clampedEnd) + "; ++__mem_idx) { " + memory.storageName +
                        "[__mem_idx] = " + valueExpr + "; }");
                }
            }
        }

        struct GsimScratchpadMetadata
        {
            std::vector<int64_t> roots;
            std::map<std::string, std::vector<int64_t>> eventGroups;
            std::vector<std::string> eventGroupNames;
            std::vector<std::string> scheduleActivityOrder;
            std::map<std::string, std::vector<int64_t>> scheduleActivityMembers;
            std::map<std::string, std::string> scheduleActivityClasses;
            std::vector<std::string> hypergraphNodeNames;
            std::map<std::string, std::vector<int64_t>> hypergraphNodeMembers;
            std::vector<std::string> hypergraphEdgeNames;
            std::map<std::string, std::string> hypergraphEdgeSources;
            std::map<std::string, std::string> hypergraphEdgeTargets;
            std::map<std::string, std::vector<int64_t>> hypergraphEdgeSinks;
            std::vector<int64_t> topoOrder;
            std::map<int64_t, std::vector<int64_t>> predecessors;
            std::map<int64_t, std::vector<int64_t>> successors;
            std::map<int64_t, std::string> classifications;
            std::vector<std::string> opDescriptors;
            std::string scheduleKind;
            int64_t scheduleVersion = 0;
            std::string scheduleContract;
            std::string hypergraphKind;
            int64_t hypergraphVersion = 0;
            std::string hypergraphContract;
            std::string graphSymbol;
            int64_t opCount = 0;
            int64_t graphRevision = 0;
        };

        std::optional<std::string> attrValue(const EmitOptions &options, std::string_view key)
        {
            const auto it = options.attributes.find(std::string(key));
            if (it == options.attributes.end() || it->second.empty())
            {
                return std::nullopt;
            }
            return it->second;
        }

        std::string sanitizeIdentifier(std::string_view text)
        {
            std::string out;
            out.reserve(text.size());
            for (char ch : text)
            {
                if (std::isalnum(static_cast<unsigned char>(ch)) != 0)
                {
                    out.push_back(ch);
                }
                else
                {
                    out.push_back('_');
                }
            }
            if (out.empty())
            {
                out = "unnamed";
            }
            if (std::isdigit(static_cast<unsigned char>(out.front())) != 0)
            {
                out.insert(out.begin(), '_');
            }
            return out;
        }

        std::string joinStrings(const std::vector<std::string> &items, std::string_view delim)
        {
            std::string out;
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                if (i != 0)
                {
                    out.append(delim);
                }
                out.append(items[i]);
            }
            return out;
        }

        std::string joinInts(const std::vector<int64_t> &items, std::string_view delim)
        {
            std::string out;
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                if (i != 0)
                {
                    out.append(delim);
                }
                out.append(std::to_string(items[i]));
            }
            return out;
        }

        std::optional<GsimScratchpadMetadata> loadMetadata(const wolvrix::lib::grh::Design &design,
                                                           const std::string &scratchPrefix,
                                                           EmitDiagnostics *diagnostics)
        {
            auto reportError = [&](std::string message, std::string context = {})
            {
                if (diagnostics != nullptr)
                {
                    diagnostics->error(std::move(message), std::move(context));
                }
            };

            auto require = [&](auto *ptr, std::string_view suffix, std::string_view expectedType) -> bool
            {
                if (ptr != nullptr)
                {
                    return true;
                }
                std::string key = scratchPrefix;
                key.append(suffix);
                if (design.hasScratchpad(key))
                {
                    reportError("gsim scratchpad metadata has unexpected type", key + " expected " + std::string(expectedType));
                }
                else
                {
                    reportError("missing required gsim scratchpad metadata", key);
                }
                return false;
            };

            const auto *roots = design.getScratchpad<std::vector<int64_t>>(scratchPrefix + ".roots");
            const auto *eventGroups = design.getScratchpad<std::map<std::string, std::vector<int64_t>>>(scratchPrefix + ".event_groups");
            const auto *eventGroupNames = design.getScratchpad<std::vector<std::string>>(scratchPrefix + ".event_group_names");
            const auto *scheduleActivityOrder = design.getScratchpad<std::vector<std::string>>(scratchPrefix + ".schedule.activity_order");
            const auto *scheduleActivityMembers = design.getScratchpad<std::map<std::string, std::vector<int64_t>>>(scratchPrefix + ".schedule.activity_members");
            const auto *scheduleActivityClasses = design.getScratchpad<std::map<std::string, std::string>>(scratchPrefix + ".schedule.activity_classes");
            const auto *hypergraphNodeNames = design.getScratchpad<std::vector<std::string>>(scratchPrefix + ".hypergraph.node_names");
            const auto *hypergraphNodeMembers = design.getScratchpad<std::map<std::string, std::vector<int64_t>>>(scratchPrefix + ".hypergraph.node_members");
            const auto *hypergraphEdgeNames = design.getScratchpad<std::vector<std::string>>(scratchPrefix + ".hypergraph.edge_names");
            const auto *hypergraphEdgeSources = design.getScratchpad<std::map<std::string, std::string>>(scratchPrefix + ".hypergraph.edge_sources");
            const auto *hypergraphEdgeTargets = design.getScratchpad<std::map<std::string, std::string>>(scratchPrefix + ".hypergraph.edge_targets");
            const auto *hypergraphEdgeSinks = design.getScratchpad<std::map<std::string, std::vector<int64_t>>>(scratchPrefix + ".hypergraph.edge_sinks");
            const auto *topoOrder = design.getScratchpad<std::vector<int64_t>>(scratchPrefix + ".topology.order");
            const auto *predecessors = design.getScratchpad<std::map<int64_t, std::vector<int64_t>>>(scratchPrefix + ".topology.predecessors");
            const auto *successors = design.getScratchpad<std::map<int64_t, std::vector<int64_t>>>(scratchPrefix + ".topology.successors");
            const auto *classifications = design.getScratchpad<std::map<int64_t, std::string>>(scratchPrefix + ".ops.classification");
            const auto *opDescriptors = design.getScratchpad<std::vector<std::string>>(scratchPrefix + ".ops.descriptors");
            const auto *scheduleKind = design.getScratchpad<std::string>(scratchPrefix + ".schedule.kind");
            const auto *scheduleVersion = design.getScratchpad<int64_t>(scratchPrefix + ".schedule.version");
            const auto *scheduleContract = design.getScratchpad<std::string>(scratchPrefix + ".schedule.contract");
            const auto *hypergraphKind = design.getScratchpad<std::string>(scratchPrefix + ".hypergraph.kind");
            const auto *hypergraphVersion = design.getScratchpad<int64_t>(scratchPrefix + ".hypergraph.version");
            const auto *hypergraphContract = design.getScratchpad<std::string>(scratchPrefix + ".hypergraph.contract");
            const auto *graphSymbol = design.getScratchpad<std::string>(scratchPrefix + ".graph_symbol");
            const auto *opCount = design.getScratchpad<int64_t>(scratchPrefix + ".op_count");
            const auto *graphRevision = design.getScratchpad<int64_t>(scratchPrefix + ".graph_revision");

            bool ok = true;
            ok = require(roots, ".roots", "vector<int64_t>") && ok;
            ok = require(eventGroups, ".event_groups", "map<string, vector<int64_t>>") && ok;
            ok = require(eventGroupNames, ".event_group_names", "vector<string>") && ok;
            ok = require(scheduleActivityOrder, ".schedule.activity_order", "vector<string>") && ok;
            ok = require(scheduleActivityMembers, ".schedule.activity_members", "map<string, vector<int64_t>>") && ok;
            ok = require(scheduleActivityClasses, ".schedule.activity_classes", "map<string, string>") && ok;
            ok = require(hypergraphNodeNames, ".hypergraph.node_names", "vector<string>") && ok;
            ok = require(hypergraphNodeMembers, ".hypergraph.node_members", "map<string, vector<int64_t>>") && ok;
            ok = require(hypergraphEdgeNames, ".hypergraph.edge_names", "vector<string>") && ok;
            ok = require(hypergraphEdgeSources, ".hypergraph.edge_sources", "map<string, string>") && ok;
            ok = require(hypergraphEdgeTargets, ".hypergraph.edge_targets", "map<string, string>") && ok;
            ok = require(hypergraphEdgeSinks, ".hypergraph.edge_sinks", "map<string, vector<int64_t>>") && ok;
            ok = require(topoOrder, ".topology.order", "vector<int64_t>") && ok;
            ok = require(predecessors, ".topology.predecessors", "map<int64_t, vector<int64_t>>") && ok;
            ok = require(successors, ".topology.successors", "map<int64_t, vector<int64_t>>") && ok;
            ok = require(classifications, ".ops.classification", "map<int64_t, string>") && ok;
            ok = require(opDescriptors, ".ops.descriptors", "vector<string>") && ok;
            ok = require(scheduleKind, ".schedule.kind", "string") && ok;
            ok = require(scheduleVersion, ".schedule.version", "int64_t") && ok;
            ok = require(scheduleContract, ".schedule.contract", "string") && ok;
            ok = require(hypergraphKind, ".hypergraph.kind", "string") && ok;
            ok = require(hypergraphVersion, ".hypergraph.version", "int64_t") && ok;
            ok = require(hypergraphContract, ".hypergraph.contract", "string") && ok;
            ok = require(graphSymbol, ".graph_symbol", "string") && ok;
            ok = require(opCount, ".op_count", "int64_t") && ok;
            ok = require(graphRevision, ".graph_revision", "int64_t") && ok;
            if (!ok)
            {
                return std::nullopt;
            }

            GsimScratchpadMetadata metadata{*roots,
                                            *eventGroups,
                                            *eventGroupNames,
                                            *scheduleActivityOrder,
                                            *scheduleActivityMembers,
                                            *scheduleActivityClasses,
                                            *hypergraphNodeNames,
                                            *hypergraphNodeMembers,
                                            *hypergraphEdgeNames,
                                            *hypergraphEdgeSources,
                                            *hypergraphEdgeTargets,
                                            *hypergraphEdgeSinks,
                                            *topoOrder,
                                            *predecessors,
                                            *successors,
                                            *classifications,
                                            *opDescriptors,
                                            *scheduleKind,
                                            *scheduleVersion,
                                            *scheduleContract,
                                            *hypergraphKind,
                                            *hypergraphVersion,
                                            *hypergraphContract,
                                            *graphSymbol,
                                            *opCount,
                                            *graphRevision};

            if (metadata.graphSymbol.empty())
            {
                reportError("gsim scratchpad metadata is malformed", scratchPrefix + ".graph_symbol is empty");
                return std::nullopt;
            }
            if (metadata.opCount < 0)
            {
                reportError("gsim scratchpad metadata is malformed", scratchPrefix + ".op_count must be non-negative");
                return std::nullopt;
            }
            if (metadata.graphRevision < 0)
            {
                reportError("gsim scratchpad metadata is malformed", scratchPrefix + ".graph_revision must be non-negative");
                return std::nullopt;
            }
            if (metadata.scheduleVersion <= 0 || metadata.hypergraphVersion <= 0)
            {
                reportError("gsim scratchpad metadata is malformed", scratchPrefix + " metadata version must be positive");
                return std::nullopt;
            }
            if (metadata.scheduleKind != "activity-v1" || metadata.scheduleContract != "gsim.activity.schedule.v1")
            {
                reportError("gsim scratchpad metadata is malformed", scratchPrefix + " schedule metadata contract mismatch");
                return std::nullopt;
            }
            if (metadata.hypergraphKind != "activity-connectivity-v1" || metadata.hypergraphContract != "gsim.activity.hypergraph.v1")
            {
                reportError("gsim scratchpad metadata is malformed", scratchPrefix + " hypergraph metadata contract mismatch");
                return std::nullopt;
            }
            if (metadata.topoOrder.size() != static_cast<std::size_t>(metadata.opCount) ||
                metadata.classifications.size() != static_cast<std::size_t>(metadata.opCount) ||
                metadata.predecessors.size() != static_cast<std::size_t>(metadata.opCount) ||
                metadata.successors.size() != static_cast<std::size_t>(metadata.opCount) ||
                metadata.opDescriptors.size() != static_cast<std::size_t>(metadata.opCount))
            {
                reportError("gsim scratchpad metadata is incomplete",
                            scratchPrefix + " op_count does not match topology/classification/adjacency metadata");
                return std::nullopt;
            }

            std::set<int64_t> topoIds(metadata.topoOrder.begin(), metadata.topoOrder.end());
            if (topoIds.size() != metadata.topoOrder.size())
            {
                reportError("gsim scratchpad metadata is malformed", scratchPrefix + ".topology.order contains duplicates");
                return std::nullopt;
            }
            for (int64_t id : metadata.topoOrder)
            {
                if (metadata.classifications.count(id) == 0 ||
                    metadata.predecessors.count(id) == 0 ||
                    metadata.successors.count(id) == 0)
                {
                    reportError("gsim scratchpad metadata is incomplete",
                                scratchPrefix + " topology ids must have classification and adjacency entries");
                    return std::nullopt;
                }
            }
            for (const auto &groupName : metadata.eventGroupNames)
            {
                if (metadata.eventGroups.count(groupName) == 0)
                {
                    reportError("gsim scratchpad metadata is incomplete",
                                scratchPrefix + ".event_group_names references missing event_groups entry");
                    return std::nullopt;
                }
            }
            if (metadata.scheduleActivityOrder.empty() || metadata.scheduleActivityOrder.size() != metadata.eventGroupNames.size())
            {
                reportError("gsim scratchpad metadata is incomplete",
                            scratchPrefix + ".schedule.activity_order must describe every event group");
                return std::nullopt;
            }
            for (const auto &activityName : metadata.scheduleActivityOrder)
            {
                if (metadata.scheduleActivityMembers.count(activityName) == 0 ||
                    metadata.scheduleActivityClasses.count(activityName) == 0)
                {
                    reportError("gsim scratchpad metadata is incomplete",
                                scratchPrefix + ".schedule activity entries must provide members and classes");
                    return std::nullopt;
                }
            }
            if (metadata.hypergraphNodeNames.size() != metadata.eventGroupNames.size() ||
                metadata.hypergraphEdgeNames.size() != metadata.eventGroupNames.size())
            {
                reportError("gsim scratchpad metadata is incomplete",
                            scratchPrefix + ".hypergraph node/edge counts must align with event groups");
                return std::nullopt;
            }
            for (const auto &nodeName : metadata.hypergraphNodeNames)
            {
                if (metadata.hypergraphNodeMembers.count(nodeName) == 0)
                {
                    reportError("gsim scratchpad metadata is incomplete",
                                scratchPrefix + ".hypergraph.node_names references missing node_members entry");
                    return std::nullopt;
                }
            }
            for (const auto &edgeName : metadata.hypergraphEdgeNames)
            {
                if (metadata.hypergraphEdgeSources.count(edgeName) == 0 ||
                    metadata.hypergraphEdgeTargets.count(edgeName) == 0 ||
                    metadata.hypergraphEdgeSinks.count(edgeName) == 0)
                {
                    reportError("gsim scratchpad metadata is incomplete",
                                scratchPrefix + ".hypergraph edges must define source, target, and sinks");
                    return std::nullopt;
                }
            }

            return metadata;
        }

        struct EmitTarget
        {
            const wolvrix::lib::grh::Graph *graph = nullptr;
            std::string selectionPath;
            std::string scratchGraphSymbol;
            std::string namespacePath;
        };

        std::optional<EmitTarget> resolveEmitTarget(const wolvrix::lib::grh::Design &design,
                                                    const std::span<const wolvrix::lib::grh::Graph *const> topGraphs,
                                                    const EmitOptions &options,
                                                    EmitDiagnostics *diagnostics)
        {
            auto reportError = [&](std::string message, std::string context = {})
            {
                if (diagnostics != nullptr)
                {
                    diagnostics->error(std::move(message), std::move(context));
                }
            };

            if (topGraphs.empty())
            {
                reportError("No top graphs available for emission");
                return std::nullopt;
            }

            if (const auto path = attrValue(options, "path"))
            {
                auto &mutableDesign = const_cast<wolvrix::lib::grh::Design &>(design);
                std::string errorText;
                auto resolved = wolvrix::lib::transform::resolveTargetPath(mutableDesign,
                                                                           *path,
                                                                           wolvrix::lib::transform::TargetPathRequirement::GraphOnlyOrInstancePath,
                                                                           errorText);
                if (!resolved)
                {
                    reportError("failed to resolve gsim emit target path", errorText);
                    return std::nullopt;
                }
                return EmitTarget{resolved->targetGraph,
                                  *path,
                                  resolved->targetGraph->symbol(),
                                  resolved->scratchpadNamespace};
            }

            if (topGraphs.size() != 1)
            {
                reportError("EmitGsimCpp requires exactly one selected top graph when no path attribute is provided");
                return std::nullopt;
            }
            return EmitTarget{topGraphs.front(),
                              topGraphs.front()->symbol(),
                              topGraphs.front()->symbol(),
                              "gsim." + std::string(topGraphs.front()->symbol())};
        }

        std::string defaultBaseName(const EmitTarget &target)
        {
            return "gsim_" + sanitizeIdentifier(target.scratchGraphSymbol);
        }

        bool attrEnabled(const EmitOptions &options, std::string_view key, bool defaultValue)
        {
            const auto value = attrValue(options, key);
            if (!value)
            {
                return defaultValue;
            }
            std::string lowered = *value;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            if (lowered == "0" || lowered == "false" || lowered == "off" || lowered == "no")
            {
                return false;
            }
            if (lowered == "1" || lowered == "true" || lowered == "on" || lowered == "yes")
            {
                return true;
            }
            return defaultValue;
        }

        int parsePositiveIntAttr(const EmitOptions &options, std::string_view key, int defaultValue)
        {
            const auto value = attrValue(options, key);
            if (!value)
            {
                return defaultValue;
            }
            try
            {
                const int parsed = std::stoi(*value);
                return parsed > 0 ? parsed : defaultValue;
            }
            catch (...)
            {
                return defaultValue;
            }
        }

        std::optional<std::pair<std::string, std::string>> parseSequentialDomain(std::string_view key)
        {
            const std::size_t pos = key.find(':');
            if (pos == std::string_view::npos || pos == 0 || pos + 1 >= key.size())
            {
                return std::nullopt;
            }
            return std::make_pair(std::string(key.substr(0, pos)), std::string(key.substr(pos + 1)));
        }

        void writeHeader(std::ostream &os,
                         const EmitTarget &target,
                         const GsimScratchpadMetadata &metadata,
                         const CodegenState& state,
                         bool emitMetadata)
        {
            const std::string ns = sanitizeIdentifier(target.scratchGraphSymbol);
            const std::string structName = "GsimMetadata_" + ns;

            os << "#pragma once\n\n";
            os << "#include <cstdint>\n";
            os << "#include <map>\n";
            os << "#include <stdexcept>\n";
            os << "#include <string>\n";
            os << "#include <type_traits>\n";
            os << "#include <vector>\n\n";
            os << "inline std::uint64_t wolvrix_gsim_slice_dynamic_to_u64(\n";
            os << "    const std::vector<std::uint64_t>& value,\n";
            os << "    std::uint64_t bitIndex,\n";
            os << "    std::uint32_t sliceWidth,\n";
            os << "    std::uint32_t operandWidth) {\n";
            os << "    if (sliceWidth == 0 || sliceWidth > 64 || bitIndex >= operandWidth) {\n";
            os << "        return 0ULL;\n";
            os << "    }\n";
            os << "    const auto availableBits = std::min<std::uint64_t>(sliceWidth, operandWidth - bitIndex);\n";
            os << "    std::uint64_t result = 0ULL;\n";
            os << "    for (std::uint64_t offset = 0; offset < availableBits; ++offset) {\n";
            os << "        const std::uint64_t absoluteBit = bitIndex + offset;\n";
            os << "        const auto wordIndex = static_cast<std::size_t>(absoluteBit / 64ULL);\n";
            os << "        const auto bitOffset = static_cast<std::uint32_t>(absoluteBit % 64ULL);\n";
            os << "        const std::uint64_t word = wordIndex < value.size() ? value[wordIndex] : 0ULL;\n";
            os << "        result |= ((word >> bitOffset) & 1ULL) << offset;\n";
            os << "    }\n";
            os << "    return result;\n";
            os << "}\n\n";
            os << "template <typename T>\n";
            os << "inline void wolvrix_gsim_store_bits(std::vector<std::uint64_t>& out, std::uint64_t bitOffset, const T& value, std::uint32_t width) {\n";
            os << "    static_assert(std::is_integral_v<T> || std::is_enum_v<T>);\n";
            os << "    const std::uint64_t word = static_cast<std::uint64_t>(value);\n";
            os << "    const auto availableBits = std::min<std::uint32_t>(width, 64U);\n";
            os << "    for (std::uint32_t i = 0; i < availableBits; ++i) {\n";
            os << "        if (((word >> i) & 1ULL) == 0ULL) continue;\n";
            os << "        const std::uint64_t absoluteBit = bitOffset + i;\n";
            os << "        const auto wordIndex = static_cast<std::size_t>(absoluteBit / 64ULL);\n";
            os << "        const auto bitInWord = static_cast<std::uint32_t>(absoluteBit % 64ULL);\n";
            os << "        if (wordIndex < out.size()) out[wordIndex] |= (1ULL << bitInWord);\n";
            os << "    }\n";
            os << "}\n";
            os << "inline void wolvrix_gsim_store_bits(std::vector<std::uint64_t>& out, std::uint64_t bitOffset, const std::vector<std::uint64_t>& value, std::uint32_t width) {\n";
            os << "    for (std::uint32_t i = 0; i < width; ++i) {\n";
            os << "        const auto srcWord = static_cast<std::size_t>(i / 64U);\n";
            os << "        const auto srcBit = static_cast<std::uint32_t>(i % 64U);\n";
            os << "        const std::uint64_t word = srcWord < value.size() ? value[srcWord] : 0ULL;\n";
            os << "        if (((word >> srcBit) & 1ULL) == 0ULL) continue;\n";
            os << "        const std::uint64_t absoluteBit = bitOffset + i;\n";
            os << "        const auto dstWord = static_cast<std::size_t>(absoluteBit / 64ULL);\n";
            os << "        const auto dstBit = static_cast<std::uint32_t>(absoluteBit % 64ULL);\n";
            os << "        if (dstWord < out.size()) out[dstWord] |= (1ULL << dstBit);\n";
            os << "    }\n";
            os << "}\n";
            os << "template <typename L, typename R>\n";
            os << "inline std::vector<std::uint64_t> wolvrix_gsim_concat(const L& lhs, std::uint32_t lhsWidth, const R& rhs, std::uint32_t rhsWidth) {\n";
            os << "    std::vector<std::uint64_t> result((static_cast<std::uint64_t>(lhsWidth) + rhsWidth + 63ULL) / 64ULL, 0ULL);\n";
            os << "    wolvrix_gsim_store_bits(result, 0ULL, rhs, rhsWidth);\n";
            os << "    wolvrix_gsim_store_bits(result, rhsWidth, lhs, lhsWidth);\n";
            os << "    return result;\n";
            os << "}\n\n";

            // Generated Simulator Class (use SSimTop for compatibility)
            os << "class SSimTop {\n";
            os << "public:\n";
            os << "    SSimTop() { reset(); }\n";
            os << "    ~SSimTop() = default;\n\n";

            // Reset and set_reset (for compatibility)
            os << "    void set_reset(unsigned reset) { reset_ = reset; }\n\n";
            os << "    void reset() {\n";
            os << "        reset_ = false;\n";
            for (const auto& stmt : state.storageResetStmts) {
                os << "        " << stmt << "\n";
            }
            for (const auto& [name, type] : state.outputPorts) {
                (void)type;
                os << "        output_" << sanitizeIdentifier(name) << "_ = " << zeroInitializerForType(type) << ";\n";
            }
            for (const auto &domain : state.sequentialStmts) {
                const auto parsedDomain = parseSequentialDomain(domain.first);
                if (parsedDomain) {
                    const std::string resetClock =
                        resolveSequentialClockStateName(parsedDomain->second, state.inputPorts);
                    os << "        prev_" << resetClock << "_ = false;\n";
                }
            }
            os << "    }\n\n";

            os << "    void settle() {\n";
            if (!state.latchStmts.empty()) {
                os << "        if (!reset_) {\n";
                for (const auto &stmt : state.latchStmts) {
                    os << stmt << "\n";
                }
                os << "        }\n";
            }
            if (!state.outputPorts.empty()) {
                for (const auto &[valueId, portInfo] : state.outputPortValues) {
                    auto valueIt = state.valueVars.find(valueId);
                    const std::string expr = valueIt != state.valueVars.end() ? valueIt->second : "0";
                    os << "        output_" << sanitizeIdentifier(portInfo.first) << "_ = " << expr << ";\n";
                }
            }
            os << "    }\n\n";

            os << "    void commit_step() {\n";
            os << "        bool committed_ = false;\n";
            if (!state.sequentialStmts.empty()) {
                if (state.sequentialStmts.size() > 1) {
                    os << "        throw std::runtime_error(\"GSIM emitted runtime supports only one clock domain\");\n";
                } else {
                    const auto &domain = *state.sequentialStmts.begin();
                    const auto parsedDomain = parseSequentialDomain(domain.first);
                    if (!parsedDomain) {
                        os << "        throw std::runtime_error(\"GSIM emitted runtime encountered malformed clock domain metadata\");\n";
                    } else {
                        const std::string edge = parsedDomain->first;
                        if (edge != "posedge" && edge != "negedge") {
                            os << "        throw std::runtime_error(\"GSIM emitted runtime supports only posedge/negedge event edges\");\n";
                        } else {
                            std::string resolvedClock =
                                resolveSequentialClockStateName(parsedDomain->second, state.inputPorts);
                            std::string currClockExpr;
                            auto exprIt = state.sequentialClockExprs.find(domain.first);
                            if (exprIt != state.sequentialClockExprs.end()) {
                                currClockExpr = exprIt->second;
                            } else {
                                currClockExpr = "input_" + resolvedClock + "_";
                            }
                            const std::string prevClock = "prev_" + resolvedClock + "_";
                            const std::string edgeExpr = edge == "posedge"
                                                             ? "(!" + prevClock + " && static_cast<bool>(" + currClockExpr + "))"
                                                             : "(" + prevClock + " && !static_cast<bool>(" + currClockExpr + "))";
                            os << "        if (reset_) {\n";
                            if (!state.outputPorts.empty()) {
                                os << "            settle();\n";
                            }
                            os << "            reset_ = false;\n";
                            os << "            " << prevClock << " = static_cast<bool>(" << currClockExpr << ");\n";
                            os << "            difftest_exit_ = 0;\n";
                            os << "            return;\n";
                            os << "        }\n";
                            os << "        if (" << edgeExpr << ") {\n";
                            auto regIt = state.sequentialRegs.find(domain.first);
                            if (regIt != state.sequentialRegs.end()) {
                                for (const auto &regName : regIt->second) {
                                    os << "            auto next_" << regName << " = " << regName << ";\n";
                                }
                            }
                            for (const auto& stmt : domain.second) {
                                os << stmt << "\n";
                            }
                            if (regIt != state.sequentialRegs.end()) {
                                for (const auto &regName : regIt->second) {
                                    os << "            " << regName << " = next_" << regName << ";\n";
                                }
                            }
                            os << "        }\n";
                            os << "        " << prevClock << " = static_cast<bool>(" << currClockExpr << ");\n";
                        }
                    }
                }
            } else {
                os << "        if (reset_) {\n";
                os << "            settle();\n";
                os << "            reset_ = false;\n";
                os << "            difftest_exit_ = 0;\n";
                os << "            return;\n";
                os << "        }\n";
            }

            os << "        settle();\n";
            os << "        if (committed_) { ++difftest_step_; }\n";
            os << "        difftest_exit_ = 0;\n";
            os << "    }\n\n";

            os << "    void step() {\n";
            os << "        settle();\n";
            os << "        commit_step();\n";
            os << "    }\n\n";

            // Input port setters
            for (const auto& [name, type] : state.inputPorts) {
                std::string methodName = "set_" + sanitizeIdentifier(name);
                os << "    void " << methodName << "(" << type << " value) { input_" << sanitizeIdentifier(name) << "_ = value; }\n";
            }
            if (!state.inputPorts.empty()) os << "\n";

            // Output port getters
            for (const auto& [name, type] : state.outputPorts) {
                std::string methodName = "get_" + sanitizeIdentifier(name);
                os << "    " << type << " " << methodName << "() const { return output_" << sanitizeIdentifier(name) << "_; }\n";
            }
            if (!state.outputPorts.empty()) os << "\n";

            // Difftest stub accessors (for compatibility)
            os << "    unsigned get_difftest__DOT__uart__DOT__out__DOT__valid() const { return 0; }\n";
            os << "    std::uint8_t get_difftest__DOT__uart__DOT__out__DOT__ch() const { return 0; }\n";
            os << "    unsigned get_difftest__DOT__uart__DOT__in__DOT__valid() const { return 0; }\n";
            os << "    void set_difftest__DOT__uart__DOT__in__DOT__ch(std::uint8_t) { }\n";
            os << "    std::uint64_t get_difftest__DOT__exit() const { return difftest_exit_; }\n";
            os << "    std::uint64_t get_difftest__DOT__step() const { return difftest_step_; }\n";
            os << "    void set_difftest__DOT__perfCtrl__DOT__clean(unsigned clean) { perf_clean_ = clean; }\n";
            os << "    void set_difftest__DOT__perfCtrl__DOT__dump(unsigned dump) { perf_dump_ = dump; }\n";
            os << "    void set_difftest__DOT__logCtrl__DOT__begin(std::uint64_t begin) { log_begin_ = begin; }\n";
            os << "    void set_difftest__DOT__logCtrl__DOT__end(std::uint64_t end) { log_end_ = end; }\n\n";

            os << "private:\n";
            os << "    bool reset_ = false;\n";

            // Input port storage
            for (const auto& [name, type] : state.inputPorts) {
                os << "    " << type << " input_" << sanitizeIdentifier(name) << "_ = " << zeroInitializerForType(type) << ";\n";
            }

            // Output port storage
            for (const auto& [name, type] : state.outputPorts) {
                os << "    " << type << " output_" << sanitizeIdentifier(name) << "_ = " << zeroInitializerForType(type) << ";\n";
            }

            // Register storage
            for (const auto& decl : state.storageDecls) {
                os << "    " << decl << "\n";
            }

            // Difftest state (for compatibility)
            os << "    std::uint64_t difftest_exit_ = 0;\n";
            os << "    std::uint64_t difftest_step_ = 0;\n";
            os << "    unsigned perf_clean_ = 0;\n";
            os << "    unsigned perf_dump_ = 0;\n";
            os << "    std::uint64_t log_begin_ = 0;\n";
            os << "    std::uint64_t log_end_ = 0;\n";
            for (const auto &domain : state.sequentialStmts) {
                const auto parsedDomain = parseSequentialDomain(domain.first);
                if (parsedDomain) {
                    const std::string prevClockName =
                        resolveSequentialClockStateName(parsedDomain->second, state.inputPorts);
                    os << "    bool prev_" << prevClockName << "_ = false;\n";
                }
            }

            os << "};\n\n";

            // Metadata struct (kept for compatibility)
            os << "namespace wolvrix::gsim {\n\n";
            os << "struct " << structName << " {\n";
            os << "    std::string graph_symbol;\n";
            os << "    std::string selection_path;\n";
            os << "    std::string scratchpad_namespace;\n";
            os << "    std::int64_t op_count = 0;\n";
            os << "    std::int64_t graph_revision = 0;\n";
            if (emitMetadata)
            {
                os << "    std::vector<std::int64_t> roots;\n";
                os << "    std::vector<std::int64_t> topo_order;\n";
                os << "    std::vector<std::string> event_group_names;\n";
                os << "    std::map<std::string, std::vector<std::int64_t>> event_groups;\n";
                os << "    std::vector<std::string> schedule_activity_order;\n";
                os << "    std::map<std::string, std::vector<std::int64_t>> schedule_activity_members;\n";
                os << "    std::map<std::string, std::string> schedule_activity_classes;\n";
                os << "    std::vector<std::string> hypergraph_node_names;\n";
                os << "    std::map<std::string, std::vector<std::int64_t>> hypergraph_node_members;\n";
                os << "    std::vector<std::string> hypergraph_edge_names;\n";
                os << "    std::map<std::string, std::string> hypergraph_edge_sources;\n";
                os << "    std::map<std::string, std::string> hypergraph_edge_targets;\n";
                os << "    std::map<std::string, std::vector<std::int64_t>> hypergraph_edge_sinks;\n";
                os << "    std::map<std::int64_t, std::string> classifications;\n";
                os << "    std::map<std::int64_t, std::vector<std::int64_t>> predecessors;\n";
                os << "    std::map<std::int64_t, std::vector<std::int64_t>> successors;\n";
                os << "    std::vector<std::string> op_descriptors;\n";
                os << "    std::string schedule_kind;\n";
                os << "    std::int64_t schedule_version = 0;\n";
                os << "    std::string schedule_contract;\n";
                os << "    std::string hypergraph_kind;\n";
                os << "    std::int64_t hypergraph_version = 0;\n";
                os << "    std::string hypergraph_contract;\n";
            }
            os << "};\n\n";
            os << structName << " make_" << sanitizeIdentifier(target.scratchGraphSymbol) << "_metadata();\n";
            os << "bool validate_" << sanitizeIdentifier(target.scratchGraphSymbol) << "_metadata(const " << structName << "& metadata);\n\n";
            os << "} // namespace wolvrix::gsim\n";
            (void)metadata;
        }

        void writeSource(std::ostream &os,
                         const EmitTarget &target,
                         const GsimScratchpadMetadata &metadata,
                         std::string_view headerFilename,
                         bool emitMetadata)
        {
            const std::string ns = sanitizeIdentifier(target.scratchGraphSymbol);
            const std::string structName = "GsimMetadata_" + ns;
            const std::string factoryName = "make_" + sanitizeIdentifier(target.scratchGraphSymbol) + "_metadata";
            const std::string validateName = "validate_" + sanitizeIdentifier(target.scratchGraphSymbol) + "_metadata";

            os << "#include \"" << headerFilename << "\"\n\n";
            os << "#include <algorithm>\n";
            os << "#include <set>\n\n";
            os << "namespace wolvrix::gsim {\n\n";
            os << "namespace {\n";
            os << "template <typename T>\n";
            os << "bool has_duplicate_values(const std::vector<T>& values) {\n";
            os << "    return std::set<T>(values.begin(), values.end()).size() != values.size();\n";
            os << "}\n";
            os << "} // namespace\n\n";
            os << structName << " " << factoryName << "() {\n";
            os << "    " << structName << " metadata;\n";
            os << "    metadata.graph_symbol = \"" << target.scratchGraphSymbol << "\";\n";
            os << "    metadata.selection_path = \"" << target.selectionPath << "\";\n";
            os << "    metadata.scratchpad_namespace = \"" << target.namespacePath << "\";\n";
            os << "    metadata.op_count = " << metadata.opCount << ";\n";
            os << "    metadata.graph_revision = " << metadata.graphRevision << ";\n";
            if (!emitMetadata)
            {
                os << "    return metadata;\n";
                os << "}\n\n";
                os << "bool " << validateName << "(const " << structName << "& metadata) {\n";
                os << "    return metadata.graph_symbol == \"" << target.scratchGraphSymbol << "\";\n";
                os << "}\n\n";
                os << "} // namespace wolvrix::gsim\n";
                return;
            }
            os << "    metadata.roots = {" << joinInts(metadata.roots, ", ") << "};\n";
            os << "    metadata.topo_order = {" << joinInts(metadata.topoOrder, ", ") << "};\n";
            os << "    metadata.event_group_names = {";
            for (std::size_t i = 0; i < metadata.eventGroupNames.size(); ++i)
            {
                if (i != 0)
                {
                    os << ", ";
                }
                os << '"' << metadata.eventGroupNames[i] << '"';
            }
            os << "};\n";
            os << "    metadata.event_groups = {\n";
            for (const auto &[name, ids] : metadata.eventGroups)
            {
                os << "        {\"" << name << "\", {" << joinInts(ids, ", ") << "}},\n";
            }
            os << "    };\n";
            os << "    metadata.schedule_activity_order = {";
            for (std::size_t i = 0; i < metadata.scheduleActivityOrder.size(); ++i)
            {
                if (i != 0)
                {
                    os << ", ";
                }
                os << '"' << metadata.scheduleActivityOrder[i] << '"';
            }
            os << "};\n";
            os << "    metadata.schedule_activity_members = {\n";
            for (const auto &[name, ids] : metadata.scheduleActivityMembers)
            {
                os << "        {\"" << name << "\", {" << joinInts(ids, ", ") << "}},\n";
            }
            os << "    };\n";
            os << "    metadata.schedule_activity_classes = {\n";
            for (const auto &[name, activityClass] : metadata.scheduleActivityClasses)
            {
                os << "        {\"" << name << "\", \"" << activityClass << "\"},\n";
            }
            os << "    };\n";
            os << "    metadata.hypergraph_node_names = {";
            for (std::size_t i = 0; i < metadata.hypergraphNodeNames.size(); ++i)
            {
                if (i != 0)
                {
                    os << ", ";
                }
                os << '"' << metadata.hypergraphNodeNames[i] << '"';
            }
            os << "};\n";
            os << "    metadata.hypergraph_node_members = {\n";
            for (const auto &[name, ids] : metadata.hypergraphNodeMembers)
            {
                os << "        {\"" << name << "\", {" << joinInts(ids, ", ") << "}},\n";
            }
            os << "    };\n";
            os << "    metadata.hypergraph_edge_names = {";
            for (std::size_t i = 0; i < metadata.hypergraphEdgeNames.size(); ++i)
            {
                if (i != 0)
                {
                    os << ", ";
                }
                os << '"' << metadata.hypergraphEdgeNames[i] << '"';
            }
            os << "};\n";
            os << "    metadata.hypergraph_edge_sources = {\n";
            for (const auto &[name, sourceName] : metadata.hypergraphEdgeSources)
            {
                os << "        {\"" << name << "\", \"" << sourceName << "\"},\n";
            }
            os << "    };\n";
            os << "    metadata.hypergraph_edge_targets = {\n";
            for (const auto &[name, targetName] : metadata.hypergraphEdgeTargets)
            {
                os << "        {\"" << name << "\", \"" << targetName << "\"},\n";
            }
            os << "    };\n";
            os << "    metadata.hypergraph_edge_sinks = {\n";
            for (const auto &[name, ids] : metadata.hypergraphEdgeSinks)
            {
                os << "        {\"" << name << "\", {" << joinInts(ids, ", ") << "}},\n";
            }
            os << "    };\n";
            os << "    metadata.classifications = {\n";
            for (const auto &[id, className] : metadata.classifications)
            {
                os << "        {" << id << ", \"" << className << "\"},\n";
            }
            os << "    };\n";
            os << "    metadata.predecessors = {\n";
            for (const auto &[id, ids] : metadata.predecessors)
            {
                os << "        {" << id << ", {" << joinInts(ids, ", ") << "}},\n";
            }
            os << "    };\n";
            os << "    metadata.successors = {\n";
            for (const auto &[id, ids] : metadata.successors)
            {
                os << "        {" << id << ", {" << joinInts(ids, ", ") << "}},\n";
            }
            os << "    };\n";
            os << "    metadata.op_descriptors = {";
            for (std::size_t i = 0; i < metadata.opDescriptors.size(); ++i)
            {
                if (i != 0)
                {
                    os << ", ";
                }
                os << '"' << metadata.opDescriptors[i] << '"';
            }
            os << "};\n";
            os << "    metadata.schedule_kind = \"" << metadata.scheduleKind << "\";\n";
            os << "    metadata.schedule_version = " << metadata.scheduleVersion << ";\n";
            os << "    metadata.schedule_contract = \"" << metadata.scheduleContract << "\";\n";
            os << "    metadata.hypergraph_kind = \"" << metadata.hypergraphKind << "\";\n";
            os << "    metadata.hypergraph_version = " << metadata.hypergraphVersion << ";\n";
            os << "    metadata.hypergraph_contract = \"" << metadata.hypergraphContract << "\";\n";
            os << "    return metadata;\n";
            os << "}\n\n";
            os << "bool " << validateName << "(const " << structName << "& metadata) {\n";
            os << "    if (metadata.graph_symbol != \"" << target.scratchGraphSymbol << "\") return false;\n";
            os << "    if (metadata.scratchpad_namespace != \"" << target.namespacePath << "\") return false;\n";
            os << "    if (metadata.selection_path.empty()) return false;\n";
            os << "    if (metadata.graph_revision < 0) return false;\n";
            os << "    if (metadata.op_count < 0) return false;\n";
            os << "    if (metadata.schedule_kind != \"activity-v1\") return false;\n";
            os << "    if (metadata.schedule_contract != \"gsim.activity.schedule.v1\") return false;\n";
            os << "    if (metadata.hypergraph_kind != \"activity-connectivity-v1\") return false;\n";
            os << "    if (metadata.hypergraph_contract != \"gsim.activity.hypergraph.v1\") return false;\n";
            os << "    if (metadata.schedule_version <= 0 || metadata.hypergraph_version <= 0) return false;\n";
            os << "    if (metadata.topo_order.size() != static_cast<std::size_t>(metadata.op_count)) return false;\n";
            os << "    if (metadata.classifications.size() != static_cast<std::size_t>(metadata.op_count)) return false;\n";
            os << "    if (metadata.predecessors.size() != static_cast<std::size_t>(metadata.op_count)) return false;\n";
            os << "    if (metadata.successors.size() != static_cast<std::size_t>(metadata.op_count)) return false;\n";
            os << "    if (metadata.op_descriptors.size() != static_cast<std::size_t>(metadata.op_count)) return false;\n";
            os << "    if (metadata.schedule_activity_order.size() != metadata.event_group_names.size()) return false;\n";
            os << "    if (metadata.hypergraph_node_names.size() != metadata.event_group_names.size()) return false;\n";
            os << "    if (metadata.hypergraph_edge_names.size() != metadata.event_group_names.size()) return false;\n";
            os << "    if (has_duplicate_values(metadata.topo_order)) return false;\n";
            os << "    if (has_duplicate_values(metadata.schedule_activity_order)) return false;\n";
            os << "    if (has_duplicate_values(metadata.hypergraph_node_names)) return false;\n";
            os << "    if (has_duplicate_values(metadata.hypergraph_edge_names)) return false;\n";
            os << "    for (const auto& name : metadata.event_group_names) {\n";
            os << "        if (metadata.event_groups.find(name) == metadata.event_groups.end()) return false;\n";
            os << "    }\n";
            os << "    for (const auto& activity : metadata.schedule_activity_order) {\n";
            os << "        if (metadata.schedule_activity_members.find(activity) == metadata.schedule_activity_members.end()) return false;\n";
            os << "        if (metadata.schedule_activity_classes.find(activity) == metadata.schedule_activity_classes.end()) return false;\n";
            os << "    }\n";
            os << "    for (const auto& node : metadata.hypergraph_node_names) {\n";
            os << "        if (metadata.hypergraph_node_members.find(node) == metadata.hypergraph_node_members.end()) return false;\n";
            os << "    }\n";
            os << "    for (const auto& edge : metadata.hypergraph_edge_names) {\n";
            os << "        if (metadata.hypergraph_edge_sources.find(edge) == metadata.hypergraph_edge_sources.end()) return false;\n";
            os << "        if (metadata.hypergraph_edge_targets.find(edge) == metadata.hypergraph_edge_targets.end()) return false;\n";
            os << "        if (metadata.hypergraph_edge_sinks.find(edge) == metadata.hypergraph_edge_sinks.end()) return false;\n";
            os << "    }\n";
            os << "    for (std::int64_t id : metadata.topo_order) {\n";
            os << "        if (metadata.classifications.find(id) == metadata.classifications.end()) return false;\n";
            os << "        if (metadata.predecessors.find(id) == metadata.predecessors.end()) return false;\n";
            os << "        if (metadata.successors.find(id) == metadata.successors.end()) return false;\n";
            os << "    }\n";
            os << "    return true;\n";
            os << "}\n\n";
            os << "} // namespace wolvrix::gsim\n";
        }
    } // namespace

    EmitResult EmitGsimCpp::emitImpl(const wolvrix::lib::grh::Design &design,
                                     std::span<const wolvrix::lib::grh::Graph *const> topGraphs,
                                     const EmitOptions &options)
    {
        EmitResult result;

        const auto target = resolveEmitTarget(design, topGraphs, options, diagnostics());
        if (!target)
        {
            result.success = false;
            return result;
        }

        if (target->graph == nullptr)
        {
            reportError("EmitGsimCpp resolved a null target graph");
            result.success = false;
            return result;
        }

        const auto metadata = loadMetadata(design, target->namespacePath, diagnostics());
        if (!metadata)
        {
            result.success = false;
            return result;
        }

        if (metadata->graphSymbol != target->graph->symbol() || metadata->graphSymbol != target->scratchGraphSymbol)
        {
            reportError("gsim scratchpad namespace/path mismatch",
                        target->namespacePath + " resolved graph=" + target->graph->symbol() + " metadata graph=" + metadata->graphSymbol);
            result.success = false;
            return result;
        }
        if (metadata->graphRevision != static_cast<int64_t>(target->graph->revision()))
        {
            reportError("gsim scratchpad metadata is stale",
                        target->namespacePath + " recorded graph revision does not match current graph state");
            result.success = false;
            return result;
        }

        const std::filesystem::path outputDir = resolveOutputDir(options);
        const std::string baseName = options.outputFilename && !options.outputFilename->empty()
                                         ? sanitizeIdentifier(std::filesystem::path(*options.outputFilename).stem().string())
                                         : defaultBaseName(*target);
        const std::filesystem::path headerPath = outputDir / (baseName + ".hpp");
        const std::filesystem::path sourcePath = outputDir / (baseName + ".cpp");
        const std::filesystem::path manifestPath = outputDir / (baseName + ".manifest");

        // Generate code from GRH operations
        CodegenState state;
        state.maxShardSize = parsePositiveIntAttr(options, "behavior_shard_max_bytes", state.maxShardSize);
        const bool emitMetadata = attrEnabled(options, "emit_metadata", true);

        // Determine if sharding should be enabled based on operation count
        // For large designs (>1000 operations), enable sharding to improve compile times
        state.enableSharding = metadata->opCount > 1000;

        collectPorts(*target->graph, state, options.portOrderStrategy, options.portOrderNames);
        collectRegisters(*target->graph, state);
        collectLatches(*target->graph, state);
        collectMemories(*target->graph, state);

        std::unordered_map<std::int64_t, wolvrix::lib::grh::OperationId> opIdByIndex;
        opIdByIndex.reserve(target->graph->operations().size());
        for (const auto &opId : target->graph->operations()) {
            opIdByIndex.emplace(static_cast<std::int64_t>(opId.index), opId);
        }

        // Traverse operations in topo order
        for (int64_t opIdx : metadata->topoOrder) {
            auto opIdIt = opIdByIndex.find(opIdx);
            if (opIdIt == opIdByIndex.end()) {
                continue;
            }
            auto op = target->graph->getOperation(opIdIt->second);
            lowerOperation(*target->graph, op, *metadata, state, diagnostics());
        }

        // Check for unsupported operations
        if (!state.unsupportedOps.empty()) {
            std::string msg = "unsupported operations encountered: ";
            for (size_t i = 0; i < state.unsupportedOps.size() && i < 5; ++i) {
                if (i > 0) msg += ", ";
                msg += state.unsupportedOps[i];
            }
            if (state.unsupportedOps.size() > 5) {
                msg += " and " + std::to_string(state.unsupportedOps.size() - 5) + " more";
            }
            reportError(msg, target->graph->symbol());
            result.success = false;
            return result;
        }

        auto header = openOutputFile(headerPath);
        auto source = openOutputFile(sourcePath);
        if (!header || !source)
        {
            result.success = false;
            return result;
        }

        writeHeader(*header, *target, *metadata, state, emitMetadata);
        writeSource(*source, *target, *metadata, headerPath.filename().string(), emitMetadata);

        std::vector<std::string> manifestEntries;
        manifestEntries.push_back(sourcePath.filename().string());

        // Write out all shard files if sharding is enabled and there are any
        if (state.enableSharding && !state.shardStreams.empty()) {
            for (size_t i = 0; i < state.shardStreams.size(); ++i) {
                std::string shardFileName = baseName + "_sched_" + std::to_string(i) + ".cpp";
                std::filesystem::path shardPath = outputDir / shardFileName;

                auto shardFile = openOutputFile(shardPath);
                if (shardFile) {
                    *shardFile << "#include \"" << headerPath.filename().string() << "\"\n\n";
                    *shardFile << "// Shard " << i << " of combinational logic\n";
                    *shardFile << state.shardStreams[i]->str();
                    result.artifacts.push_back(shardPath.string());
                    manifestEntries.push_back(shardFileName);
                }
            }
        }

        auto manifest = openOutputFile(manifestPath);
        if (!manifest)
        {
            result.success = false;
            return result;
        }
        for (const auto &entry : manifestEntries)
        {
            *manifest << entry << '\n';
        }

        // Add the main header and source files to artifacts
        result.artifacts.push_back(headerPath.string());
        result.artifacts.push_back(sourcePath.string());
        result.artifacts.push_back(manifestPath.string());
        return result;
    }

} // namespace wolvrix::lib::emit
