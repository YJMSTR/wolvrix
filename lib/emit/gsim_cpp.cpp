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

namespace wolvrix::lib::emit
{

    namespace
    {
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

        // ValueId comparator for std::map
        struct ValueIdCompare {
            bool operator()(const wolvrix::lib::grh::ValueId& lhs, const wolvrix::lib::grh::ValueId& rhs) const {
                if (lhs.index != rhs.index) return lhs.index < rhs.index;
                if (lhs.generation != rhs.generation) return lhs.generation < rhs.generation;
                if (lhs.graph.index != rhs.graph.index) return lhs.graph.index < rhs.graph.index;
                return lhs.graph.generation < rhs.graph.generation;
            }
        };

        // Code generation state for lowering GRH operations to C++
        struct CodegenState
        {
            // Value expressions: maps ValueId to C++ expression string
            std::map<wolvrix::lib::grh::ValueId, std::string, ValueIdCompare> valueExprs;
            // Storage declarations for registers/latches
            std::vector<std::string> storageDecls;
            // Sequential update statements (posedge_clock, combinational latch)
            std::map<std::string, std::vector<std::string>> sequentialStmts;
            // Combinational statements that drive outputs in step()
            std::vector<std::string> combinationalStmts;
            // Port declarations
            std::vector<std::pair<std::string, std::string>> inputPorts;
            std::vector<std::pair<std::string, std::string>> outputPorts;
            // Output port ValueId -> sanitized name mapping for driving outputs
            std::map<wolvrix::lib::grh::ValueId, std::string, ValueIdCompare> outputValueNames;
            // Input port ValueId -> sanitized name mapping for reading inputs
            std::map<wolvrix::lib::grh::ValueId, std::string, ValueIdCompare> inputValueNames;
            // Track unsupported operations
            std::vector<std::string> unsupportedOps;
        };

        // Get C++ type for a value based on its width
        std::string getCppTypeForWidth(int32_t width)
        {
            if (width <= 8) return "std::uint8_t";
            if (width <= 16) return "std::uint16_t";
            if (width <= 32) return "std::uint32_t";
            if (width <= 64) return "std::uint64_t";
            return "std::vector<std::uint64_t>";
        }

        // Convert Verilog-style constant to C++ constant
        std::string convertVerilogConstant(const std::string& verilogConst)
        {
            size_t apostrophe = verilogConst.find('\'');
            if (apostrophe == std::string::npos) return verilogConst;
            if (apostrophe + 2 >= verilogConst.size()) return "0";

            char base = verilogConst[apostrophe + 1];
            std::string value = verilogConst.substr(apostrophe + 2);

            switch (base) {
                case 'h': return "0x" + value;
                case 'b': {
                    try { return std::to_string(std::stoul(value, nullptr, 2)); }
                    catch (...) { return "0"; }
                }
                case 'd': return value;
                case 'o': {
                    try { return std::to_string(std::stoul(value, nullptr, 8)); }
                    catch (...) { return "0"; }
                }
                default: return "0";
            }
        }

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

        // Forward declare sanitizeIdentifier for use in lowerOperation
        // (already defined above)

        // Lower a single operation to C++
        void lowerOperation(
            const wolvrix::lib::grh::Graph& graph,
            const wolvrix::lib::grh::Operation& op,
            CodegenState& state)
        {
            using namespace wolvrix::lib::grh;

            const auto kind = op.kind();

            // Helper to get operand expression
            auto getOperandExpr = [&](size_t idx) -> std::string {
                if (idx >= op.operands().size()) return "0";
                auto it = state.valueExprs.find(op.operands()[idx]);
                if (it != state.valueExprs.end()) return it->second;
                return "0";
            };

            // Helper to set result expression
            auto setResultExpr = [&](size_t idx, const std::string& expr) {
                if (idx < op.results().size()) {
                    state.valueExprs[op.results()[idx]] = expr;
                }
            };

            switch (kind) {
                case OperationKind::kConstant: {
                    auto valueAttr = op.attr("constValue");
                    if (!valueAttr) valueAttr = op.attr("value");
                    if (valueAttr) {
                        if (auto* strVal = std::get_if<std::string>(&*valueAttr)) {
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
                case OperationKind::kNot: {
                    setResultExpr(0, "(~" + getOperandExpr(0) + ")");
                    break;
                }
                case OperationKind::kLogicNot: {
                    setResultExpr(0, "(!" + getOperandExpr(0) + ")");
                    break;
                }
                case OperationKind::kMux: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " ? " + getOperandExpr(1) + " : " + getOperandExpr(2) + ")");
                    break;
                }
                // Comparison operations
                case OperationKind::kEq: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " == " + getOperandExpr(1) + ")");
                    break;
                }
                case OperationKind::kNe: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " != " + getOperandExpr(1) + ")");
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
                // Logical operations
                case OperationKind::kLogicAnd: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " && " + getOperandExpr(1) + ")");
                    break;
                }
                case OperationKind::kLogicOr: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " || " + getOperandExpr(1) + ")");
                    break;
                }
                case OperationKind::kXnor: {
                    setResultExpr(0, "(~(" + getOperandExpr(0) + " ^ " + getOperandExpr(1) + "))");
                    break;
                }
                // Arithmetic
                case OperationKind::kMod: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " % " + getOperandExpr(1) + ")");
                    break;
                }
                // Shift operations
                case OperationKind::kShl: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " << " + getOperandExpr(1) + ")");
                    break;
                }
                case OperationKind::kLShr: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " >> " + getOperandExpr(1) + ")");
                    break;
                }
                case OperationKind::kAShr: {
                    setResultExpr(0, "(static_cast<std::make_signed_t<decltype(" + getOperandExpr(0) + ")>>(" + getOperandExpr(0) + ") >> " + getOperandExpr(1) + ")");
                    break;
                }
                // Reduce operations
                case OperationKind::kReduceAnd: {
                    int64_t w = graph.valueWidth(op.operands()[0]);
                    if (w > 0 && w <= 64) {
                        uint64_t mask = (w == 64) ? ~uint64_t(0) : ((uint64_t(1) << w) - 1);
                        setResultExpr(0, "((" + getOperandExpr(0) + " & 0x" + ([&]{ std::ostringstream ss; ss << std::hex << mask; return ss.str(); })() + "ULL) == 0x" + ([&]{ std::ostringstream ss; ss << std::hex << mask; return ss.str(); })() + "ULL ? 1 : 0)");
                    } else {
                        setResultExpr(0, "0");
                    }
                    break;
                }
                case OperationKind::kReduceOr: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " != 0 ? 1 : 0)");
                    break;
                }
                case OperationKind::kReduceXor: {
                    // XOR reduction: count set bits, result is parity
                    setResultExpr(0, "(__builtin_parityll(static_cast<unsigned long long>(" + getOperandExpr(0) + ")))");
                    break;
                }
                case OperationKind::kReduceNand: {
                    int64_t w = graph.valueWidth(op.operands()[0]);
                    if (w > 0 && w <= 64) {
                        uint64_t mask = (w == 64) ? ~uint64_t(0) : ((uint64_t(1) << w) - 1);
                        setResultExpr(0, "((" + getOperandExpr(0) + " & 0x" + ([&]{ std::ostringstream ss; ss << std::hex << mask; return ss.str(); })() + "ULL) == 0x" + ([&]{ std::ostringstream ss; ss << std::hex << mask; return ss.str(); })() + "ULL ? 0 : 1)");
                    } else {
                        setResultExpr(0, "1");
                    }
                    break;
                }
                case OperationKind::kReduceNor: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " == 0 ? 1 : 0)");
                    break;
                }
                case OperationKind::kReduceXnor: {
                    setResultExpr(0, "(__builtin_parityll(static_cast<unsigned long long>(" + getOperandExpr(0) + ")) ^ 1)");
                    break;
                }
                // Concat: shift operands and OR together
                case OperationKind::kConcat: {
                    if (op.operands().empty()) {
                        setResultExpr(0, "0");
                    } else if (op.operands().size() == 1) {
                        setResultExpr(0, getOperandExpr(0));
                    } else {
                        // Concat: first operand is MSB, operands go high-to-low
                        // result = (op0 << (w1+w2+...)) | (op1 << (w2+w3+...)) | ... | opN
                        std::string expr;
                        // Calculate total shift for each operand
                        std::vector<int64_t> widths;
                        for (size_t i = 0; i < op.operands().size(); ++i) {
                            widths.push_back(graph.valueWidth(op.operands()[i]));
                        }
                        for (size_t i = 0; i < op.operands().size(); ++i) {
                            int64_t shift = 0;
                            for (size_t j = i + 1; j < op.operands().size(); ++j) {
                                shift += widths[j];
                            }
                            std::string part = getOperandExpr(i);
                            if (shift > 0) {
                                part = "(static_cast<std::uint64_t>(" + part + ") << " + std::to_string(shift) + ")";
                            }
                            if (expr.empty()) {
                                expr = part;
                            } else {
                                expr = "(" + expr + " | " + part + ")";
                            }
                        }
                        setResultExpr(0, expr);
                    }
                    break;
                }
                // Replicate: repeat bits N times
                case OperationKind::kReplicate: {
                    auto countAttr = op.attr("replicateCount");
                    if (!countAttr) countAttr = op.attr("count");
                    int64_t count = 1;
                    if (countAttr) {
                        if (auto* intVal = std::get_if<int64_t>(&*countAttr)) {
                            count = *intVal;
                        }
                    }
                    if (count <= 1) {
                        setResultExpr(0, getOperandExpr(0));
                    } else {
                        int64_t opWidth = graph.valueWidth(op.operands()[0]);
                        std::string expr;
                        for (int64_t i = 0; i < count; ++i) {
                            std::string part = getOperandExpr(0);
                            int64_t shift = (count - 1 - i) * opWidth;
                            if (shift > 0) {
                                part = "(static_cast<std::uint64_t>(" + part + ") << " + std::to_string(shift) + ")";
                            }
                            if (expr.empty()) {
                                expr = part;
                            } else {
                                expr = "(" + expr + " | " + part + ")";
                            }
                        }
                        setResultExpr(0, expr);
                    }
                    break;
                }
                // Static slice: extract bits [sliceEnd:sliceStart]
                case OperationKind::kSliceStatic: {
                    auto startAttr = op.attr("sliceStart");
                    auto endAttr = op.attr("sliceEnd");
                    int64_t start = 0, end = 0;
                    if (startAttr) {
                        if (auto* intVal = std::get_if<int64_t>(&*startAttr)) start = *intVal;
                    }
                    if (endAttr) {
                        if (auto* intVal = std::get_if<int64_t>(&*endAttr)) end = *intVal;
                    }
                    int64_t width = end - start + 1;
                    if (width <= 0) width = 1;
                    if (start == 0 && width >= 64) {
                        setResultExpr(0, getOperandExpr(0));
                    } else {
                        uint64_t mask = (width >= 64) ? ~uint64_t(0) : ((uint64_t(1) << width) - 1);
                        std::string maskStr = ([&]{ std::ostringstream ss; ss << "0x" << std::hex << mask << "ULL"; return ss.str(); })();
                        if (start == 0) {
                            setResultExpr(0, "(" + getOperandExpr(0) + " & " + maskStr + ")");
                        } else {
                            setResultExpr(0, "((" + getOperandExpr(0) + " >> " + std::to_string(start) + ") & " + maskStr + ")");
                        }
                    }
                    break;
                }
                // Dynamic slice: extract sliceWidth bits starting at dynamic index
                case OperationKind::kSliceDynamic: {
                    auto widthAttr = op.attr("sliceWidth");
                    int64_t width = 1;
                    if (widthAttr) {
                        if (auto* intVal = std::get_if<int64_t>(&*widthAttr)) width = *intVal;
                    }
                    if (width >= 64) {
                        setResultExpr(0, "(" + getOperandExpr(0) + " >> " + getOperandExpr(1) + ")");
                    } else {
                        uint64_t mask = (uint64_t(1) << width) - 1;
                        std::string maskStr = ([&]{ std::ostringstream ss; ss << "0x" << std::hex << mask << "ULL"; return ss.str(); })();
                        setResultExpr(0, "((" + getOperandExpr(0) + " >> " + getOperandExpr(1) + ") & " + maskStr + ")");
                    }
                    break;
                }
                // Latch (level-sensitive storage)
                case OperationKind::kLatch: {
                    // Latch defines storage - handled similarly to register
                    break;
                }
                case OperationKind::kLatchReadPort: {
                    auto latchSymAttr = op.attr("latchSymbol");
                    std::string sym;
                    if (latchSymAttr) {
                        if (auto* strVal = std::get_if<std::string>(&*latchSymAttr)) sym = *strVal;
                    }
                    if (sym.empty()) sym = std::string(op.symbolText());
                    if (!sym.empty()) {
                        std::string latchName = "latch_" + sanitizeIdentifier(sym);
                        setResultExpr(0, latchName);
                    }
                    break;
                }
                case OperationKind::kLatchWritePort: {
                    std::string condition = getOperandExpr(0);
                    std::string nextValue = getOperandExpr(1);
                    auto latchSymAttr = op.attr("latchSymbol");
                    std::string sym;
                    if (latchSymAttr) {
                        if (auto* strVal = std::get_if<std::string>(&*latchSymAttr)) sym = *strVal;
                    }
                    if (sym.empty()) sym = std::string(op.symbolText());
                    if (!sym.empty()) {
                        std::string latchName = "latch_" + sanitizeIdentifier(sym);
                        state.sequentialStmts["combinational"].push_back(
                            "        if (" + condition + ") { " + latchName + " = " + nextValue + "; }");
                    }
                    break;
                }
                case OperationKind::kRegister: {
                    // Register defines storage - handled in collectRegisters
                    break;
                }
                case OperationKind::kRegisterReadPort: {
                    auto regSymAttr = op.attr("regSymbol");
                    std::string sym;
                    if (regSymAttr) {
                        if (auto* strVal = std::get_if<std::string>(&*regSymAttr)) sym = *strVal;
                    }
                    if (sym.empty()) sym = std::string(op.symbolText());
                    if (!sym.empty()) {
                        std::string regName = "reg_" + sanitizeIdentifier(sym);
                        setResultExpr(0, regName);
                    }
                    break;
                }
                case OperationKind::kRegisterWritePort: {
                    std::string condition = getOperandExpr(0);
                    std::string nextValue = getOperandExpr(1);
                    std::string mask = getOperandExpr(2);

                    auto regSymAttr = op.attr("regSymbol");
                    std::string sym;
                    if (regSymAttr) {
                        if (auto* strVal = std::get_if<std::string>(&*regSymAttr)) sym = *strVal;
                    }
                    if (sym.empty()) sym = std::string(op.symbolText());
                    if (!sym.empty()) {
                        std::string regName = "reg_" + sanitizeIdentifier(sym);
                        if (mask != "0") {
                            state.sequentialStmts["posedge_clock"].push_back(
                                "        if (" + condition + ") { " + regName + " = (" + regName + " & ~" + mask + ") | (" + nextValue + " & " + mask + "); }");
                        } else {
                            state.sequentialStmts["posedge_clock"].push_back(
                                "        if (" + condition + ") { " + regName + " = " + nextValue + "; }");
                        }
                    }
                    break;
                }
                case OperationKind::kSystemTask:
                case OperationKind::kSystemFunction: {
                    // Debug constructs - no simulation logic
                    break;
                }
                default: {
                    std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                    state.unsupportedOps.push_back(std::string(toString(kind)) + " (" + opName + ")");
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
        void collectPorts(const wolvrix::lib::grh::Graph& graph, CodegenState& state,
                          PortOrderStrategy strategy = PortOrderStrategy::Decl,
                          const std::vector<std::string>& customOrder = {})
        {
            for (const auto& port : graph.inputPorts()) {
                auto value = graph.getValue(port.value);
                std::string type = getCppTypeForWidth(value.width());
                state.inputPorts.push_back({port.name, type});
                std::string portExpr = "input_" + sanitizeIdentifier(port.name) + "_";
                state.valueExprs[port.value] = portExpr;
                state.inputValueNames[port.value] = sanitizeIdentifier(port.name);
            }

            for (const auto& port : graph.outputPorts()) {
                auto value = graph.getValue(port.value);
                std::string type = getCppTypeForWidth(value.width());
                state.outputPorts.push_back({port.name, type});
                state.outputValueNames[port.value] = sanitizeIdentifier(port.name);
            }

            // Apply port ordering
            sortPorts(state.inputPorts, strategy, customOrder);
            sortPorts(state.outputPorts, strategy, customOrder);
        }

        // Collect register storage declarations
        void collectRegisters(const wolvrix::lib::grh::Graph& graph, CodegenState& state)
        {
            for (const auto& opId : graph.operations()) {
                auto op = graph.getOperation(opId);
                if (op.kind() == wolvrix::lib::grh::OperationKind::kRegister) {
                    std::string sym = std::string(op.symbolText());
                    if (sym.empty()) sym = "unnamed_reg_" + std::to_string(opId.index);
                    std::string regName = "reg_" + sanitizeIdentifier(sym);

                    int32_t width = 32;
                    if (!op.results().empty()) {
                        auto val = graph.getValue(op.results()[0]);
                        width = val.width();
                    }
                    std::string type = getCppTypeForWidth(width);
                    state.storageDecls.push_back(type + " " + regName + " = 0;");

                    if (!op.results().empty()) {
                        state.valueExprs[op.results()[0]] = regName;
                    }
                }
                // Collect latches similarly to registers
                if (op.kind() == wolvrix::lib::grh::OperationKind::kLatch) {
                    std::string sym = std::string(op.symbolText());
                    if (sym.empty()) sym = "unnamed_latch_" + std::to_string(opId.index);
                    std::string latchName = "latch_" + sanitizeIdentifier(sym);

                    int32_t width = 32;
                    if (!op.results().empty()) {
                        auto val = graph.getValue(op.results()[0]);
                        width = val.width();
                    }
                    std::string type = getCppTypeForWidth(width);
                    state.storageDecls.push_back(type + " " + latchName + " = 0;");

                    if (!op.results().empty()) {
                        state.valueExprs[op.results()[0]] = latchName;
                    }
                }
            }
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

        void writeHeader(std::ostream &os,
                         const EmitTarget &target,
                         const GsimScratchpadMetadata &metadata,
                         const CodegenState& state)
        {
            const std::string ns = sanitizeIdentifier(target.scratchGraphSymbol);
            const std::string structName = "GsimMetadata_" + ns;
            os << "#pragma once\n\n";
            os << "#include <cstdint>\n";
            os << "#include <map>\n";
            os << "#include <stdexcept>\n";
            os << "#include <string>\n";
            os << "#include <vector>\n\n";
            os << "class SSimTop {\n";
            os << "public:\n";
            os << "    SSimTop() { reset(); }\n";
            os << "    ~SSimTop() = default;\n\n";
            os << "    void set_reset(unsigned reset) { reset_ = reset; }\n\n";
            os << "    void reset() {\n";
            os << "        reset_ = true;\n";
            // Reset storage members to zero
            for (const auto& decl : state.storageDecls) {
                // Extract variable name from "type varname = 0;" -> "varname"
                auto eqPos = decl.find(" = ");
                if (eqPos != std::string::npos) {
                    auto spacePos = decl.rfind(' ', eqPos - 1);
                    if (spacePos != std::string::npos) {
                        std::string varName = decl.substr(spacePos + 1, eqPos - spacePos - 1);
                        os << "        " << varName << " = 0;\n";
                    }
                }
            }
            // Reset output ports
            for (const auto& [name, type] : state.outputPorts) {
                os << "        output_" << sanitizeIdentifier(name) << "_ = 0;\n";
            }
            os << "    }\n\n";
            os << "    void step() {\n";
            os << "        ++difftest_step_;\n";
            os << "        if (reset_) {\n";
            os << "            reset_ = false;\n";
            os << "            difftest_exit_ = 0;\n";
            os << "            return;\n";
            os << "        }\n";
            // Combinational logic: evaluate expressions and drive outputs
            if (!state.combinationalStmts.empty()) {
                for (const auto& stmt : state.combinationalStmts) {
                    os << stmt << "\n";
                }
            }
            // Sequential logic: register/latch updates
            if (!state.sequentialStmts.empty()) {
                for (const auto& [domain, stmts] : state.sequentialStmts) {
                    (void)domain;
                    for (const auto& stmt : stmts) {
                        os << stmt << "\n";
                    }
                }
            }
            os << "        difftest_exit_ = 0;\n";
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

            // Difftest compatibility stubs
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
                os << "    " << type << " input_" << sanitizeIdentifier(name) << "_ = 0;\n";
            }

            // Output port storage
            for (const auto& [name, type] : state.outputPorts) {
                os << "    " << type << " output_" << sanitizeIdentifier(name) << "_ = 0;\n";
            }

            // Register/latch storage
            for (const auto& decl : state.storageDecls) {
                os << "    " << decl << "\n";
            }

            // Difftest state
            os << "    std::uint64_t difftest_exit_ = 0;\n";
            os << "    std::uint64_t difftest_step_ = 0;\n";
            os << "    unsigned perf_clean_ = 0;\n";
            os << "    unsigned perf_dump_ = 0;\n";
            os << "    std::uint64_t log_begin_ = 0;\n";
            os << "    std::uint64_t log_end_ = 0;\n";
            os << "};\n\n";
            os << "namespace wolvrix::gsim {\n\n";
            os << "struct " << structName << " {\n";
            os << "    std::string graph_symbol;\n";
            os << "    std::string selection_path;\n";
            os << "    std::string scratchpad_namespace;\n";
            os << "    std::int64_t op_count = 0;\n";
            os << "    std::int64_t graph_revision = 0;\n";
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
            os << "};\n\n";
            os << structName << " make_" << sanitizeIdentifier(target.scratchGraphSymbol) << "_metadata();\n";
            os << "bool validate_" << sanitizeIdentifier(target.scratchGraphSymbol) << "_metadata(const " << structName << "& metadata);\n\n";
            os << "} // namespace wolvrix::gsim\n";
            (void)metadata;
        }

        void writeSource(std::ostream &os,
                         const EmitTarget &target,
                         const GsimScratchpadMetadata &metadata,
                         std::string_view headerFilename)
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

        // Generate code from GRH operations
        CodegenState state;
        collectPorts(*target->graph, state, options.portOrderStrategy, options.portOrderNames);
        collectRegisters(*target->graph, state);

        // Traverse operations in topo order
        for (int64_t opIdx : metadata->topoOrder) {
            auto opIdIt = std::find_if(target->graph->operations().begin(), target->graph->operations().end(),
                [&](const wolvrix::lib::grh::OperationId& id) { return static_cast<int64_t>(id.index) == opIdx; });
            if (opIdIt != target->graph->operations().end()) {
                auto op = target->graph->getOperation(*opIdIt);
                lowerOperation(*target->graph, op, state);
            }
        }

        // Generate combinational output driving statements
        // For each output port, find the GRH value that drives it and emit an assignment
        for (const auto& port : target->graph->outputPorts()) {
            auto it = state.outputValueNames.find(port.value);
            if (it == state.outputValueNames.end()) continue;
            const std::string& sanitizedName = it->second;

            // The output port's value might be defined by an operation (the GRH uses the port value as the result of some op)
            // Or the output might be connected via an assign to some other value
            // Look up the expression for this value
            auto exprIt = state.valueExprs.find(port.value);
            if (exprIt != state.valueExprs.end()) {
                state.combinationalStmts.push_back(
                    "        output_" + sanitizedName + "_ = " + exprIt->second + ";");
            } else {
                // Try to find the value through the graph's definition chain
                // The output port value should have been set by some lowered operation
                // If not found, it means the output is not driven (leave at 0)
            }
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

        const std::filesystem::path outputDir = resolveOutputDir(options);
        const std::string baseName = options.outputFilename && !options.outputFilename->empty()
                                         ? sanitizeIdentifier(std::filesystem::path(*options.outputFilename).stem().string())
                                         : defaultBaseName(*target);
        const std::filesystem::path headerPath = outputDir / (baseName + ".hpp");
        const std::filesystem::path sourcePath = outputDir / (baseName + ".cpp");

        auto header = openOutputFile(headerPath);
        auto source = openOutputFile(sourcePath);
        if (!header || !source)
        {
            result.success = false;
            return result;
        }

        writeHeader(*header, *target, *metadata, state);
        writeSource(*source, *target, *metadata, headerPath.filename().string());

        result.artifacts.push_back(headerPath.string());
        result.artifacts.push_back(sourcePath.string());
        return result;
    }

} // namespace wolvrix::lib::emit
