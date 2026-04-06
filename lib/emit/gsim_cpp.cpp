#include "emit/gsim_cpp.hpp"

#include "core/transform.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
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
            struct DpiImportArg
            {
                std::string direction;
                std::string name;
                std::string typeName;
                std::string cppType;
            };

            struct DpiImportSignature
            {
                std::string symbol;
                std::vector<DpiImportArg> args;
                bool hasReturn = false;
                std::string returnTypeName;
                std::string returnCppType;
            };

            // Value expressions: maps ValueId to C++ expression string
            std::map<wolvrix::lib::grh::ValueId, std::string, ValueIdCompare> valueExprs;
            // Storage declarations for registers/latches
            std::vector<std::string> storageDecls;
            // Explicit constructor-time allocation / setup statements.
            std::vector<std::string> ctorStmts;
            // Explicit reset statements for stateful storage
            std::vector<std::string> resetStmts;
            // Sequential update statements (posedge_clock, combinational latch)
            std::map<std::string, std::vector<std::string>> sequentialStmts;
            // Combinational statements that drive outputs in step()
            std::vector<std::string> combinationalStmts;
            // Public output assignments emitted after state updates.
            std::vector<std::string> outputStmts;
            // Port declarations
            std::vector<std::pair<std::string, std::string>> inputPorts;
            std::vector<std::pair<std::string, std::string>> outputPorts;
            // Output port ValueId -> sanitized name mapping for driving outputs
            std::map<wolvrix::lib::grh::ValueId, std::string, ValueIdCompare> outputValueNames;
            // Input port ValueId -> sanitized name mapping for reading inputs
            std::map<wolvrix::lib::grh::ValueId, std::string, ValueIdCompare> inputValueNames;
            // Memory symbol -> storage name and row count
            std::map<std::string, std::string> memoryStorageNames;
            std::map<std::string, int64_t> memoryRows;
            // DPI imports collected from the graph for declaration / lowering.
            std::map<std::string, DpiImportSignature> dpiImports;
            std::vector<std::string> dpiForwardDecls;
            // When behavior is sharded, materialized temporaries live in shared storage.
            struct PersistentTempGroup
            {
                std::string cppType;
                std::string storageName;
                std::size_t count = 0;
            };

            bool persistentTemps = false;
            std::string persistentTempPrefix = "step_tmp_group_";
            std::vector<PersistentTempGroup> persistentTempGroups;
            std::map<std::string, std::size_t> persistentTempGroupIndices;
            // Track unsupported operations
            std::vector<std::string> unsupportedOps;
            bool hasResetInput = false;
        };

        // Get C++ type for a value based on its width
        std::string getCppTypeForWidth(int32_t width)
        {
            if (width <= 8) return "std::uint8_t";
            if (width <= 16) return "std::uint16_t";
            if (width <= 32) return "std::uint32_t";
            if (width <= 64) return "std::uint64_t";
            return "wolvrix::gsim::Bits<" + std::to_string(width) + ">";
        }

        // Convert Verilog-style constant to C++ constant
        std::string convertVerilogConstant(const std::string& verilogConst)
        {
            auto lowerHexLiteral = [](std::uint64_t value) -> std::string {
                std::ostringstream ss;
                ss << "0x" << std::hex << value << "ULL";
                return ss.str();
            };

            auto parseWidth = [](std::string_view text) -> std::optional<int64_t> {
                if (text.empty())
                {
                    return std::nullopt;
                }
                int64_t width = 0;
                for (char ch : text)
                {
                    if (ch < '0' || ch > '9')
                    {
                        return std::nullopt;
                    }
                    width = width * 10 + static_cast<int64_t>(ch - '0');
                }
                return width;
            };

            auto parseUnknownTolerantWords = [](std::string_view digits, unsigned base, int64_t widthBits) -> std::optional<std::vector<std::uint64_t>> {
                if (base < 2)
                {
                    return std::nullopt;
                }

                std::size_t wordCount = 1;
                if (widthBits > 0)
                {
                    wordCount = static_cast<std::size_t>((widthBits + 63) / 64);
                }
                std::vector<std::uint64_t> words(wordCount, 0);

                for (char ch : digits)
                {
                    if (ch == '_')
                    {
                        continue;
                    }

                    const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    unsigned digit = 0;
                    if (lower == 'x' || lower == 'z' || lower == '?')
                    {
                        digit = 0;
                    }
                    else if (lower >= '0' && lower <= '9')
                    {
                        digit = static_cast<unsigned>(lower - '0');
                    }
                    else if (lower >= 'a' && lower <= 'f')
                    {
                        digit = static_cast<unsigned>(10 + (lower - 'a'));
                    }
                    else
                    {
                        return std::nullopt;
                    }

                    if (digit >= base)
                    {
                        return std::nullopt;
                    }

                    unsigned __int128 carry = digit;
                    for (std::size_t index = 0; index < words.size(); ++index)
                    {
                        const unsigned __int128 accum =
                            static_cast<unsigned __int128>(words[index]) * base + carry;
                        words[index] = static_cast<std::uint64_t>(accum);
                        carry = accum >> 64;
                    }
                }

                if (widthBits > 0)
                {
                    const auto remainder = static_cast<unsigned>(widthBits % 64);
                    if (remainder != 0)
                    {
                        words.back() &= ((std::uint64_t{1} << remainder) - 1);
                    }
                }
                return words;
            };

            auto emitWideBitsLiteral = [&](const std::vector<std::uint64_t>& words, int64_t widthBits) -> std::string {
                std::ostringstream ss;
                ss << "([]{ " << getCppTypeForWidth(static_cast<int32_t>(widthBits)) << " value; ";
                for (std::size_t index = 0; index < words.size(); ++index)
                {
                    ss << "value.words[" << index << "] = " << lowerHexLiteral(words[index]) << "; ";
                }
                ss << "value.maskUnusedBits(); return value; }())";
                return ss.str();
            };

            size_t apostrophe = verilogConst.find('\'');
            if (apostrophe == std::string::npos) return verilogConst;
            if (apostrophe + 2 >= verilogConst.size()) return "0";
            const auto widthBits = parseWidth(std::string_view(verilogConst).substr(0, apostrophe));

            std::size_t basePos = apostrophe + 1;
            if (basePos < verilogConst.size() &&
                (verilogConst[basePos] == 's' || verilogConst[basePos] == 'S'))
            {
                ++basePos;
            }
            if (basePos >= verilogConst.size())
            {
                return "0";
            }

            char base = static_cast<char>(std::tolower(static_cast<unsigned char>(verilogConst[basePos])));
            std::string value = verilogConst.substr(basePos + 1);

            switch (base) {
                case 'h': {
                    const auto parsed = parseUnknownTolerantWords(value, 16, widthBits.value_or(0));
                    if (!parsed)
                    {
                        return "0";
                    }
                    if (widthBits.has_value() && *widthBits > 64)
                    {
                        return emitWideBitsLiteral(*parsed, *widthBits);
                    }
                    return lowerHexLiteral(parsed->empty() ? 0 : (*parsed)[0]);
                }
                case 'b': {
                    const auto parsed = parseUnknownTolerantWords(value, 2, widthBits.value_or(0));
                    if (!parsed)
                    {
                        return "0";
                    }
                    if (widthBits.has_value() && *widthBits > 64)
                    {
                        return emitWideBitsLiteral(*parsed, *widthBits);
                    }
                    return lowerHexLiteral(parsed->empty() ? 0 : (*parsed)[0]);
                }
                case 'd': {
                    const auto parsed = parseUnknownTolerantWords(value, 10, widthBits.value_or(0));
                    if (!parsed)
                    {
                        return "0";
                    }
                    if (widthBits.has_value() && *widthBits > 64)
                    {
                        return emitWideBitsLiteral(*parsed, *widthBits);
                    }
                    return parsed->empty() ? "0" : std::to_string((*parsed)[0]);
                }
                case 'o': {
                    const auto parsed = parseUnknownTolerantWords(value, 8, widthBits.value_or(0));
                    if (!parsed)
                    {
                        return "0";
                    }
                    if (widthBits.has_value() && *widthBits > 64)
                    {
                        return emitWideBitsLiteral(*parsed, *widthBits);
                    }
                    return lowerHexLiteral(parsed->empty() ? 0 : (*parsed)[0]);
                }
                default: return "0";
            }
        }

        std::string cppStringLiteral(std::string_view text)
        {
            std::string out;
            out.reserve(text.size() + 2);
            out.push_back('"');
            for (char ch : text)
            {
                switch (ch)
                {
                case '\\':
                    out.append("\\\\");
                    break;
                case '"':
                    out.append("\\\"");
                    break;
                case '\n':
                    out.append("\\n");
                    break;
                case '\r':
                    out.append("\\r");
                    break;
                case '\t':
                    out.append("\\t");
                    break;
                default:
                    out.push_back(ch);
                    break;
                }
            }
            out.push_back('"');
            return out;
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

        bool boolAttrValue(const EmitOptions &options, std::string_view key, bool defaultValue)
        {
            const auto value = attrValue(options, key);
            if (!value)
            {
                return defaultValue;
            }

            std::string lowered = *value;
            std::transform(lowered.begin(),
                           lowered.end(),
                           lowered.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off")
            {
                return false;
            }
            if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on")
            {
                return true;
            }
            return defaultValue;
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

        std::string materializedValueName(const wolvrix::lib::grh::ValueId &valueId)
        {
            std::string name = "sim_tmp_v";
            name.append(std::to_string(valueId.index));
            if (valueId.generation != 0)
            {
                name.append("_g");
                name.append(std::to_string(valueId.generation));
            }
            return name;
        }

        std::string allocatePersistentTempExpr(CodegenState &state, const std::string &cppType)
        {
            auto groupIt = state.persistentTempGroupIndices.find(cppType);
            if (groupIt == state.persistentTempGroupIndices.end())
            {
                const auto groupIndex = state.persistentTempGroups.size();
                CodegenState::PersistentTempGroup group;
                group.cppType = cppType;
                group.storageName = state.persistentTempPrefix + std::to_string(groupIndex) + "_";
                groupIt = state.persistentTempGroupIndices.emplace(cppType, groupIndex).first;
                state.persistentTempGroups.push_back(std::move(group));
            }

            auto &group = state.persistentTempGroups[groupIt->second];
            const auto slot = group.count++;
            return group.storageName + "[" + std::to_string(slot) + "]";
        }

        std::string dpiScratchValueName(std::string_view prefix, const wolvrix::lib::grh::ValueId &valueId)
        {
            std::string name(prefix);
            name.push_back('_');
            name.append(materializedValueName(valueId));
            return name;
        }

        std::string lowercase(std::string_view text)
        {
            std::string out;
            out.reserve(text.size());
            for (unsigned char ch : text)
            {
                out.push_back(static_cast<char>(std::tolower(ch)));
            }
            return out;
        }

        std::optional<std::string> mapDpiScalarCppType(std::string_view typeName,
                                                       int64_t width,
                                                       bool isSigned)
        {
            const std::string lowered = lowercase(typeName);
            if (lowered == "bit" || lowered == "logic")
            {
                if (width <= 0 || width > 64)
                {
                    return std::nullopt;
                }
                if (width <= 8)
                {
                    return isSigned ? "std::int8_t" : "std::uint8_t";
                }
                if (width <= 16)
                {
                    return isSigned ? "std::int16_t" : "std::uint16_t";
                }
                if (width <= 32)
                {
                    return isSigned ? "std::int32_t" : "std::uint32_t";
                }
                return isSigned ? "std::int64_t" : "std::uint64_t";
            }
            if (lowered == "byte")
            {
                return isSigned ? "signed char" : "unsigned char";
            }
            if (lowered == "shortint")
            {
                return isSigned ? "short" : "unsigned short";
            }
            if (lowered == "int" || lowered == "integer")
            {
                return isSigned ? "int" : "unsigned int";
            }
            if (lowered == "longint" || lowered == "time")
            {
                return isSigned ? "std::int64_t" : "std::uint64_t";
            }
            return std::nullopt;
        }

        template <typename T>
        std::optional<T> getAttrAs(const wolvrix::lib::grh::Operation &op, std::string_view name)
        {
            const auto attr = op.attr(std::string(name));
            if (!attr)
            {
                return std::nullopt;
            }
            if (const auto *value = std::get_if<T>(&*attr))
            {
                return *value;
            }
            return std::nullopt;
        }

        int findNamedArgIndex(const std::vector<std::string> &names, std::string_view target)
        {
            for (std::size_t i = 0; i < names.size(); ++i)
            {
                if (names[i] == target)
                {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

        void collectDpiImports(const wolvrix::lib::grh::Graph &graph, CodegenState &state)
        {
            using namespace wolvrix::lib::grh;

            for (const auto opId : graph.operations())
            {
                const auto op = graph.getOperation(opId);
                if (op.kind() != OperationKind::kDpicImport)
                {
                    continue;
                }

                const std::string symbol =
                    op.symbolText().empty() ? ("unnamed_dpi_import_" + std::to_string(opId.index))
                                            : std::string(op.symbolText());
                const auto argsDirection = getAttrAs<std::vector<std::string>>(op, "argsDirection");
                const auto argsWidth = getAttrAs<std::vector<int64_t>>(op, "argsWidth");
                const auto argsName = getAttrAs<std::vector<std::string>>(op, "argsName");
                const auto argsSigned = getAttrAs<std::vector<bool>>(op, "argsSigned");
                const auto argsType = getAttrAs<std::vector<std::string>>(op, "argsType");
                const bool hasReturn = getAttrAs<bool>(op, "hasReturn").value_or(false);
                const int64_t returnWidth = getAttrAs<int64_t>(op, "returnWidth").value_or(0);
                const bool returnSigned = getAttrAs<bool>(op, "returnSigned").value_or(false);
                const std::string returnType = getAttrAs<std::string>(op, "returnType").value_or("void");

                if (!argsDirection || !argsWidth || !argsName || !argsSigned || !argsType ||
                    argsDirection->size() != argsWidth->size() || argsDirection->size() != argsName->size() ||
                    argsDirection->size() != argsSigned->size() || argsDirection->size() != argsType->size())
                {
                    state.unsupportedOps.push_back("kDpicImport (" + symbol + ": malformed DPI signature metadata)");
                    continue;
                }

                CodegenState::DpiImportSignature sig;
                sig.symbol = symbol;
                sig.hasReturn = hasReturn;
                sig.returnTypeName = returnType;

                bool ok = true;
                for (std::size_t i = 0; i < argsName->size(); ++i)
                {
                    std::optional<std::string> cppType;
                    if (lowercase((*argsType)[i]) == "string")
                    {
                        if ((*argsDirection)[i] == "input")
                        {
                            cppType = "const char *";
                        }
                    }
                    else
                    {
                        cppType = mapDpiScalarCppType((*argsType)[i], (*argsWidth)[i], (*argsSigned)[i]);
                    }
                    if (!cppType)
                    {
                        state.unsupportedOps.push_back(
                            "kDpicImport (" + symbol + ": unsupported DPI argument type " + (*argsType)[i] + ")");
                        ok = false;
                        break;
                    }
                    if ((*argsDirection)[i] != "input" && (*argsDirection)[i] != "output")
                    {
                        state.unsupportedOps.push_back(
                            "kDpicImport (" + symbol + ": unsupported DPI argument direction " + (*argsDirection)[i] + ")");
                        ok = false;
                        break;
                    }
                    sig.args.push_back(CodegenState::DpiImportArg{
                        (*argsDirection)[i],
                        (*argsName)[i],
                        (*argsType)[i],
                        *cppType,
                    });
                }
                if (!ok)
                {
                    continue;
                }

                if (hasReturn)
                {
                    const auto cppType = mapDpiScalarCppType(returnType, returnWidth, returnSigned);
                    if (!cppType)
                    {
                        state.unsupportedOps.push_back(
                            "kDpicImport (" + symbol + ": unsupported DPI return type " + returnType + ")");
                        continue;
                    }
                    sig.returnCppType = *cppType;
                }
                else
                {
                    sig.returnCppType = "void";
                }

                std::ostringstream decl;
                decl << sig.returnCppType << " " << symbol << "(";
                for (std::size_t i = 0; i < sig.args.size(); ++i)
                {
                    if (i != 0)
                    {
                        decl << ", ";
                    }
                    decl << sig.args[i].cppType;
                    if (sig.args[i].direction == "output")
                    {
                        decl << " *";
                    }
                    decl << " " << sanitizeIdentifier(sig.args[i].name);
                }
                decl << ");";

                const auto [it, inserted] = state.dpiImports.emplace(symbol, std::move(sig));
                if (!inserted)
                {
                    state.unsupportedOps.push_back("kDpicImport (" + symbol + ": duplicate DPI import symbol)");
                    continue;
                }
                state.dpiForwardDecls.push_back(decl.str());
                (void)it;
            }
        }

        bool isPostSequentialSideEffectDpiCall(const wolvrix::lib::grh::Operation &op)
        {
            using namespace wolvrix::lib::grh;

            if (op.kind() != OperationKind::kDpicCall)
            {
                return false;
            }

            const bool hasReturn = getAttrAs<bool>(op, "hasReturn").value_or(false);
            const auto outArgName =
                getAttrAs<std::vector<std::string>>(op, "outArgName").value_or(std::vector<std::string>{});
            const auto eventEdge =
                getAttrAs<std::vector<std::string>>(op, "eventEdge").value_or(std::vector<std::string>{});
            return !hasReturn && outArgName.empty() && !eventEdge.empty();
        }

        bool isStatefulReadOp(const wolvrix::lib::grh::Operation &op)
        {
            using namespace wolvrix::lib::grh;

            switch (op.kind())
            {
                case OperationKind::kRegisterReadPort:
                case OperationKind::kLatchReadPort:
                    return true;
                default:
                    return false;
            }
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
                    const auto resultId = op.results()[idx];
                    if (graph.valueType(resultId) == ValueType::String) {
                        state.valueExprs[resultId] = expr;
                        return;
                    }
                    const auto resultType = getCppTypeForWidth(graph.valueWidth(resultId));
                    if (state.persistentTemps) {
                        const auto tempName = allocatePersistentTempExpr(state, resultType);
                        state.combinationalStmts.push_back("        " + tempName + " = " + expr + ";");
                        state.valueExprs[resultId] = tempName;
                    } else {
                        const auto tempName = materializedValueName(resultId);
                        state.combinationalStmts.push_back(
                            "        [[maybe_unused]] const " + resultType + " " + tempName + " = " + expr + ";");
                        state.valueExprs[resultId] = tempName;
                    }
                }
            };
            struct MutableResultStorage
            {
                std::string cppType;
                std::string expr;
            };
            auto reserveMutableResultStorage = [&](size_t idx) -> std::optional<MutableResultStorage> {
                if (idx >= op.results().size()) {
                    return std::nullopt;
                }
                const auto resultId = op.results()[idx];
                if (graph.valueType(resultId) == ValueType::String) {
                    return std::nullopt;
                }
                const auto resultType = getCppTypeForWidth(graph.valueWidth(resultId));
                if (state.persistentTemps) {
                    const auto tempName = allocatePersistentTempExpr(state, resultType);
                    state.valueExprs[resultId] = tempName;
                    return MutableResultStorage{resultType, tempName};
                }
                const auto tempName = materializedValueName(resultId);
                state.combinationalStmts.push_back(
                    "        [[maybe_unused]] " + resultType + " " + tempName + "{};");
                state.valueExprs[resultId] = tempName;
                return MutableResultStorage{resultType, tempName};
            };
            auto getShiftAmountExpr = [&](size_t idx) -> std::string {
                return "(static_cast<std::size_t>(static_cast<std::uint64_t>(" + getOperandExpr(idx) + ")))";
            };
            auto setComparisonResultExpr = [&](std::string_view compareOp) {
                const std::string lhs = getOperandExpr(0);
                const std::string rhs = getOperandExpr(1);
                if (lhs == rhs) {
                    if (compareOp == "==" || compareOp == "<=" || compareOp == ">=") {
                        setResultExpr(0, "1");
                    } else {
                        setResultExpr(0, "0");
                    }
                    return;
                }
                setResultExpr(0, "(" + lhs + " " + std::string(compareOp) + " " + rhs + ")");
            };

            switch (kind) {
                case OperationKind::kConstant: {
                    auto valueAttr = op.attr("constValue");
                    if (!valueAttr) valueAttr = op.attr("value");
                    if (valueAttr) {
                        if (auto* strVal = std::get_if<std::string>(&*valueAttr)) {
                            if (!op.results().empty() && graph.valueType(op.results()[0]) == ValueType::String) {
                                setResultExpr(0, cppStringLiteral(*strVal));
                            } else {
                                setResultExpr(0, convertVerilogConstant(*strVal));
                            }
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
                    if (!op.results().empty()) {
                        state.valueExprs[op.results()[0]] = getOperandExpr(0);
                    }
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
                    const auto resultType = getCppTypeForWidth(graph.valueWidth(op.results()[0]));
                    const auto zero = resultType + "(0)";
                    setResultExpr(0,
                                  "((" + getOperandExpr(1) + ") == " + zero + " ? " + zero + " : (" +
                                  getOperandExpr(0) + " / " + getOperandExpr(1) + "))");
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
                    // Mask result to operand width to avoid promoted bitwise complement warnings
                    int64_t w = 0;
                    if (!op.operands().empty()) w = graph.valueWidth(op.operands()[0]);
                    if (w > 0 && w < 64) {
                        uint64_t mask = (uint64_t(1) << w) - 1;
                        std::ostringstream ss;
                        ss << "((~" << getOperandExpr(0) << ") & 0x" << std::hex << mask << "ULL)";
                        setResultExpr(0, ss.str());
                    } else {
                        setResultExpr(0, "(~" + getOperandExpr(0) + ")");
                    }
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
                    setComparisonResultExpr("==");
                    break;
                }
                case OperationKind::kCaseEq: {
                    setComparisonResultExpr("==");
                    break;
                }
                case OperationKind::kNe: {
                    setComparisonResultExpr("!=");
                    break;
                }
                case OperationKind::kCaseNe: {
                    setComparisonResultExpr("!=");
                    break;
                }
                case OperationKind::kLt: {
                    setComparisonResultExpr("<");
                    break;
                }
                case OperationKind::kLe: {
                    setComparisonResultExpr("<=");
                    break;
                }
                case OperationKind::kGt: {
                    setComparisonResultExpr(">");
                    break;
                }
                case OperationKind::kGe: {
                    setComparisonResultExpr(">=");
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
                    int64_t w = 0;
                    if (!op.results().empty()) w = graph.valueWidth(op.results()[0]);
                    if (w > 0 && w < 64) {
                        uint64_t mask = (uint64_t(1) << w) - 1;
                        std::ostringstream ss;
                        ss << "((~(" << getOperandExpr(0) << " ^ " << getOperandExpr(1) << ")) & 0x" << std::hex << mask << "ULL)";
                        setResultExpr(0, ss.str());
                    } else {
                        setResultExpr(0, "(~(" + getOperandExpr(0) + " ^ " + getOperandExpr(1) + "))");
                    }
                    break;
                }
                // Arithmetic
                case OperationKind::kMod: {
                    const auto resultType = getCppTypeForWidth(graph.valueWidth(op.results()[0]));
                    const auto zero = resultType + "(0)";
                    setResultExpr(0,
                                  "((" + getOperandExpr(1) + ") == " + zero + " ? " + zero + " : (" +
                                  getOperandExpr(0) + " % " + getOperandExpr(1) + "))");
                    break;
                }
                // Shift operations
                case OperationKind::kShl: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " << " + getShiftAmountExpr(1) + ")");
                    break;
                }
                case OperationKind::kLShr: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " >> " + getShiftAmountExpr(1) + ")");
                    break;
                }
                case OperationKind::kAShr: {
                    const auto operandWidth = graph.valueWidth(op.operands()[0]);
                    if (operandWidth > 64) {
                        setResultExpr(0, "(arithmeticShiftRight(" + getOperandExpr(0) + ", " + getShiftAmountExpr(1) + "))");
                    } else {
                        const auto operandExpr = getOperandExpr(0);
                        setResultExpr(
                            0,
                            "(static_cast<std::make_signed_t<std::remove_cv_t<std::remove_reference_t<decltype(" +
                                operandExpr + ")>>>>(" +
                                operandExpr + ") >> " + getShiftAmountExpr(1) + ")");
                    }
                    break;
                }
                // Reduce operations
                case OperationKind::kReduceAnd: {
                    int64_t w = graph.valueWidth(op.operands()[0]);
                    if (w > 0 && w <= 64) {
                        uint64_t mask = (w == 64) ? ~uint64_t(0) : ((uint64_t(1) << w) - 1);
                        setResultExpr(0, "((" + getOperandExpr(0) + " & 0x" + ([&]{ std::ostringstream ss; ss << std::hex << mask; return ss.str(); })() + "ULL) == 0x" + ([&]{ std::ostringstream ss; ss << std::hex << mask; return ss.str(); })() + "ULL ? 1 : 0)");
                    } else if (w > 64) {
                        setResultExpr(0, "(wolvrix::gsim::reduceAnd(" + getOperandExpr(0) + "))");
                    } else {
                        setResultExpr(0, "0");
                    }
                    break;
                }
                case OperationKind::kReduceOr: {
                    const int64_t w = graph.valueWidth(op.operands()[0]);
                    if (w > 64) {
                        setResultExpr(0, "(wolvrix::gsim::reduceOr(" + getOperandExpr(0) + "))");
                    } else {
                        setResultExpr(0, "(" + getOperandExpr(0) + " != 0 ? 1 : 0)");
                    }
                    break;
                }
                case OperationKind::kReduceXor: {
                    // XOR reduction: count set bits, result is parity
                    const int64_t w = graph.valueWidth(op.operands()[0]);
                    if (w > 64) {
                        setResultExpr(0, "(wolvrix::gsim::reduceXor(" + getOperandExpr(0) + "))");
                    } else {
                        setResultExpr(0, "(__builtin_parityll(static_cast<unsigned long long>(" + getOperandExpr(0) + ")))");
                    }
                    break;
                }
                case OperationKind::kReduceNand: {
                    int64_t w = graph.valueWidth(op.operands()[0]);
                    if (w > 0 && w <= 64) {
                        uint64_t mask = (w == 64) ? ~uint64_t(0) : ((uint64_t(1) << w) - 1);
                        setResultExpr(0, "((" + getOperandExpr(0) + " & 0x" + ([&]{ std::ostringstream ss; ss << std::hex << mask; return ss.str(); })() + "ULL) == 0x" + ([&]{ std::ostringstream ss; ss << std::hex << mask; return ss.str(); })() + "ULL ? 0 : 1)");
                    } else if (w > 64) {
                        setResultExpr(0, "((wolvrix::gsim::reduceAnd(" + getOperandExpr(0) + ")) ? 0 : 1)");
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
                    const int64_t w = graph.valueWidth(op.operands()[0]);
                    if (w > 64) {
                        setResultExpr(0, "((wolvrix::gsim::reduceXor(" + getOperandExpr(0) + ")) ^ 1)");
                    } else {
                        setResultExpr(0, "(__builtin_parityll(static_cast<unsigned long long>(" + getOperandExpr(0) + ")) ^ 1)");
                    }
                    break;
                }
                // Concat: shift operands and OR together
                case OperationKind::kConcat: {
                    if (op.operands().empty()) {
                        setResultExpr(0, "0");
                    } else if (op.operands().size() == 1) {
                        setResultExpr(0, getOperandExpr(0));
                    } else {
                        const bool wideResult = !op.results().empty() && graph.valueWidth(op.results()[0]) > 64;
                        const std::string resultType =
                            !op.results().empty() ? getCppTypeForWidth(graph.valueWidth(op.results()[0])) : "std::uint64_t";
                        std::vector<int64_t> widths;
                        for (size_t i = 0; i < op.operands().size(); ++i) {
                            widths.push_back(graph.valueWidth(op.operands()[i]));
                        }
                        if (wideResult) {
                            auto storage = reserveMutableResultStorage(0);
                            if (!storage) {
                                state.unsupportedOps.push_back(std::string(toString(kind)));
                                break;
                            }
                            state.combinationalStmts.push_back(
                                "        " + storage->expr + " = " + storage->cppType + "(" + getOperandExpr(0) + ");");
                            for (size_t i = 1; i < op.operands().size(); ++i) {
                                state.combinationalStmts.push_back(
                                    "        " + storage->expr + " = ((" + storage->expr + " << " +
                                    std::to_string(widths[i]) + ") | " + storage->cppType + "(" +
                                    getOperandExpr(i) + "));");
                            }
                        } else {
                            // Concat: first operand is MSB, operands go high-to-low
                            // result = (op0 << (w1+w2+...)) | (op1 << (w2+w3+...)) | ... | opN
                            std::string expr;
                            // Calculate total shift for each operand
                            for (size_t i = 0; i < op.operands().size(); ++i) {
                                int64_t shift = 0;
                                for (size_t j = i + 1; j < op.operands().size(); ++j) {
                                    shift += widths[j];
                                }
                                std::string part = getOperandExpr(i);
                                if (shift > 0 && shift < 64) {
                                    part = "(static_cast<std::uint64_t>(" + part + ") << " + std::to_string(shift) + ")";
                                } else if (shift >= 64) {
                                    part = "0"; // Bits beyond 64 are truncated
                                }
                                if (expr.empty()) {
                                    expr = part;
                                } else {
                                    expr = "(" + expr + " | " + part + ")";
                                }
                            }
                            setResultExpr(0, expr);
                        }
                    }
                    break;
                }
                // Replicate: repeat bits N times
                case OperationKind::kReplicate: {
                    auto countAttr = op.attr("replicateCount");
                    if (!countAttr) countAttr = op.attr("count");
                    if (!countAttr) countAttr = op.attr("rep");
                    int64_t count = 1;
                    if (countAttr) {
                        if (auto* intVal = std::get_if<int64_t>(&*countAttr)) {
                            count = *intVal;
                        }
                    }
                    if (count <= 1) {
                        setResultExpr(0, getOperandExpr(0));
                    } else {
                        const bool wideResult = !op.results().empty() && graph.valueWidth(op.results()[0]) > 64;
                        const std::string resultType =
                            !op.results().empty() ? getCppTypeForWidth(graph.valueWidth(op.results()[0])) : "std::uint64_t";
                        int64_t opWidth = graph.valueWidth(op.operands()[0]);
                        if (wideResult) {
                            auto storage = reserveMutableResultStorage(0);
                            if (!storage) {
                                state.unsupportedOps.push_back(std::string(toString(kind)));
                                break;
                            }
                            state.combinationalStmts.push_back(
                                "        " + storage->expr + " = " + storage->cppType + "{};");
                            for (int64_t i = 0; i < count; ++i) {
                                state.combinationalStmts.push_back(
                                    "        " + storage->expr + " = ((" + storage->expr + " << " +
                                    std::to_string(opWidth) + ") | " + storage->cppType + "(" +
                                    getOperandExpr(0) + "));");
                            }
                        } else {
                            std::string expr;
                            for (int64_t i = 0; i < count; ++i) {
                                std::string part = getOperandExpr(0);
                                int64_t shift = (count - 1 - i) * opWidth;
                                if (shift > 0 && shift < 64) {
                                    part = "(static_cast<std::uint64_t>(" + part + ") << " + std::to_string(shift) + ")";
                                } else if (shift >= 64) {
                                    part = "0"; // Bits beyond 64 are truncated
                                }
                                if (expr.empty()) {
                                    expr = part;
                                } else {
                                    expr = "(" + expr + " | " + part + ")";
                                }
                            }
                            setResultExpr(0, expr);
                        }
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
                    const int64_t operandWidth = graph.valueWidth(op.operands()[0]);
                    const bool wideOperand = operandWidth > 64;
                    const std::string resultType =
                        !op.results().empty() ? getCppTypeForWidth(graph.valueWidth(op.results()[0])) : "std::uint64_t";
                    if (start == 0 && width >= 64) {
                        if (wideOperand) {
                            setResultExpr(0, "(static_cast<" + resultType + ">(" + getOperandExpr(0) + "))");
                        } else {
                            setResultExpr(0, getOperandExpr(0));
                        }
                    } else {
                        uint64_t mask = (width >= 64) ? ~uint64_t(0) : ((uint64_t(1) << width) - 1);
                        std::string maskStr = ([&]{ std::ostringstream ss; ss << "0x" << std::hex << mask << "ULL"; return ss.str(); })();
                        if (start == 0) {
                            if (wideOperand) {
                                setResultExpr(0, "(static_cast<" + resultType + ">((" + getOperandExpr(0) + ") & " + maskStr + "))");
                            } else {
                                setResultExpr(0, "(" + getOperandExpr(0) + " & " + maskStr + ")");
                            }
                        } else {
                            if (wideOperand) {
                                setResultExpr(0,
                                              "(static_cast<" + resultType + ">(((" + getOperandExpr(0) + " >> " +
                                                  std::to_string(start) + ") & " + maskStr + ")))");
                            } else {
                                setResultExpr(0, "((" + getOperandExpr(0) + " >> " + std::to_string(start) + ") & " + maskStr + ")");
                            }
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
                    const int64_t operandWidth = graph.valueWidth(op.operands()[0]);
                    const bool wideOperand = operandWidth > 64;
                    const std::string resultType =
                        !op.results().empty() ? getCppTypeForWidth(graph.valueWidth(op.results()[0])) : "std::uint64_t";
                    if (width >= 64) {
                        if (wideOperand) {
                            setResultExpr(0, "(static_cast<" + resultType + ">(" + getOperandExpr(0) + " >> " + getShiftAmountExpr(1) + "))");
                        } else {
                            setResultExpr(0, "(" + getOperandExpr(0) + " >> " + getShiftAmountExpr(1) + ")");
                        }
                    } else {
                        uint64_t mask = (uint64_t(1) << width) - 1;
                        std::string maskStr = ([&]{ std::ostringstream ss; ss << "0x" << std::hex << mask << "ULL"; return ss.str(); })();
                        if (wideOperand) {
                            setResultExpr(0,
                                          "(static_cast<" + resultType + ">(((" + getOperandExpr(0) + " >> " +
                                              getShiftAmountExpr(1) + ") & " + maskStr + ")))");
                        } else {
                            setResultExpr(0, "((" + getOperandExpr(0) + " >> " + getShiftAmountExpr(1) + ") & " + maskStr + ")");
                        }
                    }
                    break;
                }
                // Latch (level-sensitive storage)
                case OperationKind::kLatch: {
                    // Latch defines storage - handled similarly to register
                    break;
                }
                case OperationKind::kMemory: {
                    // Memory storage is declared during pre-collection.
                    break;
                }
                case OperationKind::kMemoryReadPort: {
                    auto memSymAttr = op.attr("memSymbol");
                    std::string sym;
                    if (memSymAttr) {
                        if (auto* strVal = std::get_if<std::string>(&*memSymAttr)) sym = *strVal;
                    }
                    const auto memIt = state.memoryStorageNames.find(sym);
                    const auto rowIt = state.memoryRows.find(sym);
                    if (memIt == state.memoryStorageNames.end() || rowIt == state.memoryRows.end() || rowIt->second <= 0) {
                        std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                        state.unsupportedOps.push_back(std::string(toString(kind)) + " (" + opName + ")");
                        break;
                    }
                    setResultExpr(0,
                                  memIt->second + "[static_cast<std::size_t>(" + getOperandExpr(0) + ") % " +
                                      std::to_string(rowIt->second) + "ULL]");
                    break;
                }
                case OperationKind::kMemoryWritePort: {
                    std::string condition = getOperandExpr(0);
                    std::string addr = getOperandExpr(1);
                    std::string data = getOperandExpr(2);
                    std::string mask = getOperandExpr(3);
                    if (state.hasResetInput) {
                        condition = "(!input_reset_ && (" + condition + "))";
                    }

                    auto memSymAttr = op.attr("memSymbol");
                    std::string sym;
                    if (memSymAttr) {
                        if (auto* strVal = std::get_if<std::string>(&*memSymAttr)) sym = *strVal;
                    }
                    const auto memIt = state.memoryStorageNames.find(sym);
                    const auto rowIt = state.memoryRows.find(sym);
                    if (memIt == state.memoryStorageNames.end() || rowIt == state.memoryRows.end() || rowIt->second <= 0) {
                        std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                        state.unsupportedOps.push_back(std::string(toString(kind)) + " (" + opName + ")");
                        break;
                    }

                    const std::string idxName = "mem_index_" + std::to_string(op.id().index);
                    state.sequentialStmts["posedge_clock"].push_back(
                        "        if (" + condition + ") { const std::size_t " + idxName + " = static_cast<std::size_t>(" + addr + ") % " +
                        std::to_string(rowIt->second) + "ULL; " + memIt->second + "[" + idxName + "] = (" +
                        memIt->second + "[" + idxName + "] & ~" + mask + ") | (" + data + " & " + mask + "); }");
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
                        if (!op.results().empty()) {
                            state.valueExprs[op.results()[0]] = latchName;
                        }
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
                        if (!op.results().empty()) {
                            state.valueExprs[op.results()[0]] = regName;
                        }
                    }
                    break;
                }
                case OperationKind::kRegisterWritePort: {
                    std::string condition = getOperandExpr(0);
                    std::string nextValue = getOperandExpr(1);
                    std::string mask = getOperandExpr(2);
                    if (state.hasResetInput) {
                        condition = "(!input_reset_ && (" + condition + "))";
                    }

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
                case OperationKind::kDpicImport: {
                    // Signatures are pre-collected before lowering.
                    break;
                }
                case OperationKind::kDpicCall: {
                    const std::string opName =
                        op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                    const std::string targetImport =
                        getAttrAs<std::string>(op, "targetImportSymbol").value_or("");
                    const auto importIt = state.dpiImports.find(targetImport);
                    if (targetImport.empty() || importIt == state.dpiImports.end())
                    {
                        state.unsupportedOps.push_back("kDpicCall (" + opName + ": unresolved DPI import)");
                        break;
                    }

                    const auto inArgName = getAttrAs<std::vector<std::string>>(op, "inArgName").value_or(std::vector<std::string>{});
                    const auto outArgName = getAttrAs<std::vector<std::string>>(op, "outArgName").value_or(std::vector<std::string>{});
                    const auto inoutArgName = getAttrAs<std::vector<std::string>>(op, "inoutArgName").value_or(std::vector<std::string>{});
                    const auto eventEdge = getAttrAs<std::vector<std::string>>(op, "eventEdge").value_or(std::vector<std::string>{});
                    const bool hasReturn = getAttrAs<bool>(op, "hasReturn").value_or(false);
                    if (!inoutArgName.empty())
                    {
                        state.unsupportedOps.push_back("kDpicCall (" + opName + ": inout DPI calls are unsupported)");
                        break;
                    }
                    if (op.operands().size() < 1 + eventEdge.size())
                    {
                        state.unsupportedOps.push_back("kDpicCall (" + opName + ": operand count does not match eventEdge)");
                        break;
                    }

                    const auto eventStart = op.operands().size() - eventEdge.size();
                    if (eventStart < 1 || eventStart != 1 + inArgName.size())
                    {
                        state.unsupportedOps.push_back("kDpicCall (" + opName + ": operand count does not match input args)");
                        break;
                    }

                    const auto &sig = importIt->second;
                    if (sig.hasReturn != hasReturn)
                    {
                        state.unsupportedOps.push_back("kDpicCall (" + opName + ": hasReturn does not match import signature)");
                        break;
                    }

                    const std::size_t outputOffset = hasReturn ? 1 : 0;
                    if (op.results().size() != outputOffset + outArgName.size())
                    {
                        state.unsupportedOps.push_back("kDpicCall (" + opName + ": result count does not match return/output args)");
                        break;
                    }

                    std::map<std::string, std::string> outputArgCppTypes;
                    for (const auto &arg : sig.args)
                    {
                        if (arg.direction == "output")
                        {
                            outputArgCppTypes.emplace(arg.name, arg.cppType);
                        }
                    }

                    std::map<std::string, std::string> outputTemps;
                    for (std::size_t resultIndex = outputOffset; resultIndex < op.results().size(); ++resultIndex)
                    {
                        const auto &argName = outArgName[resultIndex - outputOffset];
                        const auto typeIt = outputArgCppTypes.find(argName);
                        if (typeIt == outputArgCppTypes.end())
                        {
                            state.unsupportedOps.push_back(
                                "kDpicCall (" + opName + ": missing DPI output signature for " + argName + ")");
                            outputTemps.clear();
                            break;
                        }

                        const auto valueId = op.results()[resultIndex];
                        const auto name = dpiScratchValueName("dpi_out", valueId);
                        state.combinationalStmts.push_back("        " + typeIt->second + " " + name + " = 0;");
                        outputTemps.emplace(argName, name);
                    }
                    if (outputTemps.size() != outArgName.size())
                    {
                        break;
                    }

                    std::string returnTemp;
                    if (hasReturn)
                    {
                        returnTemp = dpiScratchValueName("dpi_ret", op.results()[0]);
                        state.combinationalStmts.push_back("        " + sig.returnCppType + " " + returnTemp + " = 0;");
                    }

                    std::ostringstream call;
                    if (hasReturn)
                    {
                        call << returnTemp << " = ";
                    }
                    call << sig.symbol << "(";
                    bool firstArg = true;
                    for (const auto &arg : sig.args)
                    {
                        if (!firstArg)
                        {
                            call << ", ";
                        }
                        firstArg = false;
                        if (arg.direction == "input")
                        {
                            const int idx = findNamedArgIndex(inArgName, arg.name);
                            if (idx < 0)
                            {
                                state.unsupportedOps.push_back(
                                    "kDpicCall (" + opName + ": missing DPI input arg " + arg.name + ")");
                                call.str(std::string{});
                                break;
                            }
                            call << getOperandExpr(static_cast<std::size_t>(idx + 1));
                        }
                        else if (arg.direction == "output")
                        {
                            const auto outIt = outputTemps.find(arg.name);
                            if (outIt == outputTemps.end())
                            {
                                state.unsupportedOps.push_back(
                                    "kDpicCall (" + opName + ": missing DPI output arg " + arg.name + ")");
                                call.str(std::string{});
                                break;
                            }
                            call << "&" << outIt->second;
                        }
                    }
                    if (call.str().empty())
                    {
                        break;
                    }
                    call << ");";

                    const std::string condition = getOperandExpr(0);
                    state.combinationalStmts.push_back("        if (" + condition + ") { " + call.str() + " }");

                    if (hasReturn)
                    {
                        setResultExpr(0, returnTemp);
                    }
                    for (std::size_t resultIndex = outputOffset; resultIndex < op.results().size(); ++resultIndex)
                    {
                        const auto outIt = outputTemps.find(outArgName[resultIndex - outputOffset]);
                        if (outIt != outputTemps.end())
                        {
                            setResultExpr(resultIndex, outIt->second);
                        }
                    }
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
                if (sanitizeIdentifier(port.name) == "reset")
                {
                    state.hasResetInput = true;
                }
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
            std::map<std::string, int32_t> storageWidths;
            std::map<std::string, std::string> storageInitExprs;
            std::vector<std::string> storageOrder;

            auto noteStorage = [&](const std::string& storageName, int32_t width) {
                const int32_t clampedWidth = std::max<int32_t>(1, width);
                const auto [it, inserted] = storageWidths.emplace(storageName, clampedWidth);
                if (inserted)
                {
                    storageOrder.push_back(storageName);
                }
                else if (clampedWidth > it->second)
                {
                    it->second = clampedWidth;
                }
            };
            auto noteStorageInit = [&](const std::string& storageName, const std::string& initValue) {
                if (initValue.empty())
                {
                    return;
                }

                std::string initExpr = "0";
                if (!initValue.empty() && initValue.front() != '$')
                {
                    initExpr = convertVerilogConstant(initValue);
                }
                storageInitExprs.emplace(storageName, std::move(initExpr));
            };

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
                    noteStorage(regName, width);
                    if (auto initValueAttr = op.attr("initValue"))
                    {
                        if (auto* initValue = std::get_if<std::string>(&*initValueAttr))
                        {
                            noteStorageInit(regName, *initValue);
                        }
                    }
                    if (!op.results().empty()) {
                        state.valueExprs[op.results()[0]] = regName;
                    }
                }
                if (op.kind() == wolvrix::lib::grh::OperationKind::kLatch) {
                    std::string sym = std::string(op.symbolText());
                    if (sym.empty()) sym = "unnamed_latch_" + std::to_string(opId.index);
                    std::string latchName = "latch_" + sanitizeIdentifier(sym);

                    int32_t width = 32;
                    if (!op.results().empty()) {
                        auto val = graph.getValue(op.results()[0]);
                        width = val.width();
                    }
                    noteStorage(latchName, width);
                    if (!op.results().empty()) {
                        state.valueExprs[op.results()[0]] = latchName;
                    }
                }
                // Also collect storage names from read/write port attrs
                if (op.kind() == wolvrix::lib::grh::OperationKind::kRegisterReadPort ||
                    op.kind() == wolvrix::lib::grh::OperationKind::kRegisterWritePort) {
                    auto regSymAttr = op.attr("regSymbol");
                    std::string sym;
                    if (regSymAttr) {
                        if (auto* strVal = std::get_if<std::string>(&*regSymAttr)) sym = *strVal;
                    }
                    if (!sym.empty()) {
                        std::string regName = "reg_" + sanitizeIdentifier(sym);
                        int32_t width = 32;
                        if (op.kind() == wolvrix::lib::grh::OperationKind::kRegisterReadPort && !op.results().empty()) {
                            width = graph.getValue(op.results()[0]).width();
                        } else if (op.kind() == wolvrix::lib::grh::OperationKind::kRegisterWritePort && op.operands().size() > 1) {
                            width = graph.valueWidth(op.operands()[1]);
                        }
                        noteStorage(regName, width);
                    }
                }
                if (op.kind() == wolvrix::lib::grh::OperationKind::kLatchReadPort ||
                    op.kind() == wolvrix::lib::grh::OperationKind::kLatchWritePort) {
                    auto latchSymAttr = op.attr("latchSymbol");
                    std::string sym;
                    if (latchSymAttr) {
                        if (auto* strVal = std::get_if<std::string>(&*latchSymAttr)) sym = *strVal;
                    }
                    if (!sym.empty()) {
                        std::string latchName = "latch_" + sanitizeIdentifier(sym);
                        int32_t width = 32;
                        if (op.kind() == wolvrix::lib::grh::OperationKind::kLatchReadPort && !op.results().empty()) {
                            width = graph.getValue(op.results()[0]).width();
                        } else if (op.kind() == wolvrix::lib::grh::OperationKind::kLatchWritePort && op.operands().size() > 1) {
                            width = graph.valueWidth(op.operands()[1]);
                        }
                        noteStorage(latchName, width);
                    }
                }
            }

            for (const auto& storageName : storageOrder)
            {
                const auto widthIt = storageWidths.find(storageName);
                if (widthIt == storageWidths.end())
                {
                    continue;
                }
                const auto initExprIt = storageInitExprs.find(storageName);
                const std::string initExpr = initExprIt == storageInitExprs.end() ? "0" : initExprIt->second;
                state.storageDecls.push_back(getCppTypeForWidth(widthIt->second) + " " + storageName + " = " + initExpr + ";");
                state.resetStmts.push_back("        " + storageName + " = " + initExpr + ";");
            }
        }

        void collectMemories(const wolvrix::lib::grh::Graph& graph, CodegenState& state)
        {
            std::set<std::string> declaredStorage;
            for (const auto& decl : state.storageDecls)
            {
                const auto eqPos = decl.find(" = ");
                if (eqPos == std::string::npos)
                {
                    continue;
                }
                const auto spacePos = decl.rfind(' ', eqPos - 1);
                if (spacePos == std::string::npos)
                {
                    continue;
                }
                declaredStorage.insert(decl.substr(spacePos + 1, eqPos - spacePos - 1));
            }

            for (const auto opId : graph.operations()) {
                auto op = graph.getOperation(opId);
                if (op.kind() != wolvrix::lib::grh::OperationKind::kMemory) {
                    continue;
                }
                const std::string sym = std::string(op.symbolText());
                if (sym.empty()) {
                    continue;
                }

                int64_t width = 32;
                int64_t rows = 1;
                if (auto widthAttr = op.attr("width")) {
                    if (auto* intVal = std::get_if<int64_t>(&*widthAttr)) width = *intVal;
                }
                if (auto rowAttr = op.attr("row")) {
                    if (auto* intVal = std::get_if<int64_t>(&*rowAttr)) rows = *intVal;
                }
                if (rows <= 0) {
                    rows = 1;
                }

                const std::string memName = "mem_" + sanitizeIdentifier(sym) + "_";
                state.memoryStorageNames[sym] = memName;
                state.memoryRows[sym] = rows;
                if (declaredStorage.insert(memName).second) {
                    const std::string type = getCppTypeForWidth(static_cast<int32_t>(width));
                    state.storageDecls.push_back("std::vector<" + type + "> " + memName + ";");
                    state.ctorStmts.push_back(
                        "    " + memName + ".resize(" + std::to_string(rows) + ");");
                    state.resetStmts.push_back(
                        "        std::fill(" + memName + ".begin(), " + memName + ".end(), 0);");
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

        std::string renderWideBitsSupport()
        {
            return R"cpp(namespace wolvrix::gsim {

template <std::size_t Width>
struct Bits {
    static_assert(Width > 0, "Bits width must be positive");
    static constexpr std::size_t kWordBits = 64;
    static constexpr std::size_t kWordCount = (Width + kWordBits - 1) / kWordBits;

    std::array<std::uint64_t, kWordCount> words{};

    constexpr Bits() = default;

    constexpr Bits(std::uint64_t value) {
        words.fill(0);
        words[0] = value;
        maskUnusedBits();
    }

    template <std::size_t OtherWidth>
    constexpr Bits(const Bits<OtherWidth>& other) {
        words.fill(0);
        constexpr std::size_t copyCount = kWordCount < Bits<OtherWidth>::kWordCount ? kWordCount : Bits<OtherWidth>::kWordCount;
        for (std::size_t index = 0; index < copyCount; ++index) {
            words[index] = other.words[index];
        }
        maskUnusedBits();
    }

    constexpr void maskUnusedBits() {
        constexpr std::size_t remainder = Width % kWordBits;
        if constexpr (remainder != 0) {
            words[kWordCount - 1] &= ((std::uint64_t{1} << remainder) - 1);
        }
    }

    constexpr explicit operator bool() const {
        for (std::size_t index = 0; index < kWordCount; ++index) {
            if (words[index] != 0) {
                return true;
            }
        }
        return false;
    }

    constexpr explicit operator std::uint64_t() const {
        return words[0];
    }

    friend constexpr bool operator==(const Bits& lhs, const Bits& rhs) {
        return lhs.words == rhs.words;
    }

    friend constexpr bool operator!=(const Bits& lhs, const Bits& rhs) {
        return !(lhs == rhs);
    }

    friend constexpr bool operator<(const Bits& lhs, const Bits& rhs) {
        for (std::size_t index = kWordCount; index-- > 0;) {
            if (lhs.words[index] != rhs.words[index]) {
                return lhs.words[index] < rhs.words[index];
            }
        }
        return false;
    }

    friend constexpr bool operator<=(const Bits& lhs, const Bits& rhs) {
        return !(rhs < lhs);
    }

    friend constexpr bool operator>(const Bits& lhs, const Bits& rhs) {
        return rhs < lhs;
    }

    friend constexpr bool operator>=(const Bits& lhs, const Bits& rhs) {
        return !(lhs < rhs);
    }

    friend constexpr Bits operator~(Bits value) {
        for (std::size_t index = 0; index < kWordCount; ++index) {
            value.words[index] = ~value.words[index];
        }
        value.maskUnusedBits();
        return value;
    }

    friend constexpr Bits operator|(Bits lhs, const Bits& rhs) {
        for (std::size_t index = 0; index < kWordCount; ++index) {
            lhs.words[index] |= rhs.words[index];
        }
        lhs.maskUnusedBits();
        return lhs;
    }

    friend constexpr Bits operator&(Bits lhs, const Bits& rhs) {
        for (std::size_t index = 0; index < kWordCount; ++index) {
            lhs.words[index] &= rhs.words[index];
        }
        lhs.maskUnusedBits();
        return lhs;
    }

    friend constexpr std::uint64_t operator&(Bits lhs, std::uint64_t rhs) {
        return lhs.words[0] & rhs;
    }

    friend constexpr Bits operator^(Bits lhs, const Bits& rhs) {
        for (std::size_t index = 0; index < kWordCount; ++index) {
            lhs.words[index] ^= rhs.words[index];
        }
        lhs.maskUnusedBits();
        return lhs;
    }

    friend constexpr Bits operator<<(Bits value, std::size_t shift) {
        if (shift >= Width) {
            return Bits{};
        }
        Bits result;
        const std::size_t wordShift = shift / kWordBits;
        const std::size_t bitShift = shift % kWordBits;
        for (std::size_t index = 0; index < kWordCount; ++index) {
            if (value.words[index] == 0) {
                continue;
            }
            const std::size_t target = index + wordShift;
            if (target >= kWordCount) {
                continue;
            }
            result.words[target] |= value.words[index] << bitShift;
            if (bitShift != 0 && target + 1 < kWordCount) {
                result.words[target + 1] |= value.words[index] >> (kWordBits - bitShift);
            }
        }
        result.maskUnusedBits();
        return result;
    }

    friend constexpr Bits operator>>(Bits value, std::size_t shift) {
        if (shift >= Width) {
            return Bits{};
        }
        Bits result;
        const std::size_t wordShift = shift / kWordBits;
        const std::size_t bitShift = shift % kWordBits;
        for (std::size_t index = wordShift; index < kWordCount; ++index) {
            const std::size_t target = index - wordShift;
            result.words[target] |= value.words[index] >> bitShift;
            if (bitShift != 0 && index + 1 < kWordCount) {
                result.words[target] |= value.words[index + 1] << (kWordBits - bitShift);
            }
        }
        result.maskUnusedBits();
        return result;
    }

    friend constexpr Bits arithmeticShiftRight(Bits value, std::size_t shift) {
        constexpr std::size_t signBitIndex = Width - 1;
        const bool sign = ((value.words[signBitIndex / kWordBits] >> (signBitIndex % kWordBits)) & 0x1ULL) != 0;
        if (shift >= Width) {
            if (!sign) {
                return Bits{};
            }
            Bits result;
            for (std::size_t index = 0; index < kWordCount; ++index) {
                result.words[index] = 0xFFFFFFFFFFFFFFFFULL;
            }
            result.maskUnusedBits();
            return result;
        }

        Bits result = value >> shift;
        if (!sign || shift == 0) {
            return result;
        }
        for (std::size_t bitIndex = Width - shift; bitIndex < Width; ++bitIndex) {
            result.words[bitIndex / kWordBits] |= std::uint64_t{1} << (bitIndex % kWordBits);
        }
        result.maskUnusedBits();
        return result;
    }

    friend constexpr Bits operator+(const Bits& lhs, const Bits& rhs) {
        Bits result;
        unsigned __int128 carry = 0;
        for (std::size_t index = 0; index < kWordCount; ++index) {
            const unsigned __int128 sum =
                static_cast<unsigned __int128>(lhs.words[index]) +
                static_cast<unsigned __int128>(rhs.words[index]) +
                carry;
            result.words[index] = static_cast<std::uint64_t>(sum);
            carry = sum >> kWordBits;
        }
        result.maskUnusedBits();
        return result;
    }

    friend constexpr Bits operator-(const Bits& lhs, const Bits& rhs) {
        Bits result;
        std::uint64_t borrow = 0;
        for (std::size_t index = 0; index < kWordCount; ++index) {
            const std::uint64_t rhsWord = rhs.words[index] + borrow;
            borrow = (lhs.words[index] < rhsWord) ? 1 : 0;
            result.words[index] = lhs.words[index] - rhsWord;
        }
        result.maskUnusedBits();
        return result;
    }
};

template <std::size_t Width>
constexpr std::uint8_t reduceOr(const Bits<Width>& value) {
    return static_cast<std::uint8_t>(static_cast<bool>(value));
}

template <std::size_t Width>
constexpr std::uint8_t reduceXor(const Bits<Width>& value) {
    std::uint8_t parity = 0;
    for (std::size_t wordIndex = 0; wordIndex < Bits<Width>::kWordCount; ++wordIndex) {
        parity ^= static_cast<std::uint8_t>(__builtin_parityll(value.words[wordIndex]));
    }
    return parity & 0x1u;
}

template <std::size_t Width>
constexpr std::uint8_t reduceAnd(const Bits<Width>& value) {
    for (std::size_t wordIndex = 0; wordIndex + 1 < Bits<Width>::kWordCount; ++wordIndex) {
        if (value.words[wordIndex] != 0xFFFFFFFFFFFFFFFFULL) {
            return 0;
        }
    }
    constexpr std::size_t remainder = Width % Bits<Width>::kWordBits;
    if constexpr (remainder == 0) {
        return value.words[Bits<Width>::kWordCount - 1] == 0xFFFFFFFFFFFFFFFFULL ? 1 : 0;
    }
    const std::uint64_t mask = (std::uint64_t{1} << remainder) - 1;
    return (value.words[Bits<Width>::kWordCount - 1] & mask) == mask ? 1 : 0;
}

} // namespace wolvrix::gsim

)cpp";
        }

        constexpr std::size_t kDefaultMetadataShardMaxBytes = 16u * 1024u * 1024u;
        constexpr std::size_t kDefaultBehaviorShardMaxBytes = 256u * 1024u * 1024u;

        std::optional<std::size_t> parseMetadataShardMaxBytes(const EmitOptions &options,
                                                              EmitDiagnostics *diagnostics)
        {
            const auto attr = attrValue(options, "metadata_shard_max_bytes");
            if (!attr)
            {
                return kDefaultMetadataShardMaxBytes;
            }

            try
            {
                const auto parsed = std::stoull(*attr);
                if (parsed == 0)
                {
                    if (diagnostics != nullptr)
                    {
                        diagnostics->error("metadata_shard_max_bytes must be greater than zero", *attr);
                    }
                    return std::nullopt;
                }
                return static_cast<std::size_t>(parsed);
            }
            catch (const std::exception &)
            {
                if (diagnostics != nullptr)
                {
                    diagnostics->error("metadata_shard_max_bytes must be an unsigned integer", *attr);
                }
                return std::nullopt;
            }
        }

        std::optional<std::size_t> parseBehaviorShardMaxBytes(const EmitOptions &options,
                                                              EmitDiagnostics *diagnostics)
        {
            const auto attr = attrValue(options, "behavior_shard_max_bytes");
            if (!attr)
            {
                return kDefaultBehaviorShardMaxBytes;
            }

            try
            {
                const auto parsed = std::stoull(*attr);
                if (parsed == 0)
                {
                    if (diagnostics != nullptr)
                    {
                        diagnostics->error("behavior_shard_max_bytes must be greater than zero", *attr);
                    }
                    return std::nullopt;
                }
                return static_cast<std::size_t>(parsed);
            }
            catch (const std::exception &)
            {
                if (diagnostics != nullptr)
                {
                    diagnostics->error("behavior_shard_max_bytes must be an unsigned integer", *attr);
                }
                return std::nullopt;
            }
        }

        // Keep metadata statements materially smaller than shard budgets. Very long
        // insert(...) expressions compile poorly on large XiangShan metadata shards.
        constexpr std::size_t kDefaultMetadataStatementTargetBytes = 4u * 1024u;
        constexpr std::size_t kMetadataStatementTerminatorBytes = 1;

        std::size_t chooseMetadataStatementTargetBytes(std::size_t shardMaxBytes)
        {
            return std::min(shardMaxBytes, kDefaultMetadataStatementTargetBytes);
        }

        void appendMetadataStatement(std::vector<std::string> &statements, std::string statement)
        {
            if (statement.empty())
            {
                return;
            }
            if (statement.back() != '\n')
            {
                statement.push_back('\n');
            }
            statements.push_back(std::move(statement));
        }

        template <typename Iter, typename RenderItem>
        void appendChunkedMetadataInsertStatements(std::vector<std::string> &statements,
                                                  std::string_view prefix,
                                                  std::string_view suffix,
                                                  std::size_t targetBytes,
                                                  Iter begin,
                                                  Iter end,
                                                  RenderItem &&renderItem)
        {
            if (begin == end)
            {
                return;
            }

            std::string current(prefix);
            bool hasItems = false;
            for (auto it = begin; it != end; ++it)
            {
                std::string item = renderItem(*it);
                const std::size_t separatorBytes = hasItems ? 2 : 0;
                if (hasItems &&
                    current.size() + separatorBytes + item.size() + suffix.size() + kMetadataStatementTerminatorBytes > targetBytes)
                {
                    current.append(suffix);
                    appendMetadataStatement(statements, std::move(current));
                    current = std::string(prefix);
                    hasItems = false;
                }

                if (hasItems)
                {
                    current.append(", ");
                }
                current.append(item);
                hasItems = true;
            }

            current.append(suffix);
            appendMetadataStatement(statements, std::move(current));
        }

        template <typename Iter, typename RenderItem>
        void appendChunkedMetadataArrayLoopStatements(std::vector<std::string> &statements,
                                                     std::string_view blockPrefix,
                                                     std::string_view blockSuffix,
                                                     std::size_t targetBytes,
                                                     Iter begin,
                                                     Iter end,
                                                     RenderItem &&renderItem)
        {
            if (begin == end)
            {
                return;
            }

            std::string current(blockPrefix);
            bool hasItems = false;
            for (auto it = begin; it != end; ++it)
            {
                std::string item = renderItem(*it);
                const std::size_t separatorBytes = hasItems ? 2 : 0;
                if (hasItems &&
                    current.size() + separatorBytes + item.size() + blockSuffix.size() +
                            kMetadataStatementTerminatorBytes >
                        targetBytes)
                {
                    current.append(blockSuffix);
                    appendMetadataStatement(statements, std::move(current));
                    current = std::string(blockPrefix);
                    hasItems = false;
                }

                if (hasItems)
                {
                    current.append(", ");
                }
                current.append(item);
                hasItems = true;
            }

            current.append(blockSuffix);
            appendMetadataStatement(statements, std::move(current));
        }

        void emitIntVectorMetadataStatements(std::vector<std::string> &statements,
                                            std::string_view target,
                                            const std::vector<int64_t> &items,
                                            std::size_t targetBytes)
        {
            const std::string targetExpr(target);
            appendMetadataStatement(statements, "    " + targetExpr + ".clear();");
            appendChunkedMetadataArrayLoopStatements(
                statements,
                "    {\n"
                "        static constexpr std::int64_t values[] = {",
                "};\n"
                "        for (const auto value : values) {\n"
                "            " +
                    targetExpr + ".push_back(value);\n"
                "        }\n"
                "    }",
                targetBytes,
                items.begin(),
                items.end(),
                [&](int64_t item) { return std::to_string(item); });
        }

        void emitStringVectorMetadataStatements(std::vector<std::string> &statements,
                                               std::string_view target,
                                               const std::vector<std::string> &items,
                                               std::size_t targetBytes)
        {
            const std::string targetExpr(target);
            appendMetadataStatement(statements, "    " + targetExpr + ".clear();");

            const std::string insertPrefix = "    " + targetExpr + ".insert(" + targetExpr + ".end(), {";
            const std::string insertSuffix = "});";

            auto appendStringChunks = [&](std::string_view value, std::string_view appendTarget) {
                const std::string appendPrefix = "    " + std::string(appendTarget) + " += ";
                const std::string appendSuffix = ";";
                std::string chunk;
                auto flushChunk = [&]() {
                    if (chunk.empty())
                    {
                        return;
                    }
                    appendMetadataStatement(statements, appendPrefix + cppStringLiteral(chunk) + appendSuffix);
                    chunk.clear();
                };

                for (char ch : value)
                {
                    std::string candidate = chunk;
                    candidate.push_back(ch);
                    if (!chunk.empty() &&
                        appendPrefix.size() + cppStringLiteral(candidate).size() + appendSuffix.size() +
                            kMetadataStatementTerminatorBytes > targetBytes)
                    {
                        flushChunk();
                    }
                    chunk.push_back(ch);
                }
                flushChunk();
            };

            std::string current(insertPrefix);
            bool hasItems = false;
            auto flushInsert = [&]() {
                if (!hasItems)
                {
                    return;
                }
                current.append(insertSuffix);
                appendMetadataStatement(statements, std::move(current));
                current = insertPrefix;
                hasItems = false;
            };

            for (const auto &item : items)
            {
                const std::string rendered = cppStringLiteral(item);
                if (insertPrefix.size() + rendered.size() + insertSuffix.size() + kMetadataStatementTerminatorBytes > targetBytes)
                {
                    flushInsert();
                    appendMetadataStatement(statements, "    " + targetExpr + ".emplace_back();");
                    appendStringChunks(item, targetExpr + ".back()");
                    continue;
                }

                if (hasItems &&
                    current.size() + 2 + rendered.size() + insertSuffix.size() + kMetadataStatementTerminatorBytes > targetBytes)
                {
                    flushInsert();
                }

                if (hasItems)
                {
                    current.append(", ");
                }
                current.append(rendered);
                hasItems = true;
            }

            flushInsert();
        }

        template <typename Entries, typename RenderKey>
        void emitVectorMapMetadataStatements(std::vector<std::string> &statements,
                                            std::string_view target,
                                            const Entries &entries,
                                            std::size_t targetBytes,
                                            RenderKey &&renderKey)
        {
            const std::string targetExpr(target);
            appendMetadataStatement(statements, "    " + targetExpr + ".clear();");
            for (const auto &[key, ids] : entries)
            {
                const std::string keyExpr = renderKey(key);
                const std::string valueTarget = targetExpr + "[" + keyExpr + "]";
                emitIntVectorMetadataStatements(statements, valueTarget, ids, targetBytes);
            }
        }

        template <typename Entries, typename RenderEntry>
        void emitMapInsertMetadataStatements(std::vector<std::string> &statements,
                                            std::string_view target,
                                            const Entries &entries,
                                            std::size_t targetBytes,
                                            RenderEntry &&renderEntry)
        {
            const std::string targetExpr(target);
            appendMetadataStatement(statements, "    " + targetExpr + ".clear();");
            appendChunkedMetadataInsertStatements(
                statements,
                "    " + targetExpr + ".insert({",
                "});",
                targetBytes,
                entries.begin(),
                entries.end(),
                [&](const auto &entry) { return renderEntry(entry); });
        }

        template <typename Entries, typename RenderKey>
        void emitStringValueMapMetadataStatements(std::vector<std::string> &statements,
                                                 std::string_view target,
                                                 const Entries &entries,
                                                 std::size_t targetBytes,
                                                 RenderKey &&renderKey)
        {
            const std::string targetExpr(target);
            appendMetadataStatement(statements, "    " + targetExpr + ".clear();");

            const std::string insertPrefix = "    " + targetExpr + ".insert({";
            const std::string insertSuffix = "});";

            auto appendStringChunks = [&](std::string_view value, std::string_view appendTarget) {
                const std::string appendPrefix = "    " + std::string(appendTarget) + " += ";
                const std::string appendSuffix = ";";
                std::string chunk;
                auto flushChunk = [&]() {
                    if (chunk.empty())
                    {
                        return;
                    }
                    appendMetadataStatement(statements, appendPrefix + cppStringLiteral(chunk) + appendSuffix);
                    chunk.clear();
                };

                for (char ch : value)
                {
                    std::string candidate = chunk;
                    candidate.push_back(ch);
                    if (!chunk.empty() &&
                        appendPrefix.size() + cppStringLiteral(candidate).size() + appendSuffix.size() +
                            kMetadataStatementTerminatorBytes > targetBytes)
                    {
                        flushChunk();
                    }
                    chunk.push_back(ch);
                }
                flushChunk();
            };

            std::string current(insertPrefix);
            bool hasItems = false;
            auto flushInsert = [&]() {
                if (!hasItems)
                {
                    return;
                }
                current.append(insertSuffix);
                appendMetadataStatement(statements, std::move(current));
                current = insertPrefix;
                hasItems = false;
            };

            for (const auto &[key, value] : entries)
            {
                const std::string keyExpr = renderKey(key);
                const std::string rendered = "{" + keyExpr + ", " + cppStringLiteral(value) + "}";
                if (insertPrefix.size() + rendered.size() + insertSuffix.size() + kMetadataStatementTerminatorBytes > targetBytes)
                {
                    flushInsert();
                    const std::string valueTarget = targetExpr + "[" + keyExpr + "]";
                    appendMetadataStatement(statements, "    " + valueTarget + ".clear();");
                    appendStringChunks(value, valueTarget);
                    continue;
                }

                if (hasItems &&
                    current.size() + 2 + rendered.size() + insertSuffix.size() + kMetadataStatementTerminatorBytes > targetBytes)
                {
                    flushInsert();
                }

                if (hasItems)
                {
                    current.append(", ");
                }
                current.append(rendered);
                hasItems = true;
            }

            flushInsert();
        }

        void emitGroupedIntKeyStringValueMetadataStatements(std::vector<std::string> &statements,
                                                            std::string_view target,
                                                            const std::map<int64_t, std::string> &entries,
                                                            std::size_t targetBytes,
                                                            std::string_view groupNamePrefix)
        {
            const std::string targetExpr(target);
            appendMetadataStatement(statements, "    " + targetExpr + ".clear();");

            std::map<std::string, std::vector<int64_t>> idsByValue;
            for (const auto &[key, value] : entries)
            {
                idsByValue[value].push_back(key);
            }

            std::size_t groupIndex = 0;
            for (const auto &[value, ids] : idsByValue)
            {
                const std::string valueExpr = cppStringLiteral(value);
                const std::string arrayName = sanitizeIdentifier(std::string(groupNamePrefix)) + "_" + std::to_string(groupIndex++);
                const std::string blockPrefix =
                    "    {\n"
                    "        static constexpr std::int64_t " +
                    arrayName + "[] = {";
                const std::string blockSuffix =
                    "};\n"
                    "        for (const auto id : " +
                    arrayName + ") {\n"
                    "            " +
                    targetExpr + ".emplace(id, " + valueExpr + ");\n"
                    "        }\n"
                    "    }";

                std::string current(blockPrefix);
                bool hasItems = false;
                auto flushBlock = [&]() {
                    if (!hasItems)
                    {
                        return;
                    }
                    current.append(blockSuffix);
                    appendMetadataStatement(statements, current);
                    current = blockPrefix;
                    hasItems = false;
                };

                for (const auto id : ids)
                {
                    const std::string idExpr = std::to_string(id);
                    if (blockPrefix.size() + idExpr.size() + blockSuffix.size() + kMetadataStatementTerminatorBytes > targetBytes)
                    {
                        flushBlock();
                        appendMetadataStatement(statements, "    " + targetExpr + ".emplace(" + idExpr + ", " + valueExpr + ");");
                        continue;
                    }

                    if (hasItems &&
                        current.size() + 2 + idExpr.size() + blockSuffix.size() + kMetadataStatementTerminatorBytes >
                            targetBytes)
                    {
                        flushBlock();
                    }

                    if (hasItems)
                    {
                        current.append(", ");
                    }
                    current.append(idExpr);
                    hasItems = true;
                }

                flushBlock();
            }
        }

        std::vector<std::string> collectMetadataStatements(const GsimScratchpadMetadata &metadata,
                                                           std::size_t targetBytes)
        {
            std::vector<std::string> statements;
            emitIntVectorMetadataStatements(statements, "metadata.roots", metadata.roots, targetBytes);
            emitIntVectorMetadataStatements(statements, "metadata.topo_order", metadata.topoOrder, targetBytes);
            emitStringVectorMetadataStatements(statements, "metadata.event_group_names", metadata.eventGroupNames, targetBytes);
            emitVectorMapMetadataStatements(
                statements,
                "metadata.event_groups",
                metadata.eventGroups,
                targetBytes,
                [](const std::string &key) { return cppStringLiteral(key); });
            emitStringVectorMetadataStatements(
                statements,
                "metadata.schedule_activity_order",
                metadata.scheduleActivityOrder,
                targetBytes);
            emitVectorMapMetadataStatements(
                statements,
                "metadata.schedule_activity_members",
                metadata.scheduleActivityMembers,
                targetBytes,
                [](const std::string &key) { return cppStringLiteral(key); });
            emitStringValueMapMetadataStatements(
                statements,
                "metadata.schedule_activity_classes",
                metadata.scheduleActivityClasses,
                targetBytes,
                [](const std::string &key) { return cppStringLiteral(key); });
            emitStringVectorMetadataStatements(
                statements,
                "metadata.hypergraph_node_names",
                metadata.hypergraphNodeNames,
                targetBytes);
            emitVectorMapMetadataStatements(
                statements,
                "metadata.hypergraph_node_members",
                metadata.hypergraphNodeMembers,
                targetBytes,
                [](const std::string &key) { return cppStringLiteral(key); });
            emitStringVectorMetadataStatements(
                statements,
                "metadata.hypergraph_edge_names",
                metadata.hypergraphEdgeNames,
                targetBytes);
            emitStringValueMapMetadataStatements(
                statements,
                "metadata.hypergraph_edge_sources",
                metadata.hypergraphEdgeSources,
                targetBytes,
                [](const std::string &key) { return cppStringLiteral(key); });
            emitStringValueMapMetadataStatements(
                statements,
                "metadata.hypergraph_edge_targets",
                metadata.hypergraphEdgeTargets,
                targetBytes,
                [](const std::string &key) { return cppStringLiteral(key); });
            emitVectorMapMetadataStatements(
                statements,
                "metadata.hypergraph_edge_sinks",
                metadata.hypergraphEdgeSinks,
                targetBytes,
                [](const std::string &key) { return cppStringLiteral(key); });
            emitGroupedIntKeyStringValueMetadataStatements(
                statements,
                "metadata.classifications",
                metadata.classifications,
                targetBytes,
                "classification_ids");
            emitVectorMapMetadataStatements(
                statements,
                "metadata.predecessors",
                metadata.predecessors,
                targetBytes,
                [](int64_t key) { return std::to_string(key); });
            emitVectorMapMetadataStatements(
                statements,
                "metadata.successors",
                metadata.successors,
                targetBytes,
                [](int64_t key) { return std::to_string(key); });
            emitStringVectorMetadataStatements(statements, "metadata.op_descriptors", metadata.opDescriptors, targetBytes);
            return statements;
        }

        std::size_t estimateMetadataStatementsBytes(const std::vector<std::string> &statements)
        {
            std::size_t total = 0;
            for (const auto &statement : statements)
            {
                total += statement.size();
            }
            return total;
        }

        struct MetadataShardPlan
        {
            std::string filename;
            std::string functionName;
        };

        struct BehaviorShardPlan
        {
            std::string filename;
            std::string methodName;
        };

        constexpr std::size_t kCtorStorageInitShardMaxStatements = 128;

        std::size_t computeCtorStorageInitShardCount(const CodegenState &state,
                                                     const CodegenState &postState)
        {
            const std::size_t totalStatements = state.ctorStmts.size() + state.persistentTempGroups.size() +
                                                postState.persistentTempGroups.size();
            if (totalStatements == 0)
            {
                return 0;
            }
            if (!state.persistentTemps && !postState.persistentTemps)
            {
                return 0;
            }
            return std::max<std::size_t>(
                1, (totalStatements + kCtorStorageInitShardMaxStatements - 1) / kCtorStorageInitShardMaxStatements);
        }

        std::optional<std::vector<MetadataShardPlan>> planMetadataShards(const std::string &baseName,
                                                                         const std::vector<std::string> &statements,
                                                                         std::size_t maxBytes)
        {
            std::vector<MetadataShardPlan> plans;
            std::size_t currentBytes = 0;
            std::size_t shardIndex = 0;

            auto openNextPlan = [&]() {
                std::ostringstream indexText;
                indexText << std::setw(3) << std::setfill('0') << shardIndex++;
                const std::string suffix = indexText.str();
                plans.push_back(MetadataShardPlan{
                    baseName + "__meta_" + suffix + ".cpp",
                    "populate_" + baseName + "_metadata_shard_" + suffix,
                });
                currentBytes = 0;
            };

            for (const auto &statement : statements)
            {
                if (statement.size() > maxBytes)
                {
                    return std::nullopt;
                }
                if (plans.empty())
                {
                    openNextPlan();
                }
                if (currentBytes != 0 && currentBytes + statement.size() > maxBytes)
                {
                    openNextPlan();
                }
                currentBytes += statement.size();
            }
            return plans;
        }

        std::vector<std::string> collectStepStatements(const CodegenState &state)
        {
            std::vector<std::string> statements;
            statements.reserve(state.combinationalStmts.size());
            statements.insert(statements.end(), state.combinationalStmts.begin(), state.combinationalStmts.end());
            for (const auto &[domain, domainStatements] : state.sequentialStmts)
            {
                (void)domain;
                statements.insert(statements.end(), domainStatements.begin(), domainStatements.end());
            }
            return statements;
        }

        constexpr std::size_t statementEmitBytes(std::string_view statement)
        {
            return statement.size() + 1;
        }

        constexpr std::size_t behaviorShardEpilogueBytes()
        {
            return sizeof("}\n") - 1;
        }

        std::size_t dpiForwardDeclBlockBytes(const CodegenState &state)
        {
            if (state.dpiForwardDecls.empty())
            {
                return 0;
            }

            std::size_t total = sizeof("extern \"C\" {\n") - 1 + sizeof("}\n\n") - 1;
            for (const auto &decl : state.dpiForwardDecls)
            {
                total += decl.size() + 1;
            }
            return total;
        }

        void writeLocalDpiForwardDecls(std::ostream &os, const CodegenState &state)
        {
            if (state.dpiForwardDecls.empty())
            {
                return;
            }

            os << "extern \"C\" {\n";
            for (const auto &decl : state.dpiForwardDecls)
            {
                os << decl << "\n";
            }
            os << "}\n\n";
        }

        std::size_t behaviorShardPreambleBytes(std::string_view headerFilename,
                                               const CodegenState &state,
                                               std::string_view methodName)
        {
            return std::string("#include \"").size() + headerFilename.size() +
                   std::string("\"\n\n").size() +
                   dpiForwardDeclBlockBytes(state) +
                   std::string("void SSimTop::").size() + methodName.size() +
                   std::string("() {\n").size();
        }

        std::size_t behaviorShardWrapperBytes(std::string_view headerFilename,
                                              const CodegenState &state,
                                              std::string_view methodName)
        {
            return behaviorShardPreambleBytes(headerFilename, state, methodName) + behaviorShardEpilogueBytes();
        }

        std::size_t estimateBehaviorStatementBytes(const std::vector<std::string> &statements)
        {
            std::size_t total = 0;
            for (const auto &statement : statements)
            {
                total += statementEmitBytes(statement);
            }
            return total;
        }

        std::optional<std::vector<BehaviorShardPlan>> planBehaviorShards(const std::string &baseName,
                                                                         std::string_view fileTag,
                                                                         std::string_view methodPrefix,
                                                                         std::string_view headerFilename,
                                                                         const CodegenState &state,
                                                                         const std::vector<std::string> &statements,
                                                                         std::size_t maxBytes)
        {
            std::vector<BehaviorShardPlan> plans;
            std::size_t currentBytes = 0;
            std::size_t shardIndex = 0;

            auto openNextPlan = [&]() -> bool {
                std::ostringstream indexText;
                indexText << std::setw(3) << std::setfill('0') << shardIndex++;
                const std::string suffix = indexText.str();
                plans.push_back(BehaviorShardPlan{
                    baseName + "__" + std::string(fileTag) + "_" + suffix + ".cpp",
                    std::string(methodPrefix) + suffix,
                });
                currentBytes = behaviorShardWrapperBytes(headerFilename, state, plans.back().methodName);
                return currentBytes <= maxBytes;
            };

            for (const auto &statement : statements)
            {
                const auto bytes = statementEmitBytes(statement);
                if (plans.empty())
                {
                    if (!openNextPlan())
                    {
                        return std::nullopt;
                    }
                }
                if (currentBytes + bytes > maxBytes)
                {
                    if (!openNextPlan())
                    {
                        return std::nullopt;
                    }
                    if (currentBytes + bytes > maxBytes)
                    {
                        return std::nullopt;
                    }
                }
                currentBytes += bytes;
            }
            return plans;
        }

        std::unordered_set<std::string> readManagedSourceManifest(const std::filesystem::path &manifestPath)
        {
            std::unordered_set<std::string> files;
            std::ifstream manifest(manifestPath);
            std::string line;
            while (std::getline(manifest, line))
            {
                if (!line.empty())
                {
                    files.insert(line);
                }
            }
            return files;
        }

        bool writeManagedSourceManifest(const std::filesystem::path &manifestPath,
                                        const std::vector<std::string> &managedFiles,
                                        EmitDiagnostics *diagnostics)
        {
            std::ofstream manifest(manifestPath, std::ios::trunc);
            if (!manifest)
            {
                if (diagnostics != nullptr)
                {
                    diagnostics->error("failed to write gsim source manifest", manifestPath.string());
                }
                return false;
            }
            for (const auto &name : managedFiles)
            {
                manifest << name << "\n";
            }
            return true;
        }

        bool removeStaleManagedSources(const std::filesystem::path &outputDir,
                                       const std::unordered_set<std::string> &previousManagedFiles,
                                       const std::unordered_set<std::string> &managedFiles,
                                       EmitDiagnostics *diagnostics)
        {
            for (const auto &name : previousManagedFiles)
            {
                if (managedFiles.find(name) != managedFiles.end())
                {
                    continue;
                }
                const auto stalePath = outputDir / name;
                std::error_code removeEc;
                std::filesystem::remove(stalePath, removeEc);
                if (removeEc)
                {
                    if (diagnostics != nullptr)
                    {
                        diagnostics->error("failed to remove stale gsim source artifact", stalePath.string());
                    }
                    return false;
                }
            }
            return true;
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
            if (metadata.scheduleVersion != 1 || metadata.hypergraphVersion != 1)
            {
                reportError("gsim scratchpad metadata version mismatch",
                            scratchPrefix + " requires schedule.version=1 and hypergraph.version=1, got " +
                            std::to_string(metadata.scheduleVersion) + " and " +
                            std::to_string(metadata.hypergraphVersion));
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
                         const CodegenState& state,
                         const CodegenState& postState,
                         const std::vector<BehaviorShardPlan> &behaviorShardPlans,
                         const std::vector<BehaviorShardPlan> &postBehaviorShardPlans,
                         bool emitMetadata)
        {
            const std::string ns = sanitizeIdentifier(target.scratchGraphSymbol);
            const std::string structName = "GsimMetadata_" + ns;
            auto resetPortIt = std::find_if(state.inputPorts.begin(), state.inputPorts.end(),
                                            [](const auto &entry)
                                            {
                                                return sanitizeIdentifier(entry.first) == "reset";
                                            });
            os << "#pragma once\n\n";
            os << "#include <algorithm>\n";
            os << "#include <array>\n";
            os << "#include <cstddef>\n";
            os << "#include <cstdint>\n";
            os << "#include <map>\n";
            os << "#include <stdexcept>\n";
            os << "#include <string>\n";
            os << "#include <vector>\n\n";
            os << renderWideBitsSupport();
            os << "class SSimTop {\n";
            os << "public:\n";
            os << "    SSimTop();\n";
            os << "    ~SSimTop() = default;\n\n";
            if (resetPortIt != state.inputPorts.end())
            {
                os << "    void set_reset(unsigned reset) { input_reset_ = static_cast<" << resetPortIt->second
                   << ">(reset); }\n\n";
            }
            else
            {
                os << "    void set_reset(unsigned reset) { bootstrap_reset_pending_ = (reset != 0); }\n\n";
            }
            os << "    void reset();\n\n";
            os << "    void step();\n\n";

            // Input port setters
            for (const auto& [name, type] : state.inputPorts) {
                std::string methodName = "set_" + sanitizeIdentifier(name);
                if (methodName == "set_reset") {
                    continue;
                }
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
            os << "    bool bootstrap_reset_pending_ = false;\n";
            for (const auto &plan : behaviorShardPlans) {
                os << "    void " << plan.methodName << "();\n";
            }
            for (const auto &plan : postBehaviorShardPlans) {
                os << "    void " << plan.methodName << "();\n";
            }
            const auto ctorStorageInitShardCount = computeCtorStorageInitShardCount(state, postState);
            for (std::size_t shardIndex = 0; shardIndex < ctorStorageInitShardCount; ++shardIndex) {
                os << "    void init_ctor_storage_shard_" << shardIndex << "();\n";
            }
            if (!behaviorShardPlans.empty() || !postBehaviorShardPlans.empty()) {
                os << "\n";
            }

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
            if (state.persistentTemps && !state.persistentTempGroups.empty()) {
                for (const auto &group : state.persistentTempGroups) {
                    os << "    std::vector<" << group.cppType << "> " << group.storageName << ";\n";
                }
            }
            if (postState.persistentTemps && !postState.persistentTempGroups.empty()) {
                for (const auto &group : postState.persistentTempGroups) {
                    os << "    std::vector<" << group.cppType << "> " << group.storageName << ";\n";
                }
            }

            // Difftest state
            os << "    std::uint64_t difftest_exit_ = 0;\n";
            os << "    std::uint64_t difftest_step_ = 0;\n";
            os << "    unsigned perf_clean_ = 0;\n";
            os << "    unsigned perf_dump_ = 0;\n";
            os << "    std::uint64_t log_begin_ = 0;\n";
            os << "    std::uint64_t log_end_ = 0;\n";
            os << "};\n";
            if (emitMetadata)
            {
                os << "\nnamespace wolvrix::gsim {\n\n";
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
            }
            (void)metadata;
        }

        bool writeSource(std::ostream &os,
                         const std::filesystem::path &outputDir,
                         std::string_view baseName,
                         const EmitTarget &target,
                         const GsimScratchpadMetadata &metadata,
                         const CodegenState &state,
                         const CodegenState &postState,
                         std::string_view headerFilename,
                         std::size_t behaviorShardMaxBytes,
                         const std::vector<BehaviorShardPlan> &behaviorShardPlans,
                         const std::vector<BehaviorShardPlan> &postBehaviorShardPlans,
                         std::size_t metadataShardMaxBytes,
                         bool emitMetadata,
                         std::vector<std::string> &managedSourceFiles,
                         std::vector<std::string> &artifactPaths,
                         EmitDiagnostics *diagnostics)
        {
            const std::string ns = sanitizeIdentifier(target.scratchGraphSymbol);
            const std::string structName = "GsimMetadata_" + ns;
            const std::string factoryName = "make_" + sanitizeIdentifier(target.scratchGraphSymbol) + "_metadata";
            const std::string validateName = "validate_" + sanitizeIdentifier(target.scratchGraphSymbol) + "_metadata";
            const auto manifestPath = outputDir / (std::string(baseName) + ".manifest");
            const auto previousManagedFiles = readManagedSourceManifest(manifestPath);
            std::vector<std::string> metadataStatements;
            std::size_t metadataBytes = 0;
            if (emitMetadata)
            {
                const auto metadataStatementTargetBytes = chooseMetadataStatementTargetBytes(metadataShardMaxBytes);
                metadataStatements = collectMetadataStatements(metadata, metadataStatementTargetBytes);
                metadataBytes = estimateMetadataStatementsBytes(metadataStatements);
            }
            const auto stepStatements = collectStepStatements(state);
            const auto postStepStatements = collectStepStatements(postState);
            std::vector<MetadataShardPlan> shardPlans;
            if (emitMetadata)
            {
                const bool shardMetadata = metadataBytes > metadataShardMaxBytes;
                const auto shardPlansResult =
                    shardMetadata ? planMetadataShards(std::string(baseName), metadataStatements, metadataShardMaxBytes)
                                  : std::optional<std::vector<MetadataShardPlan>>(std::vector<MetadataShardPlan>{});
                if (!shardPlansResult)
                {
                    if (diagnostics != nullptr)
                    {
                        diagnostics->error("metadata_shard_max_bytes is too small to fit an emitted metadata statement",
                                           std::to_string(metadataShardMaxBytes));
                    }
                    return false;
                }
                shardPlans = *shardPlansResult;
            }

            os << "#include \"" << headerFilename << "\"\n\n";
            writeLocalDpiForwardDecls(os, state);
            os << "#include <algorithm>\n";
            os << "#include <set>\n\n";
            std::vector<std::string> ctorInitStatements;
            ctorInitStatements.reserve(state.ctorStmts.size() + state.persistentTempGroups.size() +
                                       postState.persistentTempGroups.size());
            ctorInitStatements.insert(ctorInitStatements.end(), state.ctorStmts.begin(), state.ctorStmts.end());
            for (const auto &group : state.persistentTempGroups)
            {
                ctorInitStatements.push_back("    " + group.storageName + ".resize(" + std::to_string(group.count) + ");");
            }
            for (const auto &group : postState.persistentTempGroups)
            {
                ctorInitStatements.push_back("    " + group.storageName + ".resize(" + std::to_string(group.count) + ");");
            }
            const auto ctorStorageInitShardCount = computeCtorStorageInitShardCount(state, postState);
            if (ctorStorageInitShardCount != 0)
            {
                for (std::size_t shardIndex = 0; shardIndex < ctorStorageInitShardCount; ++shardIndex)
                {
                    const std::size_t begin = shardIndex * kCtorStorageInitShardMaxStatements;
                    const std::size_t end =
                        std::min(begin + kCtorStorageInitShardMaxStatements, ctorInitStatements.size());
                    os << "void SSimTop::init_ctor_storage_shard_" << shardIndex << "() {\n";
                    for (std::size_t stmtIndex = begin; stmtIndex < end; ++stmtIndex)
                    {
                        os << ctorInitStatements[stmtIndex] << "\n";
                    }
                    os << "}\n\n";
                }
            }
            os << "SSimTop::SSimTop() {\n";
            if (ctorStorageInitShardCount == 0)
            {
                for (const auto &stmt : ctorInitStatements)
                {
                    os << stmt << "\n";
                }
            }
            else
            {
                for (std::size_t shardIndex = 0; shardIndex < ctorStorageInitShardCount; ++shardIndex)
                {
                    os << "    init_ctor_storage_shard_" << shardIndex << "();\n";
                }
            }
            os << "    reset();\n";
            os << "}\n\n";
            os << "void SSimTop::reset() {\n";
            os << "    bootstrap_reset_pending_ = true;\n";
            for (const auto &stmt : state.resetStmts)
            {
                os << stmt << "\n";
            }
            for (const auto &[name, type] : state.outputPorts)
            {
                (void)type;
                os << "    output_" << sanitizeIdentifier(name) << "_ = 0;\n";
            }
            os << "}\n\n";
            os << "void SSimTop::step() {\n";
            os << "    ++difftest_step_;\n";
            os << "    if (bootstrap_reset_pending_) {\n";
            os << "        bootstrap_reset_pending_ = false;\n";
            os << "        difftest_exit_ = 0;\n";
            os << "    }\n";
            if (state.hasResetInput)
            {
                os << "    if (input_reset_) {\n";
                for (const auto &stmt : state.resetStmts)
                {
                    os << stmt << "\n";
                }
                os << "    }\n";
            }
            if (behaviorShardPlans.empty())
            {
                for (const auto &stmt : stepStatements)
                {
                    os << stmt << "\n";
                }
            }
            else
            {
                for (const auto &plan : behaviorShardPlans)
                {
                    os << "    " << plan.methodName << "();\n";
                }
            }
            for (const auto &stmt : state.outputStmts)
            {
                os << stmt << "\n";
            }
            if (postBehaviorShardPlans.empty())
            {
                if (!postStepStatements.empty() || !postState.outputStmts.empty())
                {
                    os << "    {\n";
                    for (const auto &stmt : postStepStatements)
                    {
                        os << stmt << "\n";
                    }
                    for (const auto &stmt : postState.outputStmts)
                    {
                        os << stmt << "\n";
                    }
                    os << "    }\n";
                }
            }
            else
            {
                for (const auto &plan : postBehaviorShardPlans)
                {
                    os << "    " << plan.methodName << "();\n";
                }
                for (const auto &stmt : postState.outputStmts)
                {
                    os << stmt << "\n";
                }
            }
            os << "    difftest_exit_ = 0;\n";
            os << "}\n\n";
            if (emitMetadata)
            {
                os << "namespace wolvrix::gsim {\n\n";
                os << "namespace {\n";
                os << "template <typename T>\n";
                os << "bool has_duplicate_values(const std::vector<T>& values) {\n";
                os << "    return std::set<T>(values.begin(), values.end()).size() != values.size();\n";
                os << "}\n";
                os << "} // namespace\n\n";
                for (const auto &plan : shardPlans)
                {
                    os << "void " << plan.functionName << "(" << structName << "& metadata);\n";
                }
                if (!shardPlans.empty())
                {
                    os << "\n";
                }
                os << structName << " " << factoryName << "() {\n";
                os << "    " << structName << " metadata;\n";
                os << "    metadata.graph_symbol = \"" << target.scratchGraphSymbol << "\";\n";
                os << "    metadata.selection_path = \"" << target.selectionPath << "\";\n";
                os << "    metadata.scratchpad_namespace = \"" << target.namespacePath << "\";\n";
                os << "    metadata.op_count = " << metadata.opCount << ";\n";
                os << "    metadata.graph_revision = " << metadata.graphRevision << ";\n";
                os << "    metadata.schedule_kind = \"" << metadata.scheduleKind << "\";\n";
                os << "    metadata.schedule_version = " << metadata.scheduleVersion << ";\n";
                os << "    metadata.schedule_contract = \"" << metadata.scheduleContract << "\";\n";
                os << "    metadata.hypergraph_kind = \"" << metadata.hypergraphKind << "\";\n";
                os << "    metadata.hypergraph_version = " << metadata.hypergraphVersion << ";\n";
                os << "    metadata.hypergraph_contract = \"" << metadata.hypergraphContract << "\";\n";
                if (shardPlans.empty())
                {
                    for (const auto &statement : metadataStatements)
                    {
                        os << statement;
                    }
                }
                else
                {
                    for (const auto &plan : shardPlans)
                    {
                        os << "    " << plan.functionName << "(metadata);\n";
                    }
                }
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
                os << "    if (metadata.schedule_version != 1 || metadata.hypergraph_version != 1) return false;\n";
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

            managedSourceFiles.push_back(std::string(baseName) + ".cpp");
            artifactPaths.push_back((outputDir / (std::string(baseName) + ".cpp")).string());

            auto writeBehaviorShards = [&](const CodegenState &shardState,
                                           const std::vector<BehaviorShardPlan> &plans,
                                           const std::vector<std::string> &statements) -> bool {
                if (plans.empty())
                {
                    return true;
                }

                std::size_t shardIndex = 0;
                std::size_t shardBytes = 0;
                std::ofstream shardStream;

                auto finishShard = [&]() -> bool {
                    if (!shardStream.is_open())
                    {
                        return true;
                    }
                    shardStream << "}\n";
                    if (!shardStream.good())
                    {
                        if (diagnostics != nullptr)
                        {
                            diagnostics->error("failed to finalize gsim behavior shard",
                                               (outputDir / plans[shardIndex].filename).string());
                        }
                        return false;
                    }
                    shardStream.close();
                    ++shardIndex;
                    shardBytes = 0;
                    return true;
                };

                auto startShard = [&]() -> bool {
                    const auto &plan = plans[shardIndex];
                    shardStream = std::ofstream(outputDir / plan.filename, std::ios::trunc);
                    if (!shardStream)
                    {
                        if (diagnostics != nullptr)
                        {
                            diagnostics->error("failed to open gsim behavior shard for writing",
                                               (outputDir / plan.filename).string());
                        }
                        return false;
                    }
                    shardStream << "#include \"" << headerFilename << "\"\n\n";
                    writeLocalDpiForwardDecls(shardStream, shardState);
                    shardStream << "void SSimTop::" << plan.methodName << "() {\n";
                    managedSourceFiles.push_back(plan.filename);
                    artifactPaths.push_back((outputDir / plan.filename).string());
                    shardBytes = behaviorShardWrapperBytes(headerFilename, shardState, plan.methodName);
                    return true;
                };

                if (!startShard())
                {
                    return false;
                }

                for (const auto &statement : statements)
                {
                    const auto bytes = statementEmitBytes(statement);
                    if (shardBytes + bytes > behaviorShardMaxBytes)
                    {
                        if (!finishShard())
                        {
                            return false;
                        }
                        if (shardIndex >= plans.size() || !startShard())
                        {
                            return false;
                        }
                        if (shardBytes + bytes > behaviorShardMaxBytes)
                        {
                            if (diagnostics != nullptr)
                            {
                                diagnostics->error("behavior_shard_max_bytes is too small to fit an emitted behavior statement",
                                                   std::to_string(behaviorShardMaxBytes));
                            }
                            return false;
                        }
                    }

                    shardStream << statement << "\n";
                    if (!shardStream.good())
                    {
                        if (diagnostics != nullptr)
                        {
                            diagnostics->error("failed to write gsim behavior shard",
                                               (outputDir / plans[shardIndex].filename).string());
                        }
                        return false;
                    }
                    shardBytes += bytes;
                }

                return finishShard();
            };

            if (!writeBehaviorShards(state, behaviorShardPlans, stepStatements))
            {
                return false;
            }
            if (!writeBehaviorShards(postState, postBehaviorShardPlans, postStepStatements))
            {
                return false;
            }

            if (emitMetadata && !shardPlans.empty())
            {
                std::size_t shardIndex = 0;
                std::size_t shardBytes = 0;
                std::ofstream shardStream;

                auto finishShard = [&]() -> bool {
                    if (!shardStream.is_open())
                    {
                        return true;
                    }
                    shardStream << "}\n\n} // namespace wolvrix::gsim\n";
                    if (!shardStream.good())
                    {
                        if (diagnostics != nullptr)
                        {
                            diagnostics->error("failed to finalize gsim metadata shard",
                                               (outputDir / shardPlans[shardIndex].filename).string());
                        }
                        return false;
                    }
                    shardStream.close();
                    ++shardIndex;
                    shardBytes = 0;
                    return true;
                };

                auto startShard = [&]() -> bool {
                    const auto &plan = shardPlans[shardIndex];
                    shardStream = std::ofstream(outputDir / plan.filename, std::ios::trunc);
                    if (!shardStream)
                    {
                        if (diagnostics != nullptr)
                        {
                            diagnostics->error("failed to open gsim metadata shard for writing",
                                               (outputDir / plan.filename).string());
                        }
                        return false;
                    }
                    shardStream << "#include \"" << headerFilename << "\"\n\n";
                    shardStream << "namespace wolvrix::gsim {\n\n";
                    shardStream << "void " << plan.functionName << "(" << structName << "& metadata) {\n";
                    managedSourceFiles.push_back(plan.filename);
                    artifactPaths.push_back((outputDir / plan.filename).string());
                    shardBytes = 0;
                    return true;
                };

                if (!startShard())
                {
                    return false;
                }

                bool shardWriteOk = true;
                for (const auto &statement : metadataStatements)
                {
                    if (!shardWriteOk)
                    {
                        break;
                    }
                    if (statement.size() > metadataShardMaxBytes)
                    {
                        if (diagnostics != nullptr)
                        {
                            diagnostics->error("metadata_shard_max_bytes is too small to fit an emitted metadata statement",
                                               std::to_string(metadataShardMaxBytes));
                        }
                        shardWriteOk = false;
                        break;
                    }
                    if (shardBytes != 0 && shardBytes + statement.size() > metadataShardMaxBytes)
                    {
                        shardWriteOk = finishShard();
                        if (!shardWriteOk)
                        {
                            break;
                        }
                        if (shardIndex >= shardPlans.size() || !startShard())
                        {
                            shardWriteOk = false;
                            break;
                        }
                    }
                    shardStream << statement;
                    if (!shardStream.good())
                    {
                        if (diagnostics != nullptr)
                        {
                            diagnostics->error("failed to write gsim metadata shard",
                                               (outputDir / shardPlans[shardIndex].filename).string());
                        }
                        shardWriteOk = false;
                        break;
                    }
                    shardBytes += statement.size();
                }

                if (!shardWriteOk || !finishShard())
                {
                    return false;
                }
            }

            const std::unordered_set<std::string> currentManagedFiles(managedSourceFiles.begin(), managedSourceFiles.end());
            if (!removeStaleManagedSources(outputDir, previousManagedFiles, currentManagedFiles, diagnostics))
            {
                return false;
            }
            if (!writeManagedSourceManifest(manifestPath, managedSourceFiles, diagnostics))
            {
                return false;
            }
            artifactPaths.push_back(manifestPath.string());
            return true;
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

        std::vector<wolvrix::lib::grh::OperationId> operationIdsByIndex;
        for (const auto opId : target->graph->operations())
        {
            if (opId.index >= operationIdsByIndex.size())
            {
                operationIdsByIndex.resize(static_cast<std::size_t>(opId.index) + 1);
            }
            operationIdsByIndex[opId.index] = opId;
        }

        std::unordered_set<int64_t> postSequentialSideEffectOpIndices;
        std::unordered_set<int64_t> postSequentialSliceOpIndices;
        std::vector<int64_t> postSequentialWorklist;
        std::unordered_map<int32_t, int64_t> producerOpIndices;
        for (int64_t opIdx : metadata->topoOrder)
        {
            if (opIdx < 0 || static_cast<std::size_t>(opIdx) >= operationIdsByIndex.size())
            {
                continue;
            }
            const auto opId = operationIdsByIndex[static_cast<std::size_t>(opIdx)];
            if (!opId.valid())
            {
                continue;
            }
            const auto op = target->graph->getOperation(opId);
            for (const auto valueId : op.results())
            {
                producerOpIndices[valueId.index] = opIdx;
            }
            if (!isPostSequentialSideEffectDpiCall(op))
            {
                continue;
            }
            postSequentialSideEffectOpIndices.insert(opIdx);
            if (postSequentialSliceOpIndices.insert(opIdx).second)
            {
                postSequentialWorklist.push_back(opIdx);
            }
        }
        while (!postSequentialWorklist.empty())
        {
            const auto current = postSequentialWorklist.back();
            postSequentialWorklist.pop_back();
            const auto predIt = metadata->predecessors.find(current);
            if (predIt == metadata->predecessors.end())
            {
                continue;
            }
            for (const auto predecessor : predIt->second)
            {
                if (predecessor < 0)
                {
                    continue;
                }
                if (postSequentialSliceOpIndices.insert(predecessor).second)
                {
                    postSequentialWorklist.push_back(predecessor);
                }
            }
        }

        for (const auto &port : target->graph->outputPorts())
        {
            const auto producerIt = producerOpIndices.find(port.value.index);
            if (producerIt == producerOpIndices.end())
            {
                continue;
            }

            std::unordered_set<int64_t> outputSlice;
            std::vector<int64_t> outputWorklist{producerIt->second};
            bool dependsOnStatefulRead = false;
            while (!outputWorklist.empty())
            {
                const auto current = outputWorklist.back();
                outputWorklist.pop_back();
                if (!outputSlice.insert(current).second)
                {
                    continue;
                }
                if (current < 0 || static_cast<std::size_t>(current) >= operationIdsByIndex.size())
                {
                    continue;
                }
                const auto opId = operationIdsByIndex[static_cast<std::size_t>(current)];
                if (!opId.valid())
                {
                    continue;
                }
                const auto op = target->graph->getOperation(opId);
                if (isStatefulReadOp(op))
                {
                    dependsOnStatefulRead = true;
                }
                const auto predIt = metadata->predecessors.find(current);
                if (predIt == metadata->predecessors.end())
                {
                    continue;
                }
                for (const auto predecessor : predIt->second)
                {
                    if (predecessor >= 0)
                    {
                        outputWorklist.push_back(predecessor);
                    }
                }
            }

            if (!dependsOnStatefulRead)
            {
                continue;
            }
            postSequentialSliceOpIndices.insert(outputSlice.begin(), outputSlice.end());
        }

        auto buildCodegenState = [&](bool persistentTemps,
                                     const std::unordered_set<int64_t> *includedOpIndices,
                                     const std::unordered_set<int64_t> *excludedOpIndices,
                                     bool emitOutputs,
                                     std::string_view persistentTempPrefix) {
            CodegenState state;
            state.persistentTemps = persistentTemps;
            state.persistentTempPrefix = std::string(persistentTempPrefix);
            collectPorts(*target->graph, state, options.portOrderStrategy, options.portOrderNames);
            collectRegisters(*target->graph, state);
            collectMemories(*target->graph, state);
            collectDpiImports(*target->graph, state);

            for (int64_t opIdx : metadata->topoOrder) {
                if (opIdx < 0 || static_cast<std::size_t>(opIdx) >= operationIdsByIndex.size()) {
                    continue;
                }
                if (includedOpIndices != nullptr && includedOpIndices->count(opIdx) == 0) {
                    continue;
                }
                if (excludedOpIndices != nullptr && excludedOpIndices->count(opIdx) != 0) {
                    continue;
                }
                const auto opId = operationIdsByIndex[static_cast<std::size_t>(opIdx)];
                if (!opId.valid())
                {
                    continue;
                }
                auto op = target->graph->getOperation(opId);
                lowerOperation(*target->graph, op, state);
            }

            if (emitOutputs)
            {
                for (const auto& port : target->graph->outputPorts()) {
                    auto it = state.outputValueNames.find(port.value);
                    if (it == state.outputValueNames.end()) {
                        continue;
                    }
                    const std::string& sanitizedName = it->second;
                    auto exprIt = state.valueExprs.find(port.value);
                    if (exprIt != state.valueExprs.end()) {
                        state.outputStmts.push_back(
                            "        output_" + sanitizedName + "_ = " + exprIt->second + ";");
                    }
                }
            }

            return state;
        };

        auto reportUnsupportedOps = [&](std::vector<std::string> unsupportedOps) -> bool {
            if (unsupportedOps.empty()) {
                return false;
            }
            std::string msg = "unsupported operations encountered: ";
            for (size_t i = 0; i < unsupportedOps.size() && i < 5; ++i) {
                if (i > 0) msg += ", ";
                msg += unsupportedOps[i];
            }
            if (unsupportedOps.size() > 5) {
                msg += " and " + std::to_string(unsupportedOps.size() - 5) + " more";
            }
            reportError(msg, target->graph->symbol());
            result.success = false;
            return true;
        };

        CodegenState state = buildCodegenState(false,
                                               nullptr,
                                               &postSequentialSideEffectOpIndices,
                                               true,
                                               "step_tmp_group_");
        CodegenState postState = buildCodegenState(false,
                                                   &postSequentialSliceOpIndices,
                                                   nullptr,
                                                   true,
                                                   "post_step_tmp_group_");

        // Validate custom port order names
        if (options.portOrderStrategy == PortOrderStrategy::Custom && !options.portOrderNames.empty()) {
            std::set<std::string> allPortNames;
            for (const auto& [name, type] : state.inputPorts) allPortNames.insert(name);
            for (const auto& [name, type] : state.outputPorts) allPortNames.insert(name);
            std::set<std::string> seenNames;
            for (const auto& name : options.portOrderNames) {
                if (allPortNames.find(name) == allPortNames.end()) {
                    reportError("port_order_names contains nonexistent port", name);
                    result.success = false;
                    return result;
                }
                if (!seenNames.insert(name).second) {
                    reportError("port_order_names contains duplicate port", name);
                    result.success = false;
                    return result;
                }
            }
        }

        std::vector<std::string> unsupportedOps = state.unsupportedOps;
        unsupportedOps.insert(unsupportedOps.end(), postState.unsupportedOps.begin(), postState.unsupportedOps.end());
        if (reportUnsupportedOps(std::move(unsupportedOps))) {
            return result;
        }

        const std::filesystem::path outputDir = resolveOutputDir(options);
        const std::string baseName = options.outputFilename && !options.outputFilename->empty()
                                         ? sanitizeIdentifier(std::filesystem::path(*options.outputFilename).stem().string())
                                         : defaultBaseName(*target);
        const std::filesystem::path headerPath = outputDir / (baseName + ".hpp");
        const std::filesystem::path sourcePath = outputDir / (baseName + ".cpp");
        const auto headerFilename = headerPath.filename().string();
        const bool emitMetadata = boolAttrValue(options, "emit_metadata", true);
        const auto behaviorShardMaxBytes = parseBehaviorShardMaxBytes(options, diagnostics());
        const auto metadataShardMaxBytes = parseMetadataShardMaxBytes(options, diagnostics());
        if (!behaviorShardMaxBytes || !metadataShardMaxBytes)
        {
            result.success = false;
            return result;
        }
        auto behaviorStatements = collectStepStatements(state);
        auto postBehaviorStatements = collectStepStatements(postState);
        if (estimateBehaviorStatementBytes(behaviorStatements) > *behaviorShardMaxBytes)
        {
            state = buildCodegenState(true,
                                      nullptr,
                                      &postSequentialSideEffectOpIndices,
                                      true,
                                      "step_tmp_group_");
            std::vector<std::string> rebuiltUnsupportedOps = state.unsupportedOps;
            rebuiltUnsupportedOps.insert(rebuiltUnsupportedOps.end(), postState.unsupportedOps.begin(), postState.unsupportedOps.end());
            if (reportUnsupportedOps(std::move(rebuiltUnsupportedOps))) {
                return result;
            }
            behaviorStatements = collectStepStatements(state);
        }
        if (estimateBehaviorStatementBytes(postBehaviorStatements) > *behaviorShardMaxBytes)
        {
            postState = buildCodegenState(true,
                                          &postSequentialSliceOpIndices,
                                          nullptr,
                                          true,
                                          "post_step_tmp_group_");
            std::vector<std::string> rebuiltUnsupportedOps = state.unsupportedOps;
            rebuiltUnsupportedOps.insert(rebuiltUnsupportedOps.end(), postState.unsupportedOps.begin(), postState.unsupportedOps.end());
            if (reportUnsupportedOps(std::move(rebuiltUnsupportedOps))) {
                return result;
            }
            postBehaviorStatements = collectStepStatements(postState);
        }
        std::vector<BehaviorShardPlan> behaviorShardPlans;
        if (estimateBehaviorStatementBytes(behaviorStatements) > *behaviorShardMaxBytes)
        {
            auto plannedBehaviorShards =
                planBehaviorShards(baseName, "step", "run_step_shard_", headerFilename, state, behaviorStatements, *behaviorShardMaxBytes);
            if (!plannedBehaviorShards)
            {
                reportError("behavior_shard_max_bytes is too small to fit an emitted behavior statement",
                            std::to_string(*behaviorShardMaxBytes));
                result.success = false;
                return result;
            }
            behaviorShardPlans = std::move(*plannedBehaviorShards);
        }
        std::vector<BehaviorShardPlan> postBehaviorShardPlans;
        if (estimateBehaviorStatementBytes(postBehaviorStatements) > *behaviorShardMaxBytes)
        {
            auto plannedBehaviorShards =
                planBehaviorShards(baseName, "post", "run_post_shard_", headerFilename, postState, postBehaviorStatements, *behaviorShardMaxBytes);
            if (!plannedBehaviorShards)
            {
                reportError("behavior_shard_max_bytes is too small to fit an emitted post-sequential behavior statement",
                            std::to_string(*behaviorShardMaxBytes));
                result.success = false;
                return result;
            }
            postBehaviorShardPlans = std::move(*plannedBehaviorShards);
        }

        auto header = openOutputFile(headerPath);
        auto source = openOutputFile(sourcePath);
        if (!header || !source)
        {
            result.success = false;
            return result;
        }

        writeHeader(*header, *target, *metadata, state, postState, behaviorShardPlans, postBehaviorShardPlans, emitMetadata);
        std::vector<std::string> managedSourceFiles;
        if (!writeSource(*source,
                         outputDir,
                         baseName,
                         *target,
                         *metadata,
                         state,
                         postState,
                         headerFilename,
                         *behaviorShardMaxBytes,
                         behaviorShardPlans,
                         postBehaviorShardPlans,
                         *metadataShardMaxBytes,
                         emitMetadata,
                         managedSourceFiles,
                         result.artifacts,
                         diagnostics()))
        {
            result.success = false;
            return result;
        }

        result.artifacts.push_back(headerPath.string());
        return result;
    }

} // namespace wolvrix::lib::emit
