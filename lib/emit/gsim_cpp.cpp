#include "emit/gsim_cpp.hpp"

#include "core/transform.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <iomanip>
#include <unordered_set>
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
        std::string zeroInitializerForType(std::string_view cppType);
        std::string zeroInitializerForWidth(int32_t width);

        struct MemoryInfo
        {
            std::string storageName;
            std::string rowType;
            std::string zeroExpr;
            std::int64_t rows = 0;
            std::int32_t width = 0;
        };

        struct DpicImportInfo
        {
            std::string symbol;
            std::vector<std::string> argNames;
            std::vector<std::string> argDirs;
            std::vector<int64_t> argWidths;
            bool hasReturn = false;
            int64_t returnWidth = 0;
            std::string returnType;
        };

        constexpr std::uint8_t kDirtyReplayClock = 1U << 0U;
        constexpr std::uint8_t kDirtyReplayNonClock = 1U << 1U;
        constexpr std::uint8_t kDirtyReplayAll = kDirtyReplayClock | kDirtyReplayNonClock;
        constexpr std::uint8_t kDirtyReplayDpicProducer = kDirtyReplayNonClock;

        // Code generation state for lowering GRH operations to C++
        struct CodegenState
        {
            // Value variables: maps ValueId to C++ variable name (materialized for sharding)
            std::unordered_map<wolvrix::lib::grh::ValueId, std::string, wolvrix::lib::grh::ValueIdHash> valueVars;
            std::unordered_map<wolvrix::lib::grh::ValueId, std::pair<std::string, std::string>, wolvrix::lib::grh::ValueIdHash> outputPortValues;

            // Persistent storage declarations that still need named fields (primarily memories).
            std::vector<std::string> storageDecls;
            std::vector<std::string> storageResetStmts;
            std::unordered_map<std::string, int32_t> storageWidths;
            std::unordered_map<std::string, std::string> persistentVars;
            std::size_t stateU8Count = 0;
            std::size_t stateU16Count = 0;
            std::size_t stateU32Count = 0;
            std::size_t stateU64Count = 0;
            std::vector<int32_t> stateVecWidths;
            // Sharded temporary storage pools: moved out of the public header into EvalTemps
            std::size_t tempU8Count = 0;
            std::size_t tempU16Count = 0;
            std::size_t tempU32Count = 0;
            std::size_t tempU64Count = 0;
            std::vector<int32_t> tempVecWidths;

            // Sequential update statements (grouped by clock domain)
            std::map<std::string, std::vector<std::string>> sequentialPreStmts;
            std::map<std::string, std::vector<std::string>> sequentialStmts;
            std::map<std::string, std::vector<bool>> sequentialStmtDirtyOnCommit;
            std::map<std::string, std::vector<std::vector<std::string>>> sequentialStmtActivitySources;
            std::map<std::string, std::map<std::string, std::vector<std::string>>> sequentialRegStmts;
            std::map<std::string, std::string> sequentialClockExprs;

            // Non-sharded combinational statements for small designs
            std::vector<std::string> combinationalStmts;
            std::vector<std::string> latchStmts;

            // Port declarations and accessors
            std::vector<std::pair<std::string, std::string>> inputPorts;  // (name, type)
            std::vector<std::pair<std::string, std::string>> outputPorts; // (name, type)
            std::unordered_map<std::string, int32_t> inputPortWidths;
            std::unordered_map<std::string, int32_t> outputPortWidths;

            // Track unsupported operations for error reporting
            std::vector<std::string> unsupportedOps;

            // DPI-C imports/calls used by terminal-capable XiangShan difftest.
            std::map<std::string, DpicImportInfo> dpicImports;
            bool emitsDpicCalls = false;
            bool emitsNoDiffGuardedDpicCalls = false;
            bool emitsRuntimeDpicCalls = false;
            bool enableDpicTrace = false;
            bool enableXsZeroRetireTrace = false;
            std::size_t dpicTraceCallSite = 0;
            std::size_t dpicMaterializedCallSite = 0;
            std::set<int> dpicPreSettleShards;
            int dpicGlobalWarmupSteps = 0;

            // Memory declarations/read lowering support
            std::unordered_map<std::string, MemoryInfo> memories;

            // Sharding support: generated text buffers for different shards.
            // Plain strings avoid ostringstream formatting overhead and the final
            // full-buffer str() copy when writing XiangShan-scale shard files.
            std::vector<std::string> shardBuffers;
            std::vector<std::uint8_t> shardDirtyReplayMask;
            int currentShard = 0;
            int maxShardSize = 2097152; // 2MB per behavior shard
            int commitShardSize = 786432; // keep sequential commit translation units small enough for low optimization levels
            int currentShardSize = 0;
            std::uint8_t currentOpDirtyReplayMask = 0;

            // Flag to determine if sharding should be enabled based on operation count
            bool enableSharding = false;
            // Optional coarse activity watermark. Disabled by default for fast codegen
            // and to avoid clocked-cycle runtime regressions until a finer supernode
            // scheduler is available.
            bool enableActivityWatermark = false;

            std::unordered_map<wolvrix::lib::grh::ValueId, std::uint8_t, wolvrix::lib::grh::ValueIdHash> dirtyReplayRootMasks;
            std::unordered_map<wolvrix::lib::grh::ValueId, std::uint8_t, wolvrix::lib::grh::ValueIdHash> valueDirtyReplayMasks;

            // Coarse activity tracking borrowed from grhsim's active-supernode model.
            // Memory-bounded XiangShan variant: remember the first sched shard touched by
            // each root source and replay a suffix from that shard. This avoids per-value
            // transitive source sets that are too expensive for multi-million-op graphs.
            std::unordered_map<wolvrix::lib::grh::ValueId, std::string, wolvrix::lib::grh::ValueIdHash> inputActivitySourceNames;
            std::unordered_map<wolvrix::lib::grh::ValueId, int, wolvrix::lib::grh::ValueIdHash> valueActivityFirstShard;
            std::unordered_map<wolvrix::lib::grh::ValueId, int, wolvrix::lib::grh::ValueIdHash> valueProducerShard;
            std::unordered_map<wolvrix::lib::grh::ValueId, std::int64_t, wolvrix::lib::grh::ValueIdHash> valueProducerOpIndex;
            std::vector<int> opProducerFirstShardByIndex;
            std::vector<int> opProducerLastShardByIndex;
            std::vector<std::int64_t> dirtyReplayProducerOpSeeds;
            std::vector<std::set<int>> shardSuccessors;
            std::vector<std::string> currentOpDirectActivitySources;
            int currentOpActivityFirstShard = -1;
            int currentOpFirstEmittedShard = -1;
            int lastEmittedShard = -1;
            std::unordered_map<std::string, int> activitySourceFirstShard;
            std::unordered_map<std::string, std::set<int>> activitySourceHeadShards;

            // Helper to get next available shard buffer.
            std::string* getCurrentShardBuffer() {
                if (!enableSharding) {
                    // For small designs, don't enable sharding.
                    if (shardBuffers.empty()) {
                        shardBuffers.emplace_back();
                    }
                    return &shardBuffers[0];
                }

                if (shardBuffers.empty() || currentShard >= static_cast<int>(shardBuffers.size())) {
                    shardBuffers.emplace_back();
                    shardBuffers.back().reserve(static_cast<std::size_t>(maxShardSize) + 1024U);
                    shardDirtyReplayMask.push_back(0);
                    shardSuccessors.emplace_back();
                    currentShard = static_cast<int>(shardBuffers.size()) - 1;
                    currentShardSize = 0;
                }
                return &shardBuffers[currentShard];
            }

            // Helper to create a new shard when current one gets too large
            void ensureShardSpace(int estimatedSize) {
                if (!enableSharding) {
                    return; // Don't shard for small designs
                }

                if (currentShardSize + estimatedSize > maxShardSize) {
                    currentShard++;
                    currentShardSize = 0;
                    if (currentShard >= static_cast<int>(shardBuffers.size())) {
                        shardBuffers.emplace_back();
                        shardBuffers.back().reserve(static_cast<std::size_t>(maxShardSize) + 1024U);
                        shardDirtyReplayMask.push_back(0);
                        shardSuccessors.emplace_back();
                    }
                }
            }

            // Set result for a value ID
            std::string materializeResultRef(const wolvrix::lib::grh::ValueId& valueId,
                                             const std::string& cppType,
                                             int32_t width) {
                if (auto it = valueVars.find(valueId); it != valueVars.end()) {
                    return it->second;
                }

                std::string ref;
                if (cppType == "std::uint8_t")
                {
                    ref = "evalTemps_->tempU8[" + std::to_string(tempU8Count++) + "]";
                }
                else if (cppType == "std::uint16_t")
                {
                    ref = "evalTemps_->tempU16[" + std::to_string(tempU16Count++) + "]";
                }
                else if (cppType == "std::uint32_t")
                {
                    ref = "evalTemps_->tempU32[" + std::to_string(tempU32Count++) + "]";
                }
                else if (cppType == "std::uint64_t")
                {
                    ref = "evalTemps_->tempU64[" + std::to_string(tempU64Count++) + "]";
                }
                else
                {
                    ref = "evalTemps_->tempVec[" + std::to_string(tempVecWidths.size()) + "]";
                    tempVecWidths.push_back(width);
                }
                auto [insertedIt, inserted] = valueVars.emplace(valueId, std::move(ref));
                (void)inserted;
                return insertedIt->second;
            }

            void markCurrentShardActivity() {
                if (currentShard >= 0 && currentShard < static_cast<int>(shardDirtyReplayMask.size()) &&
                    currentOpDirtyReplayMask != 0)
                {
                    shardDirtyReplayMask[static_cast<std::size_t>(currentShard)] |= currentOpDirtyReplayMask;
                }
                if (enableSharding && currentShard >= 0) {
                    if (currentOpFirstEmittedShard < 0) {
                        currentOpFirstEmittedShard = currentShard;
                    }
                    lastEmittedShard = currentShard;
                }
                if (enableActivityWatermark && currentShard >= 0) {
                    for (const auto& source : currentOpDirectActivitySources) {
                        auto [it, inserted] = activitySourceFirstShard.emplace(source, currentShard);
                        if (!inserted && currentShard < it->second) {
                            it->second = currentShard;
                        }
                        activitySourceHeadShards[source].insert(currentShard);
                    }
                }
            }

            void emitShardStatement(const std::string& stmt) {
                ensureShardSpace(static_cast<int>(stmt.length()));
                markCurrentShardActivity();
                std::string* shard = getCurrentShardBuffer();
                shard->append(stmt);
                shard->push_back('\n');
                currentShardSize += static_cast<int>(stmt.length());
            }

            void emitShardAssignment(const std::string& lhs, const std::string& rhs) {
                const int estimatedSize = static_cast<int>(lhs.length() + rhs.length() + 4U);
                ensureShardSpace(estimatedSize);
                markCurrentShardActivity();
                std::string* shard = getCurrentShardBuffer();
                shard->append(lhs);
                shard->append(" = ");
                shard->append(rhs);
                shard->append(";\n");
                currentShardSize += estimatedSize;
            }

            void setResult(const wolvrix::lib::grh::ValueId& valueId,
                           const std::string& expr,
                           const std::string& cppType,
                           int32_t width) {
                if (!enableSharding)
                {
                    valueVars[valueId] = expr;
                    return;
                }

                const std::string resultRef = materializeResultRef(valueId, cppType, width);
                emitShardAssignment(resultRef, expr);
            }

            void setCurrentActivity(std::vector<std::string> directSources, int firstShard) {
                currentOpDirectActivitySources = std::move(directSources);
                currentOpActivityFirstShard = firstShard;
            }

            void setValueActivityFirstShard(const wolvrix::lib::grh::ValueId& valueId, int firstShard) {
                if (firstShard >= 0) {
                    valueActivityFirstShard[valueId] = firstShard;
                }
            }

            void markDpicPreSettleValue(const wolvrix::lib::grh::ValueId& valueId) {
                if (!enableSharding || !enableActivityWatermark) {
                    return;
                }
                if (const auto firstIt = valueActivityFirstShard.find(valueId);
                    firstIt != valueActivityFirstShard.end() && firstIt->second >= 0) {
                    dpicPreSettleShards.insert(firstIt->second);
                }
                if (const auto producerIt = valueProducerShard.find(valueId);
                    producerIt != valueProducerShard.end() && producerIt->second >= 0) {
                    dpicPreSettleShards.insert(producerIt->second);
                }
            }

            void markDirtyReplayProducerValue(const wolvrix::lib::grh::ValueId& valueId) {
                if (!enableSharding) {
                    return;
                }
                if (const auto opIt = valueProducerOpIndex.find(valueId);
                    opIt != valueProducerOpIndex.end() && opIt->second >= 0) {
                    dirtyReplayProducerOpSeeds.push_back(opIt->second);
                }
            }
            int shardCount() const {
                return static_cast<int>(shardBuffers.size());
            }

            std::string allocatePersistentStorage(const std::string& storageName,
                                                  const std::string& cppType,
                                                  int32_t width) {
                if (auto it = persistentVars.find(storageName); it != persistentVars.end()) {
                    return it->second;
                }

                std::string expr;
                if (cppType == "std::uint8_t")
                {
                    expr = "state_->stateU8[" + std::to_string(stateU8Count++) + "]";
                }
                else if (cppType == "std::uint16_t")
                {
                    expr = "state_->stateU16[" + std::to_string(stateU16Count++) + "]";
                }
                else if (cppType == "std::uint32_t")
                {
                    expr = "state_->stateU32[" + std::to_string(stateU32Count++) + "]";
                }
                else if (cppType == "std::uint64_t")
                {
                    expr = "state_->stateU64[" + std::to_string(stateU64Count++) + "]";
                }
                else
                {
                    expr = "state_->stateVec[" + std::to_string(stateVecWidths.size()) + "]";
                    stateVecWidths.push_back(width);
                }
                persistentVars.emplace(storageName, expr);
                return expr;
            }

            std::string persistentStorageExpr(const std::string& storageName) const {
                if (auto it = persistentVars.find(storageName); it != persistentVars.end()) {
                    return it->second;
                }
                return "state_->" + storageName;
            }
        };

        struct SequentialChunkPlan
        {
            std::string domainKey;
            std::string methodName;
            std::vector<std::string> regNames;
            std::map<std::string, std::vector<std::string>> regStmts;
            std::vector<std::string> stmts;
            std::vector<bool> stmtDirtyOnCommit;
            std::vector<std::vector<std::string>> stmtActivitySources;
            bool preReg = false;
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

            char base = static_cast<char>(std::tolower(static_cast<unsigned char>(verilogConst[apostrophe + 1])));
            std::string value = verilogConst.substr(apostrophe + 2);
            for (char &ch : value) {
                if (ch == '_' || std::isspace(static_cast<unsigned char>(ch))) {
                    continue;
                }
                if (ch == 'x' || ch == 'X' || ch == 'z' || ch == 'Z' || ch == '?') {
                    ch = '0';
                }
            }

            // Convert based on base
            switch (base) {
                case 'h': // Hexadecimal
                    return "0x" + value;
                case 'b': // Binary
                    try {
                        unsigned long long val = std::stoull(value, nullptr, 2);
                        std::ostringstream oss;
                        oss << "0x" << std::hex << std::uppercase << val << "ULL";
                        return oss.str();
                    } catch (...) {
                        return "0";
                    }
                case 'd': // Decimal
                    return value;
                case 'o': // Octal
                    try {
                        unsigned long long val = std::stoull(value, nullptr, 8);
                        std::ostringstream oss;
                        oss << "0x" << std::hex << std::uppercase << val << "ULL";
                        return oss.str();
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
                return "wolvrix_gsim_zero_bits(" + std::to_string(chunkCount) + "U)";
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


        std::string dpicCastExpr(const std::string &expr, int64_t width)
        {
            if (width <= 8)
            {
                return "static_cast<std::uint8_t>(" + expr + ")";
            }
            if (width <= 16)
            {
                return "static_cast<std::uint16_t>(" + expr + ")";
            }
            if (width <= 32)
            {
                return "static_cast<std::uint32_t>(" + expr + ")";
            }
            return "static_cast<std::uint64_t>(" + expr + ")";
        }

        std::string dpicCppTypeForWidth(int64_t width)
        {
            if (width <= 8)
            {
                return "std::uint8_t";
            }
            if (width <= 16)
            {
                return "std::uint16_t";
            }
            if (width <= 32)
            {
                return "std::uint32_t";
            }
            return "std::uint64_t";
        }

        std::string dpicReturnTypeForImport(const DpicImportInfo& importInfo)
        {
            if (importInfo.returnType == "int")
            {
                return "int";
            }
            return dpicCppTypeForWidth(importInfo.returnWidth);
        }

        bool startsWith(std::string_view text, std::string_view prefix)
        {
            return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
        }

        bool isZeroLiteralText(std::string_view text)
        {
            bool sawDigit = false;
            for (char ch : text)
            {
                if (ch == '\'' || ch == '_' || std::isspace(static_cast<unsigned char>(ch)))
                {
                    continue;
                }
                if (ch == 'b' || ch == 'B' || ch == 'h' || ch == 'H' || ch == 'd' || ch == 'D' || ch == 'o' ||
                    ch == 'O' || (ch >= '0' && ch <= '9'))
                {
                    if (ch >= '1' && ch <= '9')
                    {
                        return false;
                    }
                    if (ch == '0')
                    {
                        sawDigit = true;
                    }
                    continue;
                }
                return false;
            }
            return sawDigit;
        }

        std::string makeWideScalarLiteralExpr(int32_t width, const std::string &scalarExpr)
        {
            return "([&](){ std::vector<std::uint64_t> value = " + zeroInitializerForWidth(width) +
                   "; if (!value.empty()) value[0] = static_cast<std::uint64_t>(" + scalarExpr + "); return value; }())";
        }

        int verilogDigitValue(char ch)
        {
            if (ch >= '0' && ch <= '9')
            {
                return ch - '0';
            }
            if (ch >= 'a' && ch <= 'f')
            {
                return 10 + (ch - 'a');
            }
            if (ch >= 'A' && ch <= 'F')
            {
                return 10 + (ch - 'A');
            }
            return -1;
        }

        std::optional<std::string> convertWideVerilogConstant(const std::string &verilogConst, int32_t width)
        {
            if (width <= 64)
            {
                return std::nullopt;
            }

            const auto apostrophe = verilogConst.find('\'');
            if (apostrophe == std::string::npos || apostrophe + 2 >= verilogConst.size())
            {
                return std::nullopt;
            }

            const char base = static_cast<char>(std::tolower(static_cast<unsigned char>(verilogConst[apostrophe + 1])));
            int radix = 0;
            switch (base)
            {
            case 'b':
                radix = 2;
                break;
            case 'o':
                radix = 8;
                break;
            case 'd':
                radix = 10;
                break;
            case 'h':
                radix = 16;
                break;
            default:
                return std::nullopt;
            }

            std::string digits;
            digits.reserve(verilogConst.size() - apostrophe - 2);
            for (std::size_t i = apostrophe + 2; i < verilogConst.size(); ++i)
            {
                const char ch = verilogConst[i];
                if (ch == '_' || std::isspace(static_cast<unsigned char>(ch)))
                {
                    continue;
                }
                if (ch == 'x' || ch == 'X' || ch == 'z' || ch == 'Z' || ch == '?')
                {
                    digits.push_back('0');
                    continue;
                }
                digits.push_back(ch);
            }
            if (digits.empty())
            {
                return zeroInitializerForWidth(width);
            }

            std::vector<std::uint64_t> words(static_cast<std::size_t>((width + 63) / 64), 0ULL);
            for (char ch : digits)
            {
                const int digit = verilogDigitValue(ch);
                if (digit < 0 || digit >= radix)
                {
                    return std::nullopt;
                }
                unsigned __int128 carry = static_cast<unsigned __int128>(digit);
                for (auto &word : words)
                {
                    const unsigned __int128 accum =
                        static_cast<unsigned __int128>(word) * static_cast<unsigned __int128>(radix) + carry;
                    word = static_cast<std::uint64_t>(accum);
                    carry = accum >> 64;
                }
            }

            if ((width % 64) != 0)
            {
                words.back() &= ((1ULL << (width % 64)) - 1ULL);
            }

            std::ostringstream oss;
            oss << "std::vector<std::uint64_t>{";
            for (std::size_t i = 0; i < words.size(); ++i)
            {
                if (i != 0)
                {
                    oss << ", ";
                }
                oss << "0x" << std::hex << std::uppercase << words[i] << "ULL";
            }
            oss << "}";
            return oss.str();
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

        bool isNoDiffGuardedDpicTarget(std::string_view target)
        {
            return startsWith(target, "v_difftest_");
        }

        void recordDpicCall(CodegenState& state, std::string_view target)
        {
            state.emitsDpicCalls = true;
            if (isNoDiffGuardedDpicTarget(target)) {
                state.emitsNoDiffGuardedDpicCalls = true;
            } else {
                state.emitsRuntimeDpicCalls = true;
            }
        }

        std::string guardNoDiffDpicStatement(std::string stmt, std::string_view target)
        {
            if (!isNoDiffGuardedDpicTarget(target)) {
                return stmt;
            }
            return std::string("#ifndef CONFIG_NO_DIFFTEST\n") + stmt + "\n#endif";
        }

        std::string guardNoDiffDpicBlock(std::string block, std::string_view target)
        {
            if (!isNoDiffGuardedDpicTarget(target)) {
                return block;
            }
            return std::string("\n#ifndef CONFIG_NO_DIFFTEST\n") + block + "\n#endif\n";
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
            const auto operands = op.operands();
            const auto results = op.results();

            // `lowerOperation` is called once per topo op; XiangShan-scale emit
            // calls operand/result width and expression helpers millions of times.
            // Cache the immutable per-op facts locally so each switch arm does not
            // repeat Graph value lookups and valueVars hash probes.
            std::vector<int32_t> operandWidths;
            operandWidths.reserve(operands.size());
            for (const auto operand : operands) {
                operandWidths.push_back(graph.getValue(operand).width());
            }
            std::vector<int32_t> resultWidths;
            resultWidths.reserve(results.size());
            for (const auto result : results) {
                resultWidths.push_back(graph.getValue(result).width());
            }

            auto getOperandWidth = [&](std::size_t idx) -> int32_t {
                if (idx >= operandWidths.size()) {
                    return 0;
                }
                return operandWidths[idx];
            };
            auto getResultWidth = [&](std::size_t idx) -> int32_t {
                if (idx >= resultWidths.size()) {
                    return 0;
                }
                return resultWidths[idx];
            };
            std::uint8_t opDirtyReplayMask = 0;
            std::vector<std::string> directActivitySources;
            int opActivityFirstShard = -1;
            auto mergeFirstShard = [&](int shard) {
                if (shard < 0) {
                    return;
                }
                if (opActivityFirstShard < 0 || shard < opActivityFirstShard) {
                    opActivityFirstShard = shard;
                }
            };
            std::set<int> opDependencyProducerShards;
            for (const auto operand : operands)
            {
                if (const auto rootIt = state.dirtyReplayRootMasks.find(operand);
                    rootIt != state.dirtyReplayRootMasks.end())
                {
                    opDirtyReplayMask |= rootIt->second;
                    if (const auto sourceIt = state.inputActivitySourceNames.find(operand);
                        sourceIt != state.inputActivitySourceNames.end())
                    {
                        directActivitySources.push_back(sourceIt->second);
                    }
                }
                if (const auto it = state.valueDirtyReplayMasks.find(operand);
                    it != state.valueDirtyReplayMasks.end())
                {
                    opDirtyReplayMask |= it->second;
                }
                if (state.enableSharding) {
                    if (const auto producerIt = state.valueProducerShard.find(operand);
                        producerIt != state.valueProducerShard.end() && producerIt->second >= 0)
                    {
                        opDependencyProducerShards.insert(producerIt->second);
                    }
                }
                if (state.enableActivityWatermark)
                {
                    if (const auto shardIt = state.valueActivityFirstShard.find(operand);
                        shardIt != state.valueActivityFirstShard.end())
                    {
                        mergeFirstShard(shardIt->second);
                    }
                    if (const auto producerIt = state.valueProducerShard.find(operand);
                        producerIt != state.valueProducerShard.end() && producerIt->second >= 0)
                    {
                        mergeFirstShard(producerIt->second);
                    }
                }
            }
            state.currentOpDirtyReplayMask = opDirtyReplayMask;
            state.currentOpFirstEmittedShard = -1;
            if (state.enableActivityWatermark) {
                state.setCurrentActivity(std::move(directActivitySources), opActivityFirstShard);
            } else {
                state.setCurrentActivity({}, -1);
            }

            // Helper to get operand variable name (now returns var name from valueVars).
            // Cache per operand because lowering often references the same operand
            // expression repeatedly while building one emitted C++ statement.
            std::vector<std::string> operandExprCache(operands.size());
            std::vector<std::uint8_t> operandExprCached(operands.size(), 0);
            auto getOperandExpr = [&](size_t idx) -> const std::string& {
                static const std::string zero = "0";
                if (idx >= operands.size()) {
                    return zero;
                }
                if (operandExprCached[idx] == 0) {
                    if (auto it = state.valueVars.find(operands[idx]); it != state.valueVars.end()) {
                        operandExprCache[idx] = it->second;
                    } else {
                        operandExprCache[idx] = zeroInitializerForWidth(operandWidths[idx]);
                    }
                    operandExprCached[idx] = 1;
                }
                return operandExprCache[idx];
            };

            auto recordResultMetadata = [&](size_t idx) {
                if (idx >= results.size()) {
                    return;
                }
                // Absence in valueDirtyReplayMasks means no dirty replay
                // dependency.  Do not insert the overwhelmingly common non-dirty
                // entries: XiangShan-scale emit otherwise pays millions of hash
                // insertions.
                if (opDirtyReplayMask != 0) {
                    state.valueDirtyReplayMasks.emplace(results[idx], opDirtyReplayMask);
                }
                const int resultProducerShard = state.enableSharding ? state.lastEmittedShard : -1;
                if (resultProducerShard >= 0) {
                    state.valueProducerShard[results[idx]] = resultProducerShard;
                    state.valueProducerOpIndex[results[idx]] = opId.index;
                    if (opId.index >= 0 && static_cast<std::size_t>(opId.index) < state.opProducerLastShardByIndex.size()) {
                        state.opProducerLastShardByIndex[static_cast<std::size_t>(opId.index)] = resultProducerShard;
                        state.opProducerFirstShardByIndex[static_cast<std::size_t>(opId.index)] =
                            state.currentOpFirstEmittedShard >= 0 ? state.currentOpFirstEmittedShard : resultProducerShard;
                    }
                    for (const int dependencyShard : opDependencyProducerShards) {
                        if (dependencyShard >= 0 && dependencyShard != resultProducerShard &&
                            dependencyShard < static_cast<int>(state.shardSuccessors.size())) {
                            state.shardSuccessors[static_cast<std::size_t>(dependencyShard)].insert(resultProducerShard);
                        }
                    }
                }
                if (state.enableActivityWatermark) {
                    int resultFirstShard = state.currentOpActivityFirstShard;
                    if (!state.currentOpDirectActivitySources.empty() && state.lastEmittedShard >= 0 &&
                        (resultFirstShard < 0 || state.lastEmittedShard < resultFirstShard)) {
                        resultFirstShard = state.lastEmittedShard;
                    }
                    state.setValueActivityFirstShard(results[idx], resultFirstShard);
                }
            };

            // Helper to set result: materialize expression as variable and write to current shard
            auto setResultExpr = [&](size_t idx, const std::string& expr) {
                if (idx < results.size()) {
                    const auto resultValue = graph.getValue(results[idx]);
                    state.setResult(results[idx], expr, getCppTypeForWidth(resultValue.width()), resultValue.width());
                    recordResultMetadata(idx);
                }
            };

            auto setWideResultStatement = [&](size_t idx, const auto& buildStatement) {
                if (idx < results.size()) {
                    const auto resultValue = graph.getValue(results[idx]);
                    const std::string resultRef = state.materializeResultRef(
                        results[idx], getCppTypeForWidth(resultValue.width()), resultValue.width());
                    state.emitShardStatement(buildStatement(resultRef));
                    recordResultMetadata(idx);
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
                            if (!results.empty()) {
                                const auto resultValue = graph.getValue(results[0]);
                                if (resultValue.width() > 64) {
                                    if (isZeroLiteralText(*strVal)) {
                                        setResultExpr(0, zeroInitializerForWidth(resultValue.width()));
                                    } else if (auto wideInit = convertWideVerilogConstant(*strVal, resultValue.width())) {
                                        setResultExpr(0, *wideInit);
                                    } else {
                                        setResultExpr(0, makeWideScalarLiteralExpr(resultValue.width(), convertVerilogConstant(*strVal)));
                                    }
                                } else {
                                    setResultExpr(0, convertVerilogConstant(*strVal));
                                }
                            } else {
                                setResultExpr(0, convertVerilogConstant(*strVal));
                            }
                        } else if (auto* intVal = std::get_if<int64_t>(&*valueAttr)) {
                            if (!results.empty()) {
                                const auto resultValue = graph.getValue(results[0]);
                                if (resultValue.width() > 64) {
                                    if (*intVal == 0) {
                                        setResultExpr(0, zeroInitializerForWidth(resultValue.width()));
                                    } else {
                                        setResultExpr(0, makeWideScalarLiteralExpr(resultValue.width(), std::to_string(*intVal)));
                                    }
                                } else {
                                    setResultExpr(0, std::to_string(*intVal));
                                }
                            } else {
                                setResultExpr(0, std::to_string(*intVal));
                            }
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
                    const auto lhsWidth = getOperandWidth(0);
                    const auto rhsWidth = getOperandWidth(1);
                    const auto resultWidth = results.empty() ? std::max(lhsWidth, rhsWidth)
                                                                  : getResultWidth(0);
                    if (lhsWidth > 64 || rhsWidth > 64 || resultWidth > 64)
                    {
                        if (state.enableSharding) {
                            setWideResultStatement(0, [&](const std::string& resultRef) {
                                return "wolvrix_gsim_add_into(" + resultRef + ", " + getOperandExpr(0) + ", " +
                                       getOperandExpr(1) + ", " + std::to_string(resultWidth) + "U);";
                            });
                        } else {
                            setResultExpr(0,
                                          "wolvrix_gsim_add(" + getOperandExpr(0) + ", " + getOperandExpr(1) + ")");
                        }
                    }
                    else
                    {
                        setResultExpr(0, "((" + getOperandExpr(0) + ") + (" + getOperandExpr(1) + "))");
                    }
                    break;
                }

                case OperationKind::kSub: {
                    const auto lhsWidth = getOperandWidth(0);
                    const auto rhsWidth = getOperandWidth(1);
                    const auto resultWidth = results.empty() ? std::max(lhsWidth, rhsWidth)
                                                                  : getResultWidth(0);
                    if (lhsWidth > 64 || rhsWidth > 64 || resultWidth > 64)
                    {
                        if (state.enableSharding) {
                            setWideResultStatement(0, [&](const std::string& resultRef) {
                                return "wolvrix_gsim_sub_into(" + resultRef + ", " + getOperandExpr(0) + ", " +
                                       getOperandExpr(1) + ", " + std::to_string(resultWidth) + "U);";
                            });
                        } else {
                            setResultExpr(0,
                                          "wolvrix_gsim_sub(" + getOperandExpr(0) + ", " + getOperandExpr(1) + ")");
                        }
                    }
                    else
                    {
                        setResultExpr(0, "((" + getOperandExpr(0) + ") - (" + getOperandExpr(1) + "))");
                    }
                    break;
                }

                case OperationKind::kMul: {
                    setResultExpr(0, "((" + getOperandExpr(0) + ") * (" + getOperandExpr(1) + "))");
                    break;
                }

                case OperationKind::kDiv: {
                    setResultExpr(0, "wolvrix_gsim_div(" + getOperandExpr(0) + ", " + getOperandExpr(1) + ")");
                    break;
                }

                case OperationKind::kAnd: {
                    const auto lhsWidth = getOperandWidth(0);
                    const auto rhsWidth = getOperandWidth(1);
                    const auto resultWidth = results.empty() ? std::max(lhsWidth, rhsWidth)
                                                                  : getResultWidth(0);
                    if (lhsWidth > 64 || rhsWidth > 64 || resultWidth > 64)
                    {
                        if (state.enableSharding) {
                            setWideResultStatement(0, [&](const std::string& resultRef) {
                                return "wolvrix_gsim_bitwise_and_into(" + resultRef + ", " +
                                       getOperandExpr(0) + ", " + getOperandExpr(1) + ", " +
                                       std::to_string(resultWidth) + "U);";
                            });
                        } else {
                            setResultExpr(0,
                                          "wolvrix_gsim_bitwise_and(" + getOperandExpr(0) + ", " + getOperandExpr(1) +
                                              ")");
                        }
                    }
                    else
                    {
                        setResultExpr(0, "((" + getOperandExpr(0) + ") & (" + getOperandExpr(1) + "))");
                    }
                    break;
                }

                case OperationKind::kOr: {
                    const auto lhsWidth = getOperandWidth(0);
                    const auto rhsWidth = getOperandWidth(1);
                    const auto resultWidth = results.empty() ? std::max(lhsWidth, rhsWidth)
                                                                  : getResultWidth(0);
                    if (lhsWidth > 64 || rhsWidth > 64 || resultWidth > 64)
                    {
                        if (state.enableSharding) {
                            setWideResultStatement(0, [&](const std::string& resultRef) {
                                return "wolvrix_gsim_bitwise_or_into(" + resultRef + ", " +
                                       getOperandExpr(0) + ", " + getOperandExpr(1) + ", " +
                                       std::to_string(resultWidth) + "U);";
                            });
                        } else {
                            setResultExpr(0,
                                          "wolvrix_gsim_bitwise_or(" + getOperandExpr(0) + ", " + getOperandExpr(1) +
                                              ")");
                        }
                    }
                    else
                    {
                        setResultExpr(0, "((" + getOperandExpr(0) + ") | (" + getOperandExpr(1) + "))");
                    }
                    break;
                }

                case OperationKind::kXor: {
                    const auto lhsWidth = getOperandWidth(0);
                    const auto rhsWidth = getOperandWidth(1);
                    const auto resultWidth = results.empty() ? std::max(lhsWidth, rhsWidth)
                                                                  : getResultWidth(0);
                    if (lhsWidth > 64 || rhsWidth > 64 || resultWidth > 64)
                    {
                        if (state.enableSharding) {
                            setWideResultStatement(0, [&](const std::string& resultRef) {
                                return "wolvrix_gsim_bitwise_xor_into(" + resultRef + ", " +
                                       getOperandExpr(0) + ", " + getOperandExpr(1) + ", " +
                                       std::to_string(resultWidth) + "U);";
                            });
                        } else {
                            setResultExpr(0,
                                          "wolvrix_gsim_bitwise_xor(" + getOperandExpr(0) + ", " + getOperandExpr(1) +
                                              ")");
                        }
                    }
                    else
                    {
                        setResultExpr(0, "((" + getOperandExpr(0) + ") ^ (" + getOperandExpr(1) + "))");
                    }
                    break;
                }

                case OperationKind::kXnor: {
                    if (!results.empty())
                    {
                        const auto resultValue = graph.getValue(results[0]);
                        setResultExpr(0, maskExprForWidth("(~((" + getOperandExpr(0) + ") ^ (" + getOperandExpr(1) + ")))",
                                                          resultValue.width()));
                    }
                    break;
                }

                case OperationKind::kEq: {
                    const auto lhsWidth = getOperandWidth(0);
                    const auto rhsWidth = getOperandWidth(1);
                    if (lhsWidth > 64 || rhsWidth > 64) {
                        setResultExpr(0,
                                      "(wolvrix_gsim_compare_bits(" + getOperandExpr(0) + ", " +
                                          std::to_string(lhsWidth) + ", " + getOperandExpr(1) + ", " +
                                          std::to_string(rhsWidth) + ", false) == 0 ? 1U : 0U)");
                    } else {
                        setResultExpr(0, "(((" + getOperandExpr(0) + ") == (" + getOperandExpr(1) + ")) ? 1U : 0U)");
                    }
                    break;
                }

                case OperationKind::kCaseEq: {
                    const auto lhsWidth = getOperandWidth(0);
                    const auto rhsWidth = getOperandWidth(1);
                    if (lhsWidth > 64 || rhsWidth > 64) {
                        setResultExpr(0,
                                      "(wolvrix_gsim_compare_bits(" + getOperandExpr(0) + ", " +
                                          std::to_string(lhsWidth) + ", " + getOperandExpr(1) + ", " +
                                          std::to_string(rhsWidth) + ", false) == 0 ? 1U : 0U)");
                    } else {
                        setResultExpr(0, "(((" + getOperandExpr(0) + ") == (" + getOperandExpr(1) + ")) ? 1U : 0U)");
                    }
                    break;
                }

                case OperationKind::kCaseNe: {
                    const auto lhsWidth = getOperandWidth(0);
                    const auto rhsWidth = getOperandWidth(1);
                    if (lhsWidth > 64 || rhsWidth > 64) {
                        setResultExpr(0,
                                      "(wolvrix_gsim_compare_bits(" + getOperandExpr(0) + ", " +
                                          std::to_string(lhsWidth) + ", " + getOperandExpr(1) + ", " +
                                          std::to_string(rhsWidth) + ", false) != 0 ? 1U : 0U)");
                    } else {
                        setResultExpr(0, "(((" + getOperandExpr(0) + ") != (" + getOperandExpr(1) + ")) ? 1U : 0U)");
                    }
                    break;
                }

                case OperationKind::kReduceAnd: {
                    const auto operandWidth = getOperandWidth(0);
                    if (operandWidth > 64)
                    {
                        setResultExpr(0,
                                      "(wolvrix_gsim_reduce_and(" + getOperandExpr(0) + ", " +
                                          std::to_string(operandWidth) + ") ? 1U : 0U)");
                    }
                    else
                    {
                        setResultExpr(
                            0,
                            "(((" + getOperandExpr(0) + ") == " + generateMask(operandWidth) + ") ? 1U : 0U)");
                    }
                    break;
                }

                case OperationKind::kReduceOr: {
                    const auto operandWidth = getOperandWidth(0);
                    if (operandWidth > 64)
                    {
                        setResultExpr(0,
                                      "(wolvrix_gsim_reduce_or(" + getOperandExpr(0) + ", " +
                                          std::to_string(operandWidth) + ") ? 1U : 0U)");
                    }
                    else
                    {
                        setResultExpr(0, "(((" + getOperandExpr(0) + ") != 0) ? 1U : 0U)");
                    }
                    break;
                }

                case OperationKind::kReduceXor: {
                    const auto operandWidth = getOperandWidth(0);
                    if (operandWidth > 64)
                    {
                        setResultExpr(0,
                                      "(wolvrix_gsim_reduce_xor(" + getOperandExpr(0) + ", " +
                                          std::to_string(operandWidth) + ") ? 1U : 0U)");
                    }
                    else
                    {
                        setResultExpr(
                            0,
                            "(static_cast<unsigned>(__builtin_parityll(static_cast<unsigned long long>(" +
                                getOperandExpr(0) + "))))");
                    }
                    break;
                }

                case OperationKind::kLt: {
                    const auto lhsWidth = getOperandWidth(0);
                    const auto rhsWidth = getOperandWidth(1);
                    const bool signedCompare = graph.valueSigned(operands[0]) && graph.valueSigned(operands[1]);
                    const bool hasSignedOperand = graph.valueSigned(operands[0]) || graph.valueSigned(operands[1]);
                    if (lhsWidth > 64 || rhsWidth > 64 || hasSignedOperand) {
                        setResultExpr(0,
                                      "(wolvrix_gsim_compare_bits(" + getOperandExpr(0) + ", " +
                                          std::to_string(lhsWidth) + ", " + getOperandExpr(1) + ", " +
                                          std::to_string(rhsWidth) + ", " + (signedCompare ? "true" : "false") +
                                          ") < 0 ? 1U : 0U)");
                    } else {
                        setResultExpr(0, "(((" + getOperandExpr(0) + ") < (" + getOperandExpr(1) + ")) ? 1U : 0U)");
                    }
                    break;
                }

                case OperationKind::kLe: {
                    const auto lhsWidth = getOperandWidth(0);
                    const auto rhsWidth = getOperandWidth(1);
                    const bool signedCompare = graph.valueSigned(operands[0]) && graph.valueSigned(operands[1]);
                    const bool hasSignedOperand = graph.valueSigned(operands[0]) || graph.valueSigned(operands[1]);
                    if (lhsWidth > 64 || rhsWidth > 64 || hasSignedOperand) {
                        setResultExpr(0,
                                      "(wolvrix_gsim_compare_bits(" + getOperandExpr(0) + ", " +
                                          std::to_string(lhsWidth) + ", " + getOperandExpr(1) + ", " +
                                          std::to_string(rhsWidth) + ", " + (signedCompare ? "true" : "false") +
                                          ") <= 0 ? 1U : 0U)");
                    } else {
                        setResultExpr(0, "(((" + getOperandExpr(0) + ") <= (" + getOperandExpr(1) + ")) ? 1U : 0U)");
                    }
                    break;
                }

                case OperationKind::kGt: {
                    const auto lhsWidth = getOperandWidth(0);
                    const auto rhsWidth = getOperandWidth(1);
                    const bool signedCompare = graph.valueSigned(operands[0]) && graph.valueSigned(operands[1]);
                    const bool hasSignedOperand = graph.valueSigned(operands[0]) || graph.valueSigned(operands[1]);
                    if (lhsWidth > 64 || rhsWidth > 64 || hasSignedOperand) {
                        setResultExpr(0,
                                      "(wolvrix_gsim_compare_bits(" + getOperandExpr(0) + ", " +
                                          std::to_string(lhsWidth) + ", " + getOperandExpr(1) + ", " +
                                          std::to_string(rhsWidth) + ", " + (signedCompare ? "true" : "false") +
                                          ") > 0 ? 1U : 0U)");
                    } else {
                        setResultExpr(0, "(((" + getOperandExpr(0) + ") > (" + getOperandExpr(1) + ")) ? 1U : 0U)");
                    }
                    break;
                }

                case OperationKind::kGe: {
                    const auto lhsWidth = getOperandWidth(0);
                    const auto rhsWidth = getOperandWidth(1);
                    const bool signedCompare = graph.valueSigned(operands[0]) && graph.valueSigned(operands[1]);
                    const bool hasSignedOperand = graph.valueSigned(operands[0]) || graph.valueSigned(operands[1]);
                    if (lhsWidth > 64 || rhsWidth > 64 || hasSignedOperand) {
                        setResultExpr(0,
                                      "(wolvrix_gsim_compare_bits(" + getOperandExpr(0) + ", " +
                                          std::to_string(lhsWidth) + ", " + getOperandExpr(1) + ", " +
                                          std::to_string(rhsWidth) + ", " + (signedCompare ? "true" : "false") +
                                          ") >= 0 ? 1U : 0U)");
                    } else {
                        setResultExpr(0, "(((" + getOperandExpr(0) + ") >= (" + getOperandExpr(1) + ")) ? 1U : 0U)");
                    }
                    break;
                }

                case OperationKind::kNe: {
                    const auto lhsWidth = getOperandWidth(0);
                    const auto rhsWidth = getOperandWidth(1);
                    if (lhsWidth > 64 || rhsWidth > 64) {
                        setResultExpr(0,
                                      "(wolvrix_gsim_compare_bits(" + getOperandExpr(0) + ", " +
                                          std::to_string(lhsWidth) + ", " + getOperandExpr(1) + ", " +
                                          std::to_string(rhsWidth) + ", false) != 0 ? 1U : 0U)");
                    } else {
                        setResultExpr(0, "(((" + getOperandExpr(0) + ") != (" + getOperandExpr(1) + ")) ? 1U : 0U)");
                    }
                    break;
                }

                case OperationKind::kNot: {
                    if (!results.empty())
                    {
                        const auto resultValue = graph.getValue(results[0]);
                        if (resultValue.width() > 64)
                        {
                            if (state.enableSharding) {
                                setWideResultStatement(0, [&](const std::string& resultRef) {
                                    return "wolvrix_gsim_bitwise_not_into(" + resultRef + ", " +
                                           getOperandExpr(0) + ", " + std::to_string(resultValue.width()) + "U);";
                                });
                            } else {
                                setResultExpr(
                                    0,
                                    "wolvrix_gsim_bitwise_not(" + getOperandExpr(0) + ", " +
                                        std::to_string(resultValue.width()) + ")");
                            }
                        }
                        else
                        {
                            setResultExpr(0, maskExprForWidth("(~" + getOperandExpr(0) + ")", resultValue.width()));
                        }
                    }
                    break;
                }

                case OperationKind::kLogicNot: {
                    setResultExpr(0, "(!" + getOperandExpr(0) + ")");
                    break;
                }

                case OperationKind::kLogicAnd: {
                    setResultExpr(0, "(((" + getOperandExpr(0) + ") && (" + getOperandExpr(1) + ")) ? 1U : 0U)");
                    break;
                }

                case OperationKind::kLogicOr: {
                    setResultExpr(0, "(((" + getOperandExpr(0) + ") || (" + getOperandExpr(1) + ")) ? 1U : 0U)");
                    break;
                }

                case OperationKind::kMux: {
                    setResultExpr(0, "(" + getOperandExpr(0) + " ? " + getOperandExpr(1) + " : " + getOperandExpr(2) + ")");
                    break;
                }

                case OperationKind::kConcat: {
                    if (operands.empty()) {
                        break;
                    }
                    if (operands.size() == 1) {
                        setResultExpr(0, getOperandExpr(0));
                        break;
                    }
                    std::int64_t totalWidth = 0;
                    bool widthKnown = true;
                    bool hasWideOperand = false;
                    for (const auto operand : operands) {
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

                    auto scalarConcatTerm = [&](std::size_t operandIndex, std::uint64_t sourceShift,
                                                   std::uint64_t destShift, std::uint64_t width) {
                        std::string term = "static_cast<std::uint64_t>(" + getOperandExpr(operandIndex) + ")";
                        if (sourceShift > 0) {
                            term = "(" + term + " >> " + std::to_string(sourceShift) + ")";
                        }
                        if (width < 64) {
                            term = "(" + term + " & " + generateMask(static_cast<int32_t>(width)) + ")";
                        }
                        if (destShift > 0) {
                            term = "(" + term + " << " + std::to_string(destShift) + ")";
                        }
                        return term;
                    };

                    if (totalWidth > 64 || hasWideOperand) {
                        if (!state.enableSharding) {
                            std::string expr = getOperandExpr(0);
                            std::int64_t exprWidth = getOperandWidth(0);
                            for (std::size_t i = 1; i < operands.size(); ++i) {
                                const auto value = graph.getValue(operands[i]);
                                expr = "wolvrix_gsim_concat(" + expr + ", " + std::to_string(exprWidth) + ", " +
                                       getOperandExpr(i) + ", " + std::to_string(value.width()) + ")";
                                exprWidth += value.width();
                            }
                            setResultExpr(0, expr);
                            break;
                        }
                        const auto resultValue = graph.getValue(results[0]);
                        const std::string resultRef = state.materializeResultRef(
                            results[0], getCppTypeForWidth(resultValue.width()), resultValue.width());
                        state.emitShardStatement(resultRef + " = " +
                                                     zeroInitializerForWidth(resultValue.width()) + ";");

                        if (!hasWideOperand) {
                            std::vector<std::vector<std::string>> wordTerms(
                                static_cast<std::size_t>((totalWidth + 63) / 64));
                            std::uint64_t bitOffset = 0;
                            for (std::size_t i = operands.size(); i-- > 0;) {
                                const auto value = graph.getValue(operands[i]);
                                std::uint64_t remaining = static_cast<std::uint64_t>(value.width());
                                std::uint64_t sourceShift = 0;
                                while (remaining > 0) {
                                    const std::uint64_t wordIndex = bitOffset / 64;
                                    const std::uint64_t destShift = bitOffset % 64;
                                    const std::uint64_t chunkWidth = std::min<std::uint64_t>(remaining, 64 - destShift);
                                    wordTerms[static_cast<std::size_t>(wordIndex)].push_back(
                                        scalarConcatTerm(i, sourceShift, destShift, chunkWidth));
                                    bitOffset += chunkWidth;
                                    sourceShift += chunkWidth;
                                    remaining -= chunkWidth;
                                }
                            }
                            for (std::size_t wordIndex = 0; wordIndex < wordTerms.size(); ++wordIndex) {
                                const auto& terms = wordTerms[wordIndex];
                                if (terms.empty()) {
                                    continue;
                                }
                                constexpr std::size_t termsPerStatement = 12;
                                for (std::size_t termIndex = 0; termIndex < terms.size(); termIndex += termsPerStatement) {
                                    const std::size_t end = std::min(terms.size(), termIndex + termsPerStatement);
                                    std::string expr = terms[termIndex];
                                    for (std::size_t j = termIndex + 1; j < end; ++j) {
                                        expr += " | " + terms[j];
                                    }
                                    state.emitShardStatement(resultRef + "[" + std::to_string(wordIndex) + "] |= " + expr + ";");
                                }
                            }
                            break;
                        }

                        std::uint64_t bitOffset = 0;
                        for (std::size_t i = operands.size(); i-- > 0;) {
                            const auto width = static_cast<std::uint64_t>(getOperandWidth(i));
                            state.emitShardStatement("wolvrix_gsim_store_bits(" + resultRef + ", " +
                                                     std::to_string(bitOffset) + "ULL, " + getOperandExpr(i) +
                                                     ", " + std::to_string(width) + ");");
                            bitOffset += width;
                        }
                        break;
                    }

                    if (operands.size() > 16) {
                        const auto resultValue = graph.getValue(results[0]);
                        const std::string resultRef = state.materializeResultRef(
                            results[0], getCppTypeForWidth(resultValue.width()), resultValue.width());
                        state.emitShardStatement(resultRef + " = 0;");
                        std::uint64_t bitOffset = 0;
                        std::vector<std::string> terms;
                        terms.reserve(operands.size());
                        for (std::size_t i = operands.size(); i-- > 0;) {
                            const auto width = static_cast<std::uint64_t>(getOperandWidth(i));
                            terms.push_back(scalarConcatTerm(i, 0, bitOffset, width));
                            bitOffset += width;
                        }
                        constexpr std::size_t termsPerStatement = 12;
                        for (std::size_t termIndex = 0; termIndex < terms.size(); termIndex += termsPerStatement) {
                            const std::size_t end = std::min(terms.size(), termIndex + termsPerStatement);
                            std::string expr = terms[termIndex];
                            for (std::size_t j = termIndex + 1; j < end; ++j) {
                                expr += " | " + terms[j];
                            }
                            state.emitShardStatement(resultRef + " |= " + expr + ";");
                        }
                        break;
                    }

                    std::uint64_t bitOffset = 0;
                    std::vector<std::string> terms;
                    terms.reserve(operands.size());
                    for (std::size_t i = operands.size(); i-- > 0;) {
                        const auto value = graph.getValue(operands[i]);
                        terms.push_back(scalarConcatTerm(i, 0, bitOffset, static_cast<std::uint64_t>(value.width())));
                        bitOffset += static_cast<std::uint64_t>(value.width());
                    }
                    std::string expr = terms.empty() ? "0" : terms.front();
                    for (std::size_t i = 1; i < terms.size(); ++i) {
                        expr += " | " + terms[i];
                    }
                    setResultExpr(0, expr);
                    break;
                }

                case OperationKind::kSliceDynamic: {
                    if (operands.size() < 2 || results.empty()) {
                        break;
                    }

                    auto sliceWidthAttr = op.attr("sliceWidth");
                    auto *sliceWidth = sliceWidthAttr ? std::get_if<int64_t>(&*sliceWidthAttr) : nullptr;
                    if (sliceWidth == nullptr || *sliceWidth <= 0) {
                        std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                        state.unsupportedOps.push_back("kSliceDynamic-width (" + opName + ")");
                        break;
                    }

                    const auto baseValue = graph.getValue(operands[0]);
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
                    if (state.enableSharding && operandWidth > 64 && *sliceWidth > 64) {
                        setWideResultStatement(0, [&](const std::string& resultRef) {
                            return "wolvrix_gsim_slice_dynamic_to_bits_into(" + resultRef + ", " +
                                   getOperandExpr(0) + ", " + indexExpr + ", " +
                                   std::to_string(*sliceWidth) + ", " + std::to_string(operandWidth) + ");";
                        });
                        break;
                    }

                    if (operandWidth > 64 && *sliceWidth > 64) {
                        setResultExpr(0,
                                      "wolvrix_gsim_slice_dynamic_to_bits(" + getOperandExpr(0) + ", " + indexExpr +
                                          ", " + std::to_string(*sliceWidth) + ", " +
                                          std::to_string(operandWidth) + ")");
                        break;
                    }

                    if (operandWidth > 64) {
                        const std::string slicedExpr =
                            "wolvrix_gsim_slice_dynamic_to_u64(" + getOperandExpr(0) + ", " + indexExpr + ", " +
                            std::to_string(*sliceWidth) + ", " + std::to_string(operandWidth) + ")";
                        setResultExpr(0, maskExprForWidth(slicedExpr, static_cast<int32_t>(*sliceWidth)));
                        break;
                    }

                    const std::string slicedExpr =
                        "((" + indexExpr + " >= " + std::to_string(operandWidth) + ") ? 0ULL : ((" +
                        "(" + getOperandExpr(0) + ") >> " + indexExpr + ") & " +
                        generateMask(static_cast<int32_t>(*sliceWidth)) + "))";
                    setResultExpr(0, maskExprForWidth(slicedExpr, static_cast<int32_t>(*sliceWidth)));
                    break;
                }

                case OperationKind::kSliceStatic: {
                    if (operands.empty() || results.empty()) {
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

                    const auto baseValue = graph.getValue(operands[0]);
                    const auto operandWidth = baseValue.width();
                    const auto sliceWidth = *sliceEnd - *sliceStart + 1;
                    if (operandWidth <= 0 || *sliceEnd >= operandWidth || sliceWidth <= 0) {
                        std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                        state.unsupportedOps.push_back("kSliceStatic-wide (" + opName + ")");
                        break;
                    }

                    if (state.enableSharding && operandWidth > 64 && sliceWidth > 64)
                    {
                        setWideResultStatement(0, [&](const std::string& resultRef) {
                            return "wolvrix_gsim_slice_dynamic_to_bits_into(" + resultRef + ", " +
                                   getOperandExpr(0) + ", " + std::to_string(*sliceStart) + "ULL, " +
                                   std::to_string(sliceWidth) + ", " + std::to_string(operandWidth) + ");";
                        });
                        break;
                    }

                    if (operandWidth > 64 && sliceWidth > 64)
                    {
                        setResultExpr(0,
                                      "wolvrix_gsim_slice_dynamic_to_bits(" + getOperandExpr(0) + ", " +
                                          std::to_string(*sliceStart) + "ULL, " +
                                          std::to_string(sliceWidth) + ", " +
                                          std::to_string(operandWidth) + ")");
                        break;
                    }

                    if (operandWidth > 64)
                    {
                        const std::string slicedExpr =
                            "wolvrix_gsim_slice_dynamic_to_u64(" + getOperandExpr(0) + ", " +
                            std::to_string(*sliceStart) + "ULL, " + std::to_string(sliceWidth) + ", " +
                            std::to_string(operandWidth) + ")";
                        setResultExpr(0, maskExprForWidth(slicedExpr, static_cast<int32_t>(sliceWidth)));
                        break;
                    }

                    const std::string slicedExpr =
                        "(((" + getOperandExpr(0) + ") >> " + std::to_string(*sliceStart) + ") & " +
                        generateMask(static_cast<int32_t>(sliceWidth)) + ")";
                    setResultExpr(0, maskExprForWidth(slicedExpr, static_cast<int32_t>(sliceWidth)));
                    break;
                }

                case OperationKind::kReplicate: {
                    if (operands.empty() || results.empty()) {
                        break;
                    }
                    auto repAttr = op.attr("rep");
                    auto *rep = repAttr ? std::get_if<int64_t>(&*repAttr) : nullptr;
                    if (rep == nullptr || *rep <= 0) {
                        std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                        state.unsupportedOps.push_back("kReplicate-attr (" + opName + ")");
                        break;
                    }
                    const auto operandWidth = getOperandWidth(0);
                    const auto totalWidth = operandWidth * *rep;
                    if (operandWidth <= 0) {
                        std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                        state.unsupportedOps.push_back("kReplicate-wide (" + opName + ")");
                        break;
                    }
                    if (state.enableSharding && (operandWidth > 64 || totalWidth > 64)) {
                        setWideResultStatement(0, [&](const std::string& resultRef) {
                            return "wolvrix_gsim_replicate_into(" + resultRef + ", " + getOperandExpr(0) + ", " +
                                   std::to_string(operandWidth) + ", " + std::to_string(*rep) + ");";
                        });
                        break;
                    }
                    if (operandWidth > 64 || totalWidth > 64) {
                        setResultExpr(0,
                                      "wolvrix_gsim_replicate(" + getOperandExpr(0) + ", " +
                                          std::to_string(operandWidth) + ", " + std::to_string(*rep) + ")");
                        break;
                    }
                    if (*rep == 1) {
                        setResultExpr(0, maskExprForWidth(getOperandExpr(0), static_cast<int32_t>(totalWidth)));
                        break;
                    }
                    std::string expr = "0";
                    for (int64_t i = 0; i < *rep; ++i) {
                        expr = "((static_cast<std::uint64_t>(" + expr + ") << " +
                               std::to_string(operandWidth) + ") | (static_cast<std::uint64_t>(" +
                               getOperandExpr(0) + ") & " + generateMask(operandWidth) + "))";
                    }
                    setResultExpr(0, maskExprForWidth(expr, static_cast<int32_t>(totalWidth)));
                    break;
                }

                case OperationKind::kShl: {
                    if (!results.empty()) {
                        const auto resultWidth = getResultWidth(0);
                        const auto operandWidth = getOperandWidth(0);
                        if (state.enableSharding && (operandWidth > 64 || resultWidth > 64)) {
                            setWideResultStatement(0, [&](const std::string& resultRef) {
                                return "wolvrix_gsim_shift_left_bits_into(" + resultRef + ", " + getOperandExpr(0) +
                                       ", " + getOperandExpr(1) + ", " + std::to_string(resultWidth) + ");";
                            });
                        } else if (operandWidth > 64 || resultWidth > 64) {
                            setResultExpr(0,
                                          "wolvrix_gsim_shift_left_bits(" + getOperandExpr(0) + ", " +
                                              getOperandExpr(1) + ", " + std::to_string(resultWidth) + ")");
                        } else {
                            setResultExpr(
                                0,
                                maskExprForWidth("((" + getOperandExpr(0) + ") << (" + getOperandExpr(1) + "))",
                                                 resultWidth));
                        }
                    }
                    break;
                }

                case OperationKind::kLShr: {
                    if (!results.empty()) {
                        const auto resultWidth = getResultWidth(0);
                        const auto operandWidth = getOperandWidth(0);
                        if (state.enableSharding && (operandWidth > 64 || resultWidth > 64)) {
                            setWideResultStatement(0, [&](const std::string& resultRef) {
                                return "wolvrix_gsim_shift_right_bits_into(" + resultRef + ", " + getOperandExpr(0) +
                                       ", " + getOperandExpr(1) + ", " + std::to_string(resultWidth) + ");";
                            });
                        } else if (operandWidth > 64 || resultWidth > 64) {
                            setResultExpr(0,
                                          "wolvrix_gsim_shift_right_bits(" + getOperandExpr(0) + ", " +
                                              getOperandExpr(1) + ", " + std::to_string(resultWidth) + ")");
                        } else {
                            setResultExpr(0, "((" + getOperandExpr(0) + ") >> (" + getOperandExpr(1) + "))");
                        }
                    }
                    break;
                }

                case OperationKind::kAShr: {
                    if (!results.empty()) {
                        const auto resultWidth = getResultWidth(0);
                        const auto operandWidth = getOperandWidth(0);
                        const bool signExtend = graph.valueSigned(operands[0]);
                        setResultExpr(0,
                                      "wolvrix_gsim_arith_shift_right(" + getOperandExpr(0) + ", " +
                                          getOperandExpr(1) + ", " + std::to_string(operandWidth) + ", " +
                                          std::to_string(resultWidth) + ", " +
                                          (signExtend ? "true" : "false") + ")");
                    }
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

                case OperationKind::kInstance: {
                    auto instanceNameAttr = op.attr("instanceName");
                    auto moduleNameAttr = op.attr("moduleName");
                    std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                    if (instanceNameAttr) {
                        if (auto *instanceName = std::get_if<std::string>(&*instanceNameAttr); instanceName != nullptr && !instanceName->empty()) {
                            opName = *instanceName;
                        }
                    }
                    if (moduleNameAttr) {
                        if (auto *moduleName = std::get_if<std::string>(&*moduleNameAttr); moduleName != nullptr && !moduleName->empty()) {
                            opName += ":" + *moduleName;
                        }
                    }
                    state.unsupportedOps.push_back("kInstance (" + opName + ")");
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
                        state.setCurrentActivity(std::vector<std::string>{regName}, -1);
                        setResultExpr(0, state.persistentStorageExpr(regName));
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
                        state.setCurrentActivity(std::vector<std::string>{latchName}, -1);
                        setResultExpr(0, state.persistentStorageExpr(latchName));
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
                    if (state.enableActivityWatermark) {
                        auto directMemorySources = state.currentOpDirectActivitySources;
                        directMemorySources.push_back(memory.storageName);
                        state.setCurrentActivity(std::move(directMemorySources), state.currentOpActivityFirstShard);
                    }
                    const std::string indexExpr =
                        "static_cast<std::size_t>(static_cast<std::uint64_t>(" + getOperandExpr(0) + "))";
                    setResultExpr(
                        0,
                        "((" + indexExpr + " < state_->" + memory.storageName + ".size()) ? " +
                            "state_->" + memory.storageName + "[" + indexExpr + "] : " + memory.zeroExpr + ")");
                    break;
                }

                case OperationKind::kMemoryWritePort: {
                    if (operands.size() < 5) {
                        break;
                    }

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
                        state.unsupportedOps.push_back("kMemoryWritePort-unresolved (" + opName + ")");
                        break;
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

                    const auto eventValue = graph.getValue(operands[4]);
                    std::string clockSymbol = eventValue.symbolText().empty()
                                                  ? "clock"
                                                  : std::string(eventValue.symbolText());
                    const std::string domainKey = eventEdge + ":" + clockSymbol;
                    const std::string clockExpr = getOperandExpr(4);
                    if (clockExpr != "0" && !clockExpr.empty()) {
                        state.sequentialClockExprs.try_emplace(domainKey, clockExpr);
                    }

                    const auto &memory = memoryIt->second;
                    const std::string condition = getOperandExpr(0);
                    const std::string indexExpr =
                        "static_cast<std::size_t>(static_cast<std::uint64_t>(" + getOperandExpr(1) + "))";
                    const std::string dataExpr = getOperandExpr(2);
                    const std::string maskExpr = getOperandExpr(3);
                    std::string writeExpr;
                    if (memory.width > 64) {
                        writeExpr = "if (wolvrix_gsim_mask_merge_in_place(state_->" + memory.storageName +
                                    "[__mem_idx], " + dataExpr + ", " + maskExpr + ")) { committed_ = true; }";
                    } else {
                        writeExpr = "const auto __mem_mask_ = static_cast<" + memory.rowType + ">(" + maskExpr +
                                    "); const auto __mem_next_ = static_cast<" + memory.rowType + ">((state_->" +
                                    memory.storageName + "[__mem_idx] & static_cast<" + memory.rowType +
                                    ">(~__mem_mask_)) | (static_cast<" + memory.rowType + ">(" + dataExpr +
                                    ") & __mem_mask_)); if (state_->" + memory.storageName +
                                    "[__mem_idx] != __mem_next_) { state_->" + memory.storageName +
                                    "[__mem_idx] = __mem_next_; committed_ = true; }";
                    }
                    state.sequentialStmts[domainKey].push_back(
                        "        if (" + condition + ") { const auto __mem_idx = " + indexExpr + "; if (__mem_idx < state_->" +
                        memory.storageName + ".size()) { " + writeExpr + " } }");
                    state.sequentialStmtDirtyOnCommit[domainKey].push_back(true);
                    state.sequentialStmtActivitySources[domainKey].push_back({memory.storageName});
                    break;
                }

                case OperationKind::kLatchWritePort: {
                    if (operands.size() < 3) {
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
                    const std::string latchExpr = state.persistentStorageExpr(latchName);
                    const std::string condition = getOperandExpr(0);
                    const std::string nextValue = getOperandExpr(1);
                    const std::string mask = getOperandExpr(2);

                    const auto latchWidthIt = state.storageWidths.find(latchName);
                    const bool wideLatch = latchWidthIt != state.storageWidths.end() && latchWidthIt->second > 64;

                    if (mask != "0") {
                        if (wideLatch) {
                            state.latchStmts.push_back(
                                "        if (" + condition + ") { " + latchExpr +
                                " = wolvrix_gsim_mask_merge(" + latchExpr + ", " + nextValue + ", " + mask +
                                "); }");
                        } else {
                            state.latchStmts.push_back(
                                "        if (" + condition + ") { " + latchExpr + " = ((" + latchExpr +
                                ") & ~(" + mask + ")) | ((" + nextValue + ") & (" + mask + ")); }");
                        }
                    } else {
                        state.latchStmts.push_back(
                            "        if (" + condition + ") { " + latchExpr + " = " + nextValue + "; }");
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
                    if (regName.empty() && !operands.empty()) {
                        auto condVal = operands[0];
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

                    if (operands.size() > 3) {
                        const std::string clockExpr = getOperandExpr(3);
                        if (clockExpr != "0" && !clockExpr.empty()) {
                            state.sequentialClockExprs.try_emplace(domainKey, clockExpr);
                        }
                    }

                        if (!regName.empty()) {
                            auto &regStmts = state.sequentialRegStmts[domainKey][regName];
                            const std::string regExpr = state.persistentStorageExpr(regName);
                            const std::string nextRegExpr = "next_" + regName;
                            const auto regWidthIt = state.storageWidths.find(regName);
                            const bool wideReg = regWidthIt != state.storageWidths.end() && regWidthIt->second > 64;
                            if (mask != "0") {
                                if (wideReg) {
                                    regStmts.push_back(
                                        "        if (" + condition + ") { " + nextRegExpr +
                                        " = wolvrix_gsim_mask_merge(" + regExpr + ", " + nextValue + ", " + mask +
                                        "); " + nextRegExpr + "_updated_ = true; committed_ = true; }");
                                } else {
                                    regStmts.push_back(
                                        "        if (" + condition + ") { " + nextRegExpr + " = ((" + regExpr +
                                        ") & ~(" + mask + ")) | ((" + nextValue + ") & (" + mask +
                                        ")); committed_ = true; }");
                                }
                            } else {
                                regStmts.push_back(
                                    "        if (" + condition + ") { " + nextRegExpr + " = " + nextValue + "; committed_ = true; }");
                            }
                        }
                    break;
                }

                case OperationKind::kDpicImport: {
                    DpicImportInfo importInfo;
                    importInfo.symbol = op.symbolText().empty() ? std::string{} : std::string(op.symbolText());
                    if (auto namesAttr = op.attr("argsName")) {
                        if (auto *names = std::get_if<std::vector<std::string>>(&*namesAttr)) {
                            importInfo.argNames = *names;
                        }
                    }
                    if (auto dirsAttr = op.attr("argsDirection")) {
                        if (auto *dirs = std::get_if<std::vector<std::string>>(&*dirsAttr)) {
                            importInfo.argDirs = *dirs;
                        }
                    }
                    if (auto widthsAttr = op.attr("argsWidth")) {
                        if (auto *widths = std::get_if<std::vector<int64_t>>(&*widthsAttr)) {
                            importInfo.argWidths = *widths;
                        }
                    }
                    if (auto hasReturnAttr = op.attr("hasReturn")) {
                        if (auto *hasReturn = std::get_if<bool>(&*hasReturnAttr)) {
                            importInfo.hasReturn = *hasReturn;
                        }
                    }
                    if (auto returnWidthAttr = op.attr("returnWidth")) {
                        if (auto *returnWidth = std::get_if<int64_t>(&*returnWidthAttr)) {
                            importInfo.returnWidth = *returnWidth;
                        }
                    }
                    if (auto returnTypeAttr = op.attr("returnType")) {
                        if (auto *returnType = std::get_if<std::string>(&*returnTypeAttr)) {
                            importInfo.returnType = *returnType;
                        }
                    }
                    if (!importInfo.symbol.empty()) {
                        state.dpicImports[importInfo.symbol] = std::move(importInfo);
                    }
                    break;
                }

                case OperationKind::kDpicCall: {
                    if (operands.empty()) {
                        break;
                    }
                    auto targetAttr = op.attr("targetImportSymbol");
                    auto inNamesAttr = op.attr("inArgName");
                    const auto *target = targetAttr ? std::get_if<std::string>(&*targetAttr) : nullptr;
                    const auto *inNames = inNamesAttr ? std::get_if<std::vector<std::string>>(&*inNamesAttr) : nullptr;
                    if (target == nullptr || target->empty() || inNames == nullptr) {
                        std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                        state.unsupportedOps.push_back("kDpicCall-metadata (" + opName + ")");
                        break;
                    }

                    const auto importIt = state.dpicImports.find(*target);
                    if (importIt == state.dpicImports.end()) {
                        std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                        state.unsupportedOps.push_back("kDpicCall-import (" + opName + ")");
                        break;
                    }
                    const DpicImportInfo &importInfo = importIt->second;
                    if (importInfo.argNames.size() != importInfo.argDirs.size()) {
                        std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                        state.unsupportedOps.push_back("kDpicCall-signature (" + opName + ")");
                        break;
                    }
                    const std::size_t formalCount = importInfo.argNames.size();
                    if (importInfo.argWidths.size() < formalCount) {
                        std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                        state.unsupportedOps.push_back("kDpicCall-widths (" + opName + ")");
                        break;
                    }

                    auto eventEdgeAttr = op.attr("eventEdge");
                    const auto *eventEdges = eventEdgeAttr ? std::get_if<std::vector<std::string>>(&*eventEdgeAttr) : nullptr;
                    const std::size_t eventCount = eventEdges != nullptr && !eventEdges->empty() ? eventEdges->size() : 1U;
                    if (operands.size() < 1U + eventCount) {
                        std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                        state.unsupportedOps.push_back("kDpicCall-operands (" + opName + ")");
                        break;
                    }
                    const std::size_t eventStart = operands.size() - eventCount;
                    const std::string condition = getOperandExpr(0);
                    auto hasReturnAttr = op.attr("hasReturn");
                    const auto *hasReturnPtr = hasReturnAttr ? std::get_if<bool>(&*hasReturnAttr) : nullptr;
                    const bool hasReturn = hasReturnPtr != nullptr ? *hasReturnPtr : importInfo.hasReturn;
                    auto outNamesAttr = op.attr("outArgName");
                    const auto *outNames = outNamesAttr ? std::get_if<std::vector<std::string>>(&*outNamesAttr) : nullptr;
                    const bool hasOutputArgs = outNames != nullptr && !outNames->empty();

                    const auto eventValue = graph.getValue(operands[eventStart]);
                    const std::string clockSymbol = eventValue.symbolText().empty() ? "clock" : std::string(eventValue.symbolText());
                    const std::string eventEdge = eventEdges != nullptr && !eventEdges->empty() && !(*eventEdges)[0].empty()
                                                    ? (*eventEdges)[0]
                                                    : std::string("posedge");
                    const std::string domainKey = eventEdge + ":" + clockSymbol;
                    const std::string clockExpr = getOperandExpr(eventStart);
                    if (clockExpr != "0" && !clockExpr.empty()) {
                        state.sequentialClockExprs.try_emplace(domainKey, clockExpr);
                    }

                    if (hasReturn && hasOutputArgs) {
                        const std::size_t returnOffset = 1U;
                        if (results.size() != returnOffset + outNames->size()) {
                            std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                            state.unsupportedOps.push_back("kDpicCall-result-count (" + opName + ")");
                            break;
                        }

                        std::vector<std::string> resultRefs(results.size());
                        std::vector<std::string> resultTypes(results.size());
                        std::vector<int32_t> resultWidths(results.size(), 0);
                        bool resultRefsOk = true;
                        for (std::size_t resultIndex = 0; resultIndex < results.size(); ++resultIndex) {
                            const auto resultValue = graph.getValue(results[resultIndex]);
                            if (resultValue.width() > 64) {
                                std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                                state.unsupportedOps.push_back("kDpicCall-materialized-wide (" + opName + ")");
                                resultRefsOk = false;
                                break;
                            }
                            resultWidths[resultIndex] = resultValue.width();
                            resultTypes[resultIndex] = getCppTypeForWidth(resultValue.width());
                            resultRefs[resultIndex] =
                                state.materializeResultRef(results[resultIndex], resultTypes[resultIndex], resultValue.width());
                        }
                        if (!resultRefsOk) {
                            break;
                        }

                        const std::size_t callSite = state.dpicMaterializedCallSite++;
                        const std::string structName = "WolvrixGsimDpicResult" + std::to_string(callSite);
                        const std::string resultName = "dpic_result_" + std::to_string(callSite) + "_";

                        std::vector<std::string> callArgs;
                        callArgs.reserve(formalCount);
                        bool materializedCallOk = true;
                        for (std::size_t formalIndex = 0; formalIndex < formalCount; ++formalIndex) {
                            if (importInfo.argDirs[formalIndex] == "input") {
                                const auto nameIt =
                                    std::find(inNames->begin(), inNames->end(), importInfo.argNames[formalIndex]);
                                if (nameIt == inNames->end()) {
                                    std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                                    state.unsupportedOps.push_back("kDpicCall-arg (" + opName + ")");
                                    materializedCallOk = false;
                                    break;
                                }
                                const std::size_t inputIndex =
                                    static_cast<std::size_t>(std::distance(inNames->begin(), nameIt));
                                const std::size_t operandIndex = 1U + inputIndex;
                                if (operandIndex >= eventStart) {
                                    std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                                    state.unsupportedOps.push_back("kDpicCall-arg-index (" + opName + ")");
                                    materializedCallOk = false;
                                    break;
                                }
                                callArgs.push_back(
                                    dpicCastExpr(getOperandExpr(operandIndex), importInfo.argWidths[formalIndex]));
                            } else if (importInfo.argDirs[formalIndex] == "output") {
                                const auto nameIt =
                                    std::find(outNames->begin(), outNames->end(), importInfo.argNames[formalIndex]);
                                if (nameIt == outNames->end()) {
                                    std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                                    state.unsupportedOps.push_back("kDpicCall-output-arg (" + opName + ")");
                                    materializedCallOk = false;
                                    break;
                                }
                                const std::size_t outputIndex =
                                    static_cast<std::size_t>(std::distance(outNames->begin(), nameIt));
                                callArgs.push_back("&result.out" + std::to_string(outputIndex));
                            } else {
                                std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                                state.unsupportedOps.push_back("kDpicCall-arg-dir (" + opName + ")");
                                materializedCallOk = false;
                                break;
                            }
                        }
                        if (!materializedCallOk) {
                            break;
                        }

                        state.markDpicPreSettleValue(operands[0]);
                        state.markDirtyReplayProducerValue(operands[0]);
                        for (std::size_t operandIndex = 1U; operandIndex < eventStart; ++operandIndex) {
                            state.markDpicPreSettleValue(operands[operandIndex]);
                            state.markDirtyReplayProducerValue(operands[operandIndex]);
                        }

                        std::string callExpr = *target + "(";
                        for (std::size_t i = 0; i < callArgs.size(); ++i) {
                            if (i != 0) {
                                callExpr += ", ";
                            }
                            callExpr += callArgs[i];
                        }
                        callExpr += ")";

                        std::string stmt = "        { struct " + structName + " { " + resultTypes[0] + " ret{};";
                        for (std::size_t outputIndex = 0; outputIndex < outNames->size(); ++outputIndex) {
                            const std::size_t resultIndex = returnOffset + outputIndex;
                            stmt += " " + resultTypes[resultIndex] + " out" + std::to_string(outputIndex) + "{};";
                        }
                        stmt += " }; const auto " + resultName + " = ([&](){ " + structName + " result{}; ";
                        stmt += guardNoDiffDpicBlock("if (" + condition + ") { result.ret = " +
                                                     castScalarExprForWidth(callExpr, resultWidths[0]) +
                                                     "; committed_ = true; }",
                                                     *target);
                        stmt += " return result; }()); ";
                        stmt += resultRefs[0] + " = " + resultName + ".ret;";
                        for (std::size_t outputIndex = 0; outputIndex < outNames->size(); ++outputIndex) {
                            const std::size_t resultIndex = returnOffset + outputIndex;
                            stmt += " " + resultRefs[resultIndex] + " = " + resultName + ".out" +
                                    std::to_string(outputIndex) + ";";
                        }
                        stmt += " }";
                        state.sequentialPreStmts[domainKey].push_back(std::move(stmt));
                        for (std::size_t resultIndex = 0; resultIndex < results.size(); ++resultIndex) {
                            recordResultMetadata(resultIndex);
                        }
                        recordDpicCall(state, *target);
                        break;
                    }

                    if (!hasReturn && hasOutputArgs && outNames->size() == 1U && results.size() == 1U) {
                        const auto resultValue = graph.getValue(results[0]);
                        if (resultValue.width() > 64) {
                            std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                            state.unsupportedOps.push_back("kDpicCall-output-wide (" + opName + ")");
                            break;
                        }
                        const std::string outType = getCppTypeForWidth(resultValue.width());
                        const std::string outName = (*outNames)[0];
                        std::vector<std::string> lambdaArgs;
                        lambdaArgs.reserve(formalCount);
                        bool outputCallOk = true;
                        for (std::size_t formalIndex = 0; formalIndex < formalCount; ++formalIndex) {
                            if (importInfo.argDirs[formalIndex] == "input") {
                                const auto nameIt =
                                    std::find(inNames->begin(), inNames->end(), importInfo.argNames[formalIndex]);
                                if (nameIt == inNames->end()) {
                                    std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                                    state.unsupportedOps.push_back("kDpicCall-arg (" + opName + ")");
                                    outputCallOk = false;
                                    break;
                                }
                                const std::size_t inputIndex =
                                    static_cast<std::size_t>(std::distance(inNames->begin(), nameIt));
                                const std::size_t operandIndex = 1U + inputIndex;
                                if (operandIndex >= eventStart) {
                                    std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                                    state.unsupportedOps.push_back("kDpicCall-arg-index (" + opName + ")");
                                    outputCallOk = false;
                                    break;
                                }
                                lambdaArgs.push_back(
                                    dpicCastExpr(getOperandExpr(operandIndex), importInfo.argWidths[formalIndex]));
                            } else if (importInfo.argDirs[formalIndex] == "output" &&
                                       importInfo.argNames[formalIndex] == outName) {
                                lambdaArgs.push_back("&dpic_out_");
                            } else {
                                std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                                state.unsupportedOps.push_back("kDpicCall-output (" + opName + ")");
                                outputCallOk = false;
                                break;
                            }
                        }
                        if (!outputCallOk) {
                            break;
                        }
                        state.markDpicPreSettleValue(operands[0]);
                        state.markDirtyReplayProducerValue(operands[0]);
                        for (std::size_t operandIndex = 1U; operandIndex < eventStart; ++operandIndex) {
                            state.markDpicPreSettleValue(operands[operandIndex]);
                            state.markDirtyReplayProducerValue(operands[operandIndex]);
                        }
                        std::string callExpr = *target + "(";
                        for (std::size_t i = 0; i < lambdaArgs.size(); ++i) {
                            if (i != 0) {
                                callExpr += ", ";
                            }
                            callExpr += lambdaArgs[i];
                        }
                        callExpr += ")";
                        std::string lambdaExpr = "([&](){ " + outType + " dpic_out_ = " +
                                                 zeroInitializerForWidth(resultValue.width()) + "; ";
                        lambdaExpr += guardNoDiffDpicBlock("if (" + condition + ") { " + callExpr + "; }", *target);
                        lambdaExpr += " return dpic_out_; }())";
                        setResultExpr(0, lambdaExpr);
                        recordDpicCall(state, *target);
                        break;
                    }

                    std::vector<std::string> args;
                    args.reserve(formalCount);
                    bool callOk = true;
                    for (std::size_t formalIndex = 0; formalIndex < formalCount; ++formalIndex) {
                        if (importInfo.argDirs[formalIndex] != "input") {
                            std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                            state.unsupportedOps.push_back("kDpicCall-noninput (" + opName + ")");
                            callOk = false;
                            break;
                        }
                        const auto nameIt = std::find(inNames->begin(), inNames->end(), importInfo.argNames[formalIndex]);
                        if (nameIt == inNames->end()) {
                            std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                            state.unsupportedOps.push_back("kDpicCall-arg (" + opName + ")");
                            callOk = false;
                            break;
                        }
                        const std::size_t inputIndex = static_cast<std::size_t>(std::distance(inNames->begin(), nameIt));
                        const std::size_t operandIndex = 1U + inputIndex;
                        if (operandIndex >= eventStart) {
                            std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                            state.unsupportedOps.push_back("kDpicCall-arg-index (" + opName + ")");
                            callOk = false;
                            break;
                        }
                        args.push_back(dpicCastExpr(getOperandExpr(operandIndex), importInfo.argWidths[formalIndex]));
                    }
                    if (!callOk) {
                        break;
                    }
                    state.markDpicPreSettleValue(operands[0]);
                    state.markDirtyReplayProducerValue(operands[0]);
                    for (std::size_t operandIndex = 1U; operandIndex < eventStart; ++operandIndex) {
                        state.markDpicPreSettleValue(operands[operandIndex]);
                        state.markDirtyReplayProducerValue(operands[operandIndex]);
                    }
                    if (hasReturn && !hasOutputArgs) {
                        if (results.empty()) {
                            std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                            state.unsupportedOps.push_back("kDpicCall-return-result (" + opName + ")");
                            break;
                        }
                        const auto resultValue = graph.getValue(results[0]);
                        if (resultValue.width() > 64) {
                            std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                            state.unsupportedOps.push_back("kDpicCall-return-wide (" + opName + ")");
                            break;
                        }
                        const std::string returnType = getCppTypeForWidth(resultValue.width());
                        std::string callExpr = *target + "(";
                        for (std::size_t i = 0; i < args.size(); ++i) {
                            if (i != 0) {
                                callExpr += ", ";
                            }
                            callExpr += args[i];
                        }
                        callExpr += ")";
                        std::string lambdaExpr = "([&](){ " + returnType + " dpic_return_ = " +
                                                 zeroInitializerForWidth(resultValue.width()) + "; ";
                        lambdaExpr += guardNoDiffDpicBlock("if (" + condition + ") { dpic_return_ = " +
                                                          castScalarExprForWidth(callExpr, resultValue.width()) +
                                                          "; }",
                                                          *target);
                        lambdaExpr += " return dpic_return_; }())";
                        state.valueVars[results[0]] = std::move(lambdaExpr);
                        recordResultMetadata(0);
                        recordDpicCall(state, *target);
                        break;
                    }
                    if (hasOutputArgs) {
                        std::string opName = op.symbolText().empty() ? "unnamed" : std::string(op.symbolText());
                        state.unsupportedOps.push_back("kDpicCall-output (" + opName + ")");
                        break;
                    }

                    std::string stmt;
                    if (state.enableDpicTrace) {
                        const std::size_t callSite = state.dpicTraceCallSite++;
                        const std::string condName = "dpic_cond_" + std::to_string(callSite) + "_";
                        const std::string seenName = "dpic_seen_" + std::to_string(callSite) + "_";
                        const std::string hitsName = "dpic_hits_" + std::to_string(callSite) + "_";
                        const std::string missesName = "dpic_misses_" + std::to_string(callSite) + "_";
                        const bool traceFalseSamples = true;
                        auto appendRound36XsZeroRetireTrace = [&]() {
                            if (!state.enableXsZeroRetireTrace) {
                                return;
                            }
                            if (*target != "v_difftest_InstrCommit") {
                                return;
                            }
                            if (state.tempU8Count <= 2516282U || state.stateU8Count <= 236509U) {
                                return;
                            }
                            auto appendTempU8 = [&](std::string_view label, std::size_t index) {
                                stmt += " << \" r36_" + std::string(label) +
                                        "=\" << static_cast<std::uint64_t>(evalTemps_->tempU8[" +
                                        std::to_string(index) + "])";
                            };
                            auto appendStateU8 = [&](std::string_view label, std::size_t index) {
                                stmt += " << \" r36_" + std::string(label) +
                                        "=\" << static_cast<std::uint64_t>(state_->stateU8[" +
                                        std::to_string(index) + "])";
                            };
                            appendTempU8("t1903395", 1903395);
                            appendTempU8("t2516282", 2516282);
                            appendStateU8("s78873", 78873);
                            appendStateU8("hc0", 92257);
                            appendStateU8("hc1", 92258);
                            appendStateU8("hc2", 92259);
                            appendStateU8("hc3", 92260);
                            appendStateU8("hc4", 92261);
                            appendStateU8("deq0_v", 92274);
                            appendStateU8("deq0_w", 92275);
                            appendStateU8("deq1_v", 92304);
                            appendStateU8("deq1_w", 92305);
                            appendStateU8("redirectValid", 93751);
                            appendStateU8("redirectAll", 93752);
                            appendStateU8("flushLast", 77454);
                            appendStateU8("endpointValid", 236509);
                        };
                        stmt = "        { const bool " + condName + " = static_cast<bool>(" + condition + "); ";
                        stmt += "static bool " + seenName + " = false; static unsigned " + hitsName + " = 0; ";
                        if (traceFalseSamples) {
                            stmt += "static unsigned " + missesName + " = 0; ";
                        }
                        stmt += "if (!" + seenName + ") { std::cerr << \"[wolvrix-gsim-dpic] site=" + std::to_string(callSite) + " target=" + *target + " first_cond=\" << " + condName;
                        for (std::size_t i = 0; i < args.size(); ++i) {
                            stmt += " << \" arg" + std::to_string(i) + "=\" << static_cast<std::uint64_t>(" + args[i] + ")";
                        }
                        appendRound36XsZeroRetireTrace();
                        stmt += " << \"\\n\"; " + seenName + " = true; } ";
                        if (traceFalseSamples) {
                            stmt += "if (!" + condName + ") { ++" + missesName + "; if (" + missesName + " <= 16U || (" + missesName + " % 1024U) == 0U) { std::cerr << \"[wolvrix-gsim-dpic] site=" + std::to_string(callSite) + " target=" + *target + " miss=\" << " + missesName;
                            for (std::size_t i = 0; i < args.size(); ++i) {
                                stmt += " << \" arg" + std::to_string(i) + "=\" << static_cast<std::uint64_t>(" + args[i] + ")";
                            }
                            appendRound36XsZeroRetireTrace();
                            stmt += " << \"\\n\"; } } ";
                        }
                        stmt += "if (" + condName + ") { ++" + hitsName + "; if (" + hitsName + " <= 16U) { std::cerr << \"[wolvrix-gsim-dpic] site=" + std::to_string(callSite) + " target=" + *target + " hit=\" << " + hitsName;
                        for (std::size_t i = 0; i < args.size(); ++i) {
                            stmt += " << \" arg" + std::to_string(i) + "=\" << static_cast<std::uint64_t>(" + args[i] + ")";
                        }
                        appendRound36XsZeroRetireTrace();
                        stmt += " << \"\\n\"; } " + *target + "(";
                    } else {
                        stmt = "        if (" + condition + ") { " + *target + "(";
                    }
                    for (std::size_t i = 0; i < args.size(); ++i) {
                        if (i != 0) {
                            stmt += ", ";
                        }
                        stmt += args[i];
                    }
                    stmt += "); committed_ = true; }";
                    if (state.enableDpicTrace) {
                        stmt += " }";
                    }
                    recordDpicCall(state, *target);
                    state.sequentialStmts[domainKey].push_back(guardNoDiffDpicStatement(std::move(stmt), *target));
                    state.sequentialStmtDirtyOnCommit[domainKey].push_back(false);
                    state.sequentialStmtActivitySources[domainKey].push_back({});
                    break;
                }

                case OperationKind::kSystemTask:
                case OperationKind::kSystemFunction: {
                    // System tasks/functions are debug/diagnostic constructs and do not
                    // generate simulation logic in the emitted C++ runtime.
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
            const std::vector<std::string>& customOrder = {},
            const std::vector<std::string>& outputKeepPrefixes = {})
        {
            for (const auto& port : graph.inputPorts()) {
                auto value = graph.getValue(port.value);
                std::string type = getCppTypeForWidth(value.width());
                state.inputPorts.push_back({port.name, type});
                state.inputPortWidths.emplace(port.name, value.width());
                // Seed input port values into CodegenState as variable names
                std::string portVar = "input_" + sanitizeIdentifier(port.name) + "_";
                state.valueVars[port.value] = portVar;
            }

            for (const auto& port : graph.outputPorts()) {
                if (!outputKeepPrefixes.empty()) {
                    bool keep = false;
                    for (const auto& prefix : outputKeepPrefixes) {
                        if (port.name.rfind(prefix, 0) == 0) {
                            keep = true;
                            break;
                        }
                    }
                    if (!keep) {
                        continue;
                    }
                }
                auto value = graph.getValue(port.value);
                std::string type = getCppTypeForWidth(value.width());
                state.outputPorts.push_back({port.name, type});
                state.outputPortWidths.emplace(port.name, value.width());
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

                    int32_t width = 32; // Default width
                    if (auto widthAttr = op.attr("width")) {
                        if (auto *attrWidth = std::get_if<int64_t>(&*widthAttr)) {
                            width = static_cast<int32_t>(*attrWidth);
                        }
                    }
                    if (!op.results().empty()) {
                        auto val = graph.getValue(op.results()[0]);
                        if (val.width() > 0) {
                            width = val.width();
                        }
                    }
                    if (state.storageWidths.find(regName) != state.storageWidths.end()) {
                        if (!op.results().empty()) {
                            state.valueVars[op.results()[0]] = state.persistentStorageExpr(regName);
                        }
                        continue;
                    }
                    std::string type = getCppTypeForWidth(width);
                    const std::string regExpr = state.allocatePersistentStorage(regName, type, width);
                    state.storageWidths[regName] = width;

                    // Also create a mapping from the register's result ValueId to the register name
                    if (!op.results().empty()) {
                        state.valueVars[op.results()[0]] = regExpr;
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
                    if (state.storageWidths.find(latchName) != state.storageWidths.end()) {
                        if (!op.results().empty()) {
                            state.valueVars[op.results()[0]] = state.persistentStorageExpr(latchName);
                        }
                        continue;
                    }
                    const std::string latchExpr = state.allocatePersistentStorage(latchName, type, width);
                    state.storageWidths[latchName] = width;
                    if (!op.results().empty()) {
                        state.valueVars[op.results()[0]] = latchExpr;
                    }
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
                        state.storageResetStmts.push_back("state_->" + memory.storageName + " = " + fillExpr + ";");
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
                            "state_->" + memory.storageName + "[" + std::to_string(clampedStart) + "] = " + valueExpr + ";");
                        continue;
                    }
                    state.storageResetStmts.push_back(
                        "for (std::size_t __mem_idx = " + std::to_string(clampedStart) + "; __mem_idx < " +
                        std::to_string(clampedEnd) + "; ++__mem_idx) { state_->" + memory.storageName +
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

        std::vector<std::string> splitCsv(std::string_view text)
        {
            std::vector<std::string> out;
            std::size_t start = 0;
            while (start < text.size())
            {
                const std::size_t comma = text.find(',', start);
                const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
                std::string item(text.substr(start, end - start));
                item.erase(item.begin(), std::find_if(item.begin(), item.end(), [](unsigned char ch) { return !std::isspace(ch); }));
                item.erase(std::find_if(item.rbegin(), item.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), item.end());
                if (!item.empty())
                {
                    out.push_back(std::move(item));
                }
                if (comma == std::string_view::npos)
                {
                    break;
                }
                start = comma + 1;
            }
            return out;
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

        std::vector<SequentialChunkPlan> buildSequentialChunkPlans(const CodegenState& state)
        {
            std::vector<SequentialChunkPlan> plans;
            const std::size_t maxChunkBytes = static_cast<std::size_t>(std::max(32768, state.commitShardSize));
            std::set<std::string> domains;
            for (const auto& [domainKey, _] : state.sequentialPreStmts) {
                domains.insert(domainKey);
            }
            for (const auto& [domainKey, _] : state.sequentialRegStmts) {
                domains.insert(domainKey);
            }
            for (const auto& [domainKey, _] : state.sequentialStmts) {
                domains.insert(domainKey);
            }
            for (const auto& domainKey : domains) {
                std::size_t chunkIndex = 0;
                if (auto preIt = state.sequentialPreStmts.find(domainKey); preIt != state.sequentialPreStmts.end()) {
                    SequentialChunkPlan current;
                    current.domainKey = domainKey;
                    current.preReg = true;
                    std::size_t currentBytes = 0;
                    auto flushCurrent = [&]() {
                        if (current.stmts.empty()) {
                            return;
                        }
                        current.methodName = "commit_chunk_" + sanitizeIdentifier(domainKey) + "_" + std::to_string(chunkIndex++);
                        plans.push_back(current);
                        current = SequentialChunkPlan{};
                        current.domainKey = domainKey;
                        current.preReg = true;
                        currentBytes = 0;
                    };
                    for (const auto& stmt : preIt->second) {
                        const std::size_t estimatedBytes = stmt.size() + 1;
                        if (!current.stmts.empty() && currentBytes + estimatedBytes > maxChunkBytes) {
                            flushCurrent();
                        }
                        current.stmts.push_back(stmt);
                        currentBytes += estimatedBytes;
                    }
                    flushCurrent();
                }
                if (auto regIt = state.sequentialRegStmts.find(domainKey); regIt != state.sequentialRegStmts.end()) {
                    SequentialChunkPlan current;
                    current.domainKey = domainKey;
                    std::size_t currentBytes = 0;
                    auto flushCurrent = [&]() {
                        if (current.regNames.empty() && current.stmts.empty()) {
                            return;
                        }
                        current.methodName = "commit_chunk_" + sanitizeIdentifier(domainKey) + "_" + std::to_string(chunkIndex++);
                        plans.push_back(current);
                        current = SequentialChunkPlan{};
                        current.domainKey = domainKey;
                        currentBytes = 0;
                    };
                    for (const auto& [regName, stmts] : regIt->second) {
                        std::size_t estimatedBytes = regName.size() * 2 + 64;
                        for (const auto& stmt : stmts) {
                            estimatedBytes += stmt.size() + 1;
                        }
                        if ((!current.regNames.empty() || !current.stmts.empty()) &&
                            currentBytes + estimatedBytes > maxChunkBytes) {
                            flushCurrent();
                        }
                        current.regNames.push_back(regName);
                        current.regStmts[regName] = stmts;
                        currentBytes += estimatedBytes;
                    }
                    flushCurrent();
                }
                if (auto stmtIt = state.sequentialStmts.find(domainKey); stmtIt != state.sequentialStmts.end()) {
                    const auto dirtyIt = state.sequentialStmtDirtyOnCommit.find(domainKey);
                    const std::vector<bool>* dirtyOnCommit =
                        dirtyIt != state.sequentialStmtDirtyOnCommit.end() ? &dirtyIt->second : nullptr;
                    const auto activityIt = state.sequentialStmtActivitySources.find(domainKey);
                    const std::vector<std::vector<std::string>>* stmtActivitySources =
                        activityIt != state.sequentialStmtActivitySources.end() ? &activityIt->second : nullptr;
                    SequentialChunkPlan current;
                    current.domainKey = domainKey;
                    std::size_t currentBytes = 0;
                    auto flushCurrent = [&]() {
                        if (current.stmts.empty()) {
                            return;
                        }
                        current.methodName = "commit_chunk_" + sanitizeIdentifier(domainKey) + "_" + std::to_string(chunkIndex++);
                        plans.push_back(current);
                        current = SequentialChunkPlan{};
                        current.domainKey = domainKey;
                        currentBytes = 0;
                    };
                    for (std::size_t stmtIndex = 0; stmtIndex < stmtIt->second.size(); ++stmtIndex) {
                        const auto& stmt = stmtIt->second[stmtIndex];
                        const std::size_t estimatedBytes = stmt.size() + 1;
                        if (!current.stmts.empty() && currentBytes + estimatedBytes > maxChunkBytes) {
                            flushCurrent();
                        }
                        current.stmts.push_back(stmt);
                        current.stmtDirtyOnCommit.push_back(
                            dirtyOnCommit == nullptr || stmtIndex >= dirtyOnCommit->size() || (*dirtyOnCommit)[stmtIndex]);
                        if (stmtActivitySources != nullptr && stmtIndex < stmtActivitySources->size()) {
                            current.stmtActivitySources.push_back((*stmtActivitySources)[stmtIndex]);
                        } else {
                            current.stmtActivitySources.push_back({});
                        }
                        currentBytes += estimatedBytes;
                    }
                    flushCurrent();
                }
            }
            return plans;
        }

        void writeRuntimeHelpers(std::ostream &os)
        {
            os << "#include <algorithm>\n";
            os << "#include <cstdint>\n";
            os << "#include <cstddef>\n";
            os << "#include <map>\n";
            os << "#include <memory>\n";
            os << "#include <stdexcept>\n";
            os << "#include <string>\n";
            os << "#include <type_traits>\n";
            os << "#include <vector>\n\n";
            os << "inline std::uint64_t wolvrix_gsim_low_mask(std::uint32_t width) {\n";
            os << "    return width >= 64U ? ~0ULL : ((1ULL << width) - 1ULL);\n";
            os << "}\n";
            os << "inline const std::vector<std::uint64_t>& wolvrix_gsim_zero_bits(std::size_t wordCount) {\n";
            os << "    static const std::vector<std::uint64_t> empty;\n";
            os << "    static const std::vector<std::uint64_t> one(1U, 0ULL);\n";
            os << "    static const std::vector<std::uint64_t> two(2U, 0ULL);\n";
            os << "    static const std::vector<std::uint64_t> three(3U, 0ULL);\n";
            os << "    static const std::vector<std::uint64_t> four(4U, 0ULL);\n";
            os << "    static const std::vector<std::uint64_t> eight(8U, 0ULL);\n";
            os << "    switch (wordCount) {\n";
            os << "    case 0U: return empty;\n";
            os << "    case 1U: return one;\n";
            os << "    case 2U: return two;\n";
            os << "    case 3U: return three;\n";
            os << "    case 4U: return four;\n";
            os << "    case 8U: return eight;\n";
            os << "    default: {\n";
            os << "        static thread_local std::vector<std::uint64_t> scratch;\n";
            os << "        scratch.assign(wordCount, 0ULL);\n";
            os << "        return scratch;\n";
            os << "    }\n";
            os << "    }\n";
            os << "}\n";
            os << "inline std::uint64_t wolvrix_gsim_load_shifted_word(\n";
            os << "    const std::vector<std::uint64_t>& value,\n";
            os << "    std::uint64_t bitIndex) {\n";
            os << "    const auto wordIndex = static_cast<std::size_t>(bitIndex / 64ULL);\n";
            os << "    const auto bitOffset = static_cast<std::uint32_t>(bitIndex % 64ULL);\n";
            os << "    std::uint64_t result = wordIndex < value.size() ? value[wordIndex] >> bitOffset : 0ULL;\n";
            os << "    if (bitOffset != 0U && wordIndex + 1U < value.size()) {\n";
            os << "        result |= value[wordIndex + 1U] << (64U - bitOffset);\n";
            os << "    }\n";
            os << "    return result;\n";
            os << "}\n";
            os << "inline std::uint64_t wolvrix_gsim_slice_dynamic_to_u64(\n";
            os << "    const std::vector<std::uint64_t>& value,\n";
            os << "    std::uint64_t bitIndex,\n";
            os << "    std::uint32_t sliceWidth,\n";
            os << "    std::uint32_t operandWidth) {\n";
            os << "    if (sliceWidth == 0 || sliceWidth > 64 || bitIndex >= operandWidth) {\n";
            os << "        return 0ULL;\n";
            os << "    }\n";
            os << "    const auto availableBits = static_cast<std::uint32_t>(std::min<std::uint64_t>(sliceWidth, operandWidth - bitIndex));\n";
            os << "    return wolvrix_gsim_load_shifted_word(value, bitIndex) & wolvrix_gsim_low_mask(availableBits);\n";
            os << "}\n\n";
            os << "inline std::vector<std::uint64_t> wolvrix_gsim_slice_dynamic_to_bits(\n";
            os << "    const std::vector<std::uint64_t>& value,\n";
            os << "    std::uint64_t bitIndex,\n";
            os << "    std::uint32_t sliceWidth,\n";
            os << "    std::uint32_t operandWidth) {\n";
            os << "    std::vector<std::uint64_t> result((sliceWidth + 63U) / 64U, 0ULL);\n";
            os << "    if (sliceWidth == 0 || bitIndex >= operandWidth) {\n";
            os << "        return result;\n";
            os << "    }\n";
            os << "    const auto availableBits = static_cast<std::uint32_t>(std::min<std::uint64_t>(sliceWidth, operandWidth - bitIndex));\n";
            os << "    const auto availableWords = static_cast<std::size_t>((availableBits + 63U) / 64U);\n";
            os << "    for (std::size_t i = 0; i < availableWords; ++i) {\n";
            os << "        result[i] = wolvrix_gsim_load_shifted_word(value, bitIndex + static_cast<std::uint64_t>(i) * 64ULL);\n";
            os << "    }\n";
            os << "    if ((availableBits % 64U) != 0U && availableWords != 0U) {\n";
            os << "        result[availableWords - 1U] &= wolvrix_gsim_low_mask(availableBits % 64U);\n";
            os << "    }\n";
            os << "    return result;\n";
            os << "}\n\n";
            os << "inline void wolvrix_gsim_slice_dynamic_to_bits_into(\n";
            os << "    std::vector<std::uint64_t>& result,\n";
            os << "    const std::vector<std::uint64_t>& value,\n";
            os << "    std::uint64_t bitIndex,\n";
            os << "    std::uint32_t sliceWidth,\n";
            os << "    std::uint32_t operandWidth) {\n";
            os << "    const auto requiredWords = static_cast<std::size_t>((sliceWidth + 63U) / 64U);\n";
            os << "    if (result.size() != requiredWords) { result.assign(requiredWords, 0ULL); }\n";
            os << "    else { std::fill(result.begin(), result.end(), 0ULL); }\n";
            os << "    if (sliceWidth == 0 || bitIndex >= operandWidth) { return; }\n";
            os << "    const auto availableBits = static_cast<std::uint32_t>(std::min<std::uint64_t>(sliceWidth, operandWidth - bitIndex));\n";
            os << "    const auto availableWords = static_cast<std::size_t>((availableBits + 63U) / 64U);\n";
            os << "    for (std::size_t i = 0; i < availableWords; ++i) {\n";
            os << "        result[i] = wolvrix_gsim_load_shifted_word(value, bitIndex + static_cast<std::uint64_t>(i) * 64ULL);\n";
            os << "    }\n";
            os << "    if ((availableBits % 64U) != 0U && availableWords != 0U) {\n";
            os << "        result[availableWords - 1U] &= wolvrix_gsim_low_mask(availableBits % 64U);\n";
            os << "    }\n";
            os << "}\n\n";
            os << "inline unsigned wolvrix_gsim_reduce_or(const std::vector<std::uint64_t>& value, std::uint32_t width) {\n";
            os << "    const auto wordCount = static_cast<std::size_t>((width + 63U) / 64U);\n";
            os << "    for (std::size_t i = 0; i < wordCount; ++i) {\n";
            os << "        std::uint64_t word = i < value.size() ? value[i] : 0ULL;\n";
            os << "        if (i + 1 == wordCount && (width % 64U) != 0U) {\n";
            os << "            word &= ((1ULL << (width % 64U)) - 1ULL);\n";
            os << "        }\n";
            os << "        if (word != 0ULL) return 1U;\n";
            os << "    }\n";
            os << "    return 0U;\n";
            os << "}\n";
            os << "inline unsigned wolvrix_gsim_reduce_and(const std::vector<std::uint64_t>& value, std::uint32_t width) {\n";
            os << "    if (width == 0U) return 0U;\n";
            os << "    const auto wordCount = static_cast<std::size_t>((width + 63U) / 64U);\n";
            os << "    for (std::size_t i = 0; i < wordCount; ++i) {\n";
            os << "        const std::uint64_t expected = (i + 1 == wordCount && (width % 64U) != 0U)\n";
            os << "            ? ((1ULL << (width % 64U)) - 1ULL)\n";
            os << "            : ~0ULL;\n";
            os << "        const std::uint64_t word = i < value.size() ? value[i] : 0ULL;\n";
            os << "        if ((word & expected) != expected) return 0U;\n";
            os << "    }\n";
            os << "    return 1U;\n";
            os << "}\n";
            os << "inline unsigned wolvrix_gsim_reduce_xor(const std::vector<std::uint64_t>& value, std::uint32_t width) {\n";
            os << "    const auto wordCount = static_cast<std::size_t>((width + 63U) / 64U);\n";
            os << "    unsigned parity = 0U;\n";
            os << "    for (std::size_t i = 0; i < wordCount; ++i) {\n";
            os << "        std::uint64_t word = i < value.size() ? value[i] : 0ULL;\n";
            os << "        if (i + 1 == wordCount && (width % 64U) != 0U) {\n";
            os << "            word &= ((1ULL << (width % 64U)) - 1ULL);\n";
            os << "        }\n";
            os << "        parity ^= static_cast<unsigned>(__builtin_parityll(word));\n";
            os << "    }\n";
            os << "    return parity & 1U;\n";
            os << "}\n";
            os << "inline std::vector<std::uint64_t> wolvrix_gsim_mask_merge(\n";
            os << "    const std::vector<std::uint64_t>& current,\n";
            os << "    const std::vector<std::uint64_t>& next,\n";
            os << "    const std::vector<std::uint64_t>& mask) {\n";
            os << "    const auto wordCount = std::max(current.size(), std::max(next.size(), mask.size()));\n";
            os << "    std::vector<std::uint64_t> result(wordCount, 0ULL);\n";
            os << "    for (std::size_t i = 0; i < wordCount; ++i) {\n";
            os << "        const std::uint64_t currWord = i < current.size() ? current[i] : 0ULL;\n";
            os << "        const std::uint64_t nextWord = i < next.size() ? next[i] : 0ULL;\n";
            os << "        const std::uint64_t maskWord = i < mask.size() ? mask[i] : 0ULL;\n";
            os << "        result[i] = (currWord & ~maskWord) | (nextWord & maskWord);\n";
            os << "    }\n";
            os << "    return result;\n";
            os << "}\n";
            os << "inline bool wolvrix_gsim_mask_merge_in_place(\n";
            os << "    std::vector<std::uint64_t>& current,\n";
            os << "    const std::vector<std::uint64_t>& next,\n";
            os << "    const std::vector<std::uint64_t>& mask) {\n";
            os << "    const auto wordCount = std::max(current.size(), std::max(next.size(), mask.size()));\n";
            os << "    const auto oldSize = current.size();\n";
            os << "    bool changed = oldSize != wordCount;\n";
            os << "    if (current.size() < wordCount) current.resize(wordCount, 0ULL);\n";
            os << "    for (std::size_t i = 0; i < wordCount; ++i) {\n";
            os << "        const std::uint64_t currWord = current[i];\n";
            os << "        const std::uint64_t nextWord = i < next.size() ? next[i] : 0ULL;\n";
            os << "        const std::uint64_t maskWord = i < mask.size() ? mask[i] : 0ULL;\n";
            os << "        const std::uint64_t merged = (currWord & ~maskWord) | (nextWord & maskWord);\n";
            os << "        if (current[i] != merged) { current[i] = merged; changed = true; }\n";
            os << "    }\n";
            os << "    return changed;\n";
            os << "}\n\n";
            os << "template <typename T>\n";
            os << "inline void wolvrix_gsim_store_bits(std::vector<std::uint64_t>& out, std::uint64_t bitOffset, const T& value, std::uint32_t width);\n";
            os << "inline void wolvrix_gsim_store_bits(std::vector<std::uint64_t>& out, std::uint64_t bitOffset, const std::vector<std::uint64_t>& value, std::uint32_t width);\n";
            os << "template <typename T>\n";
            os << "inline std::uint32_t wolvrix_gsim_helper_width(const T& value) {\n";
            os << "    if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {\n";
            os << "        return 64U;\n";
            os << "    } else {\n";
            os << "        return static_cast<std::uint32_t>(value.size() * 64U);\n";
            os << "    }\n";
            os << "}\n";
            os << "template <typename T>\n";
            os << "inline std::uint64_t wolvrix_gsim_word_at(const T& value, std::size_t index) {\n";
            os << "    if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {\n";
            os << "        return index == 0U ? static_cast<std::uint64_t>(value) : 0ULL;\n";
            os << "    } else {\n";
            os << "        return index < value.size() ? value[index] : 0ULL;\n";
            os << "    }\n";
            os << "}\n";
            os << "template <typename L, typename R, typename Op>\n";
            os << "inline void wolvrix_gsim_bitwise_into(\n";
            os << "    std::vector<std::uint64_t>& out,\n";
            os << "    const L& lhs,\n";
            os << "    const R& rhs,\n";
            os << "    std::uint32_t width,\n";
            os << "    Op op) {\n";
            os << "    const auto wordCount = static_cast<std::size_t>((width + 63U) / 64U);\n";
            os << "    if (out.size() != wordCount) { out.assign(wordCount, 0ULL); }\n";
            os << "    for (std::size_t i = 0; i < wordCount; ++i) {\n";
            os << "        out[i] = op(wolvrix_gsim_word_at(lhs, i), wolvrix_gsim_word_at(rhs, i));\n";
            os << "    }\n";
            os << "    if ((width % 64U) != 0U && !out.empty()) {\n";
            os << "        out.back() &= wolvrix_gsim_low_mask(width % 64U);\n";
            os << "    }\n";
            os << "}\n";
            os << "template <typename L, typename R>\n";
            os << "inline void wolvrix_gsim_bitwise_and_into(std::vector<std::uint64_t>& out, const L& lhs, const R& rhs, std::uint32_t width) {\n";
            os << "    wolvrix_gsim_bitwise_into(out, lhs, rhs, width, [](std::uint64_t a, std::uint64_t b) { return a & b; });\n";
            os << "}\n";
            os << "template <typename L, typename R>\n";
            os << "inline void wolvrix_gsim_bitwise_or_into(std::vector<std::uint64_t>& out, const L& lhs, const R& rhs, std::uint32_t width) {\n";
            os << "    wolvrix_gsim_bitwise_into(out, lhs, rhs, width, [](std::uint64_t a, std::uint64_t b) { return a | b; });\n";
            os << "}\n";
            os << "template <typename L, typename R>\n";
            os << "inline void wolvrix_gsim_bitwise_xor_into(std::vector<std::uint64_t>& out, const L& lhs, const R& rhs, std::uint32_t width) {\n";
            os << "    wolvrix_gsim_bitwise_into(out, lhs, rhs, width, [](std::uint64_t a, std::uint64_t b) { return a ^ b; });\n";
            os << "}\n";
            os << "template <typename T>\n";
            os << "inline void wolvrix_gsim_bitwise_not_into(std::vector<std::uint64_t>& out, const T& value, std::uint32_t width) {\n";
            os << "    const auto wordCount = static_cast<std::size_t>((width + 63U) / 64U);\n";
            os << "    if (out.size() != wordCount) { out.assign(wordCount, 0ULL); }\n";
            os << "    for (std::size_t i = 0; i < wordCount; ++i) { out[i] = ~wolvrix_gsim_word_at(value, i); }\n";
            os << "    if ((width % 64U) != 0U && !out.empty()) { out.back() &= wolvrix_gsim_low_mask(width % 64U); }\n";
            os << "}\n";
            os << "template <typename T>\n";
            os << "inline std::vector<std::uint64_t> wolvrix_gsim_to_bits(const T& value, std::uint32_t width) {\n";
            os << "    std::vector<std::uint64_t> result((width + 63U) / 64U, 0ULL);\n";
            os << "    wolvrix_gsim_store_bits(result, 0ULL, value, width);\n";
            os << "    return result;\n";
            os << "}\n";
            os << "template <typename L, typename R>\n";
            os << "inline std::vector<std::uint64_t> wolvrix_gsim_bitwise_and(\n";
            os << "    const L& lhs,\n";
            os << "    const R& rhs) {\n";
            os << "    const std::uint32_t lhsWidth = wolvrix_gsim_helper_width(lhs);\n";
            os << "    const std::uint32_t rhsWidth = wolvrix_gsim_helper_width(rhs);\n";
            os << "    const auto lhsBits = wolvrix_gsim_to_bits(lhs, lhsWidth);\n";
            os << "    const auto rhsBits = wolvrix_gsim_to_bits(rhs, rhsWidth);\n";
            os << "    const auto wordCount = std::max(lhsBits.size(), rhsBits.size());\n";
            os << "    std::vector<std::uint64_t> result(wordCount, 0ULL);\n";
            os << "    for (std::size_t i = 0; i < wordCount; ++i) {\n";
            os << "        const std::uint64_t lhsWord = i < lhsBits.size() ? lhsBits[i] : 0ULL;\n";
            os << "        const std::uint64_t rhsWord = i < rhsBits.size() ? rhsBits[i] : 0ULL;\n";
            os << "        result[i] = lhsWord & rhsWord;\n";
            os << "    }\n";
            os << "    return result;\n";
            os << "}\n";
            os << "inline std::vector<std::uint64_t> wolvrix_gsim_bitwise_and(\n";
            os << "    const std::vector<std::uint64_t>& lhs,\n";
            os << "    const std::vector<std::uint64_t>& rhs) {\n";
            os << "    const auto wordCount = std::max(lhs.size(), rhs.size());\n";
            os << "    std::vector<std::uint64_t> result(wordCount, 0ULL);\n";
            os << "    for (std::size_t i = 0; i < wordCount; ++i) {\n";
            os << "        const std::uint64_t lhsWord = i < lhs.size() ? lhs[i] : 0ULL;\n";
            os << "        const std::uint64_t rhsWord = i < rhs.size() ? rhs[i] : 0ULL;\n";
            os << "        result[i] = lhsWord & rhsWord;\n";
            os << "    }\n";
            os << "    return result;\n";
            os << "}\n";
            os << "template <typename L, typename R>\n";
            os << "inline std::vector<std::uint64_t> wolvrix_gsim_bitwise_or(\n";
            os << "    const L& lhs,\n";
            os << "    const R& rhs) {\n";
            os << "    const std::uint32_t lhsWidth = wolvrix_gsim_helper_width(lhs);\n";
            os << "    const std::uint32_t rhsWidth = wolvrix_gsim_helper_width(rhs);\n";
            os << "    const auto lhsBits = wolvrix_gsim_to_bits(lhs, lhsWidth);\n";
            os << "    const auto rhsBits = wolvrix_gsim_to_bits(rhs, rhsWidth);\n";
            os << "    const auto wordCount = std::max(lhsBits.size(), rhsBits.size());\n";
            os << "    std::vector<std::uint64_t> result(wordCount, 0ULL);\n";
            os << "    for (std::size_t i = 0; i < wordCount; ++i) {\n";
            os << "        const std::uint64_t lhsWord = i < lhsBits.size() ? lhsBits[i] : 0ULL;\n";
            os << "        const std::uint64_t rhsWord = i < rhsBits.size() ? rhsBits[i] : 0ULL;\n";
            os << "        result[i] = lhsWord | rhsWord;\n";
            os << "    }\n";
            os << "    return result;\n";
            os << "}\n";
            os << "inline std::vector<std::uint64_t> wolvrix_gsim_bitwise_or(\n";
            os << "    const std::vector<std::uint64_t>& lhs,\n";
            os << "    const std::vector<std::uint64_t>& rhs) {\n";
            os << "    const auto wordCount = std::max(lhs.size(), rhs.size());\n";
            os << "    std::vector<std::uint64_t> result(wordCount, 0ULL);\n";
            os << "    for (std::size_t i = 0; i < wordCount; ++i) {\n";
            os << "        const std::uint64_t lhsWord = i < lhs.size() ? lhs[i] : 0ULL;\n";
            os << "        const std::uint64_t rhsWord = i < rhs.size() ? rhs[i] : 0ULL;\n";
            os << "        result[i] = lhsWord | rhsWord;\n";
            os << "    }\n";
            os << "    return result;\n";
            os << "}\n";
            os << "template <typename L, typename R>\n";
            os << "inline std::vector<std::uint64_t> wolvrix_gsim_bitwise_xor(\n";
            os << "    const L& lhs,\n";
            os << "    const R& rhs) {\n";
            os << "    const std::uint32_t lhsWidth = wolvrix_gsim_helper_width(lhs);\n";
            os << "    const std::uint32_t rhsWidth = wolvrix_gsim_helper_width(rhs);\n";
            os << "    const auto lhsBits = wolvrix_gsim_to_bits(lhs, lhsWidth);\n";
            os << "    const auto rhsBits = wolvrix_gsim_to_bits(rhs, rhsWidth);\n";
            os << "    const auto wordCount = std::max(lhsBits.size(), rhsBits.size());\n";
            os << "    std::vector<std::uint64_t> result(wordCount, 0ULL);\n";
            os << "    for (std::size_t i = 0; i < wordCount; ++i) {\n";
            os << "        const std::uint64_t lhsWord = i < lhsBits.size() ? lhsBits[i] : 0ULL;\n";
            os << "        const std::uint64_t rhsWord = i < rhsBits.size() ? rhsBits[i] : 0ULL;\n";
            os << "        result[i] = lhsWord ^ rhsWord;\n";
            os << "    }\n";
            os << "    return result;\n";
            os << "}\n";
            os << "inline std::vector<std::uint64_t> wolvrix_gsim_bitwise_xor(\n";
            os << "    const std::vector<std::uint64_t>& lhs,\n";
            os << "    const std::vector<std::uint64_t>& rhs) {\n";
            os << "    const auto wordCount = std::max(lhs.size(), rhs.size());\n";
            os << "    std::vector<std::uint64_t> result(wordCount, 0ULL);\n";
            os << "    for (std::size_t i = 0; i < wordCount; ++i) {\n";
            os << "        const std::uint64_t lhsWord = i < lhs.size() ? lhs[i] : 0ULL;\n";
            os << "        const std::uint64_t rhsWord = i < rhs.size() ? rhs[i] : 0ULL;\n";
            os << "        result[i] = lhsWord ^ rhsWord;\n";
            os << "    }\n";
            os << "    return result;\n";
            os << "}\n";
            os << "inline std::vector<std::uint64_t> wolvrix_gsim_bitwise_not(\n";
            os << "    const std::vector<std::uint64_t>& value,\n";
            os << "    std::uint32_t width) {\n";
            os << "    std::vector<std::uint64_t> result = value;\n";
            os << "    for (auto& word : result) {\n";
            os << "        word = ~word;\n";
            os << "    }\n";
            os << "    if (!result.empty() && (width % 64U) != 0U) {\n";
            os << "        result.back() &= ((1ULL << (width % 64U)) - 1ULL);\n";
            os << "    }\n";
            os << "    return result;\n";
            os << "}\n";
            os << "template <typename L, typename R>\n";
            os << "inline void wolvrix_gsim_add_into(std::vector<std::uint64_t>& out, const L& lhs, const R& rhs, std::uint32_t width) {\n";
            os << "    const auto wordCount = static_cast<std::size_t>((width + 63U) / 64U);\n";
            os << "    if (out.size() != wordCount) { out.assign(wordCount, 0ULL); }\n";
            os << "    std::uint64_t carry = 0ULL;\n";
            os << "    for (std::size_t i = 0; i < wordCount; ++i) {\n";
            os << "        const std::uint64_t lhsWord = wolvrix_gsim_word_at(lhs, i);\n";
            os << "        const std::uint64_t rhsWord = wolvrix_gsim_word_at(rhs, i);\n";
            os << "        const std::uint64_t partial = lhsWord + carry;\n";
            os << "        const std::uint64_t sum = partial + rhsWord;\n";
            os << "        carry = (partial < lhsWord || sum < partial) ? 1ULL : 0ULL;\n";
            os << "        out[i] = sum;\n";
            os << "    }\n";
            os << "    if ((width % 64U) != 0U && !out.empty()) { out.back() &= wolvrix_gsim_low_mask(width % 64U); }\n";
            os << "}\n";
            os << "template <typename L, typename R>\n";
            os << "inline void wolvrix_gsim_sub_into(std::vector<std::uint64_t>& out, const L& lhs, const R& rhs, std::uint32_t width) {\n";
            os << "    const auto wordCount = static_cast<std::size_t>((width + 63U) / 64U);\n";
            os << "    if (out.size() != wordCount) { out.assign(wordCount, 0ULL); }\n";
            os << "    std::uint64_t borrow = 0ULL;\n";
            os << "    for (std::size_t i = 0; i < wordCount; ++i) {\n";
            os << "        const std::uint64_t lhsWord = wolvrix_gsim_word_at(lhs, i);\n";
            os << "        const std::uint64_t rhsWord = wolvrix_gsim_word_at(rhs, i);\n";
            os << "        const std::uint64_t rhsPlusBorrow = rhsWord + borrow;\n";
            os << "        const std::uint64_t nextBorrow = (rhsPlusBorrow < rhsWord || lhsWord < rhsPlusBorrow) ? 1ULL : 0ULL;\n";
            os << "        out[i] = lhsWord - rhsPlusBorrow;\n";
            os << "        borrow = nextBorrow;\n";
            os << "    }\n";
            os << "    if ((width % 64U) != 0U && !out.empty()) { out.back() &= wolvrix_gsim_low_mask(width % 64U); }\n";
            os << "}\n";
            os << "inline std::vector<std::uint64_t> wolvrix_gsim_add(\n";
            os << "    const std::vector<std::uint64_t>& lhs,\n";
            os << "    const std::vector<std::uint64_t>& rhs) {\n";
            os << "    const auto wordCount = std::max(lhs.size(), rhs.size());\n";
            os << "    std::vector<std::uint64_t> result(wordCount, 0ULL);\n";
            os << "    std::uint64_t carry = 0ULL;\n";
            os << "    for (std::size_t i = 0; i < wordCount; ++i) {\n";
            os << "        const std::uint64_t lhsWord = i < lhs.size() ? lhs[i] : 0ULL;\n";
            os << "        const std::uint64_t rhsWord = i < rhs.size() ? rhs[i] : 0ULL;\n";
            os << "        const std::uint64_t partial = lhsWord + carry;\n";
            os << "        const std::uint64_t sum = partial + rhsWord;\n";
            os << "        carry = (partial < lhsWord || sum < partial) ? 1ULL : 0ULL;\n";
            os << "        result[i] = sum;\n";
            os << "    }\n";
            os << "    return result;\n";
            os << "}\n\n";
            os << "inline std::vector<std::uint64_t> wolvrix_gsim_sub(\n";
            os << "    const std::vector<std::uint64_t>& lhs,\n";
            os << "    const std::vector<std::uint64_t>& rhs) {\n";
            os << "    const auto wordCount = std::max(lhs.size(), rhs.size());\n";
            os << "    std::vector<std::uint64_t> result(wordCount, 0ULL);\n";
            os << "    std::uint64_t borrow = 0ULL;\n";
            os << "    for (std::size_t i = 0; i < wordCount; ++i) {\n";
            os << "        const std::uint64_t lhsWord = i < lhs.size() ? lhs[i] : 0ULL;\n";
            os << "        const std::uint64_t rhsWord = i < rhs.size() ? rhs[i] : 0ULL;\n";
            os << "        const std::uint64_t rhsPlusBorrow = rhsWord + borrow;\n";
            os << "        const std::uint64_t nextBorrow = (rhsPlusBorrow < rhsWord || lhsWord < rhsPlusBorrow) ? 1ULL : 0ULL;\n";
            os << "        result[i] = lhsWord - rhsPlusBorrow;\n";
            os << "        borrow = nextBorrow;\n";
            os << "    }\n";
            os << "    return result;\n";
            os << "}\n\n";
            os << "template <typename L, typename R>\n";
            os << "inline auto wolvrix_gsim_div(const L& lhs, const R& rhs) {\n";
            os << "    using Result = decltype(lhs / rhs);\n";
            os << "    if (rhs == 0) {\n";
            os << "        return static_cast<Result>(0);\n";
            os << "    }\n";
            os << "    return static_cast<Result>(lhs / rhs);\n";
            os << "}\n\n";
            os << "template <typename L, typename R>\n";
            os << "inline int wolvrix_gsim_compare_bits(\n";
            os << "    const L& lhs,\n";
            os << "    std::uint32_t lhsWidth,\n";
            os << "    const R& rhs,\n";
            os << "    std::uint32_t rhsWidth,\n";
            os << "    bool signedCompare) {\n";
            os << "    const std::uint32_t width = std::max(lhsWidth, rhsWidth);\n";
            os << "    auto lhsBits = wolvrix_gsim_to_bits(lhs, lhsWidth);\n";
            os << "    auto rhsBits = wolvrix_gsim_to_bits(rhs, rhsWidth);\n";
            os << "    lhsBits.resize((width + 63U) / 64U, 0ULL);\n";
            os << "    rhsBits.resize((width + 63U) / 64U, 0ULL);\n";
            os << "    if (signedCompare && width > 0) {\n";
            os << "        auto signExtend = [width](std::vector<std::uint64_t>& bits, std::uint32_t sourceWidth) {\n";
            os << "            if (sourceWidth == 0U || sourceWidth >= width) return;\n";
            os << "            const std::uint32_t sourceSignIndex = sourceWidth - 1U;\n";
            os << "            const std::uint64_t sourceSign = (bits[sourceSignIndex / 64U] >> (sourceSignIndex % 64U)) & 1ULL;\n";
            os << "            if (sourceSign == 0ULL) return;\n";
            os << "            for (std::uint32_t bit = sourceWidth; bit < width; ++bit) {\n";
            os << "                bits[bit / 64U] |= (1ULL << (bit % 64U));\n";
            os << "            }\n";
            os << "        };\n";
            os << "        signExtend(lhsBits, lhsWidth);\n";
            os << "        signExtend(rhsBits, rhsWidth);\n";
            os << "        const std::uint32_t signIndex = width - 1U;\n";
            os << "        const std::uint64_t lhsSign = (lhsBits[signIndex / 64U] >> (signIndex % 64U)) & 1ULL;\n";
            os << "        const std::uint64_t rhsSign = (rhsBits[signIndex / 64U] >> (signIndex % 64U)) & 1ULL;\n";
            os << "        if (lhsSign != rhsSign) {\n";
            os << "            return lhsSign ? -1 : 1;\n";
            os << "        }\n";
            os << "    }\n";
            os << "    const std::size_t wordCount = lhsBits.size();\n";
            os << "    for (std::size_t i = wordCount; i > 0; --i) {\n";
            os << "        const std::uint64_t lhsWord = lhsBits[i - 1];\n";
            os << "        const std::uint64_t rhsWord = rhsBits[i - 1];\n";
            os << "        if (lhsWord < rhsWord) return -1;\n";
            os << "        if (lhsWord > rhsWord) return 1;\n";
            os << "    }\n";
            os << "    return 0;\n";
            os << "}\n\n";
            os << "template <typename T>\n";
            os << "inline std::uint64_t wolvrix_gsim_to_u64(const T& value) {\n";
            os << "    if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {\n";
            os << "        return static_cast<std::uint64_t>(value);\n";
            os << "    } else {\n";
            os << "        return value.empty() ? 0ULL : value.front();\n";
            os << "    }\n";
            os << "}\n";
            os << "inline void wolvrix_gsim_or_bits_word(std::vector<std::uint64_t>& out, std::uint64_t bitOffset, std::uint64_t bits, std::uint32_t width) {\n";
            os << "    if (width == 0U || bits == 0ULL) return;\n";
            os << "    bits &= wolvrix_gsim_low_mask(width);\n";
            os << "    const auto dstWord = static_cast<std::size_t>(bitOffset / 64ULL);\n";
            os << "    const auto dstBit = static_cast<std::uint32_t>(bitOffset % 64ULL);\n";
            os << "    if (dstWord < out.size()) out[dstWord] |= bits << dstBit;\n";
            os << "    if (dstBit != 0U && width > 64U - dstBit && dstWord + 1U < out.size()) {\n";
            os << "        out[dstWord + 1U] |= bits >> (64U - dstBit);\n";
            os << "    }\n";
            os << "}\n";
            os << "template <typename T>\n";
            os << "inline void wolvrix_gsim_store_bits(std::vector<std::uint64_t>& out, std::uint64_t bitOffset, const T& value, std::uint32_t width) {\n";
            os << "    static_assert(std::is_integral_v<T> || std::is_enum_v<T>);\n";
            os << "    wolvrix_gsim_or_bits_word(out, bitOffset, static_cast<std::uint64_t>(value), std::min<std::uint32_t>(width, 64U));\n";
            os << "}\n";
            os << "inline void wolvrix_gsim_store_bits(std::vector<std::uint64_t>& out, std::uint64_t bitOffset, const std::vector<std::uint64_t>& value, std::uint32_t width) {\n";
            os << "    std::uint32_t remaining = width;\n";
            os << "    std::uint64_t dstBit = bitOffset;\n";
            os << "    for (std::size_t srcWord = 0; remaining > 0U && srcWord < value.size(); ++srcWord, dstBit += 64ULL) {\n";
            os << "        const std::uint32_t chunkWidth = std::min<std::uint32_t>(remaining, 64U);\n";
            os << "        wolvrix_gsim_or_bits_word(out, dstBit, value[srcWord], chunkWidth);\n";
            os << "        remaining -= chunkWidth;\n";
            os << "    }\n";
            os << "}\n";
            os << "template <typename L, typename R>\n";
            os << "inline std::vector<std::uint64_t> wolvrix_gsim_concat(const L& lhs, std::uint32_t lhsWidth, const R& rhs, std::uint32_t rhsWidth) {\n";
            os << "    std::vector<std::uint64_t> result((static_cast<std::uint64_t>(lhsWidth) + rhsWidth + 63ULL) / 64ULL, 0ULL);\n";
            os << "    wolvrix_gsim_store_bits(result, 0ULL, rhs, rhsWidth);\n";
            os << "    wolvrix_gsim_store_bits(result, rhsWidth, lhs, lhsWidth);\n";
            os << "    return result;\n";
            os << "}\n\n";
            os << "template <typename T>\n";
            os << "inline std::vector<std::uint64_t> wolvrix_gsim_replicate(\n";
            os << "    const T& value,\n";
            os << "    std::uint32_t operandWidth,\n";
            os << "    std::uint32_t rep) {\n";
            os << "    std::vector<std::uint64_t> result((static_cast<std::uint64_t>(operandWidth) * rep + 63ULL) / 64ULL, 0ULL);\n";
            os << "    for (std::uint32_t i = 0; i < rep; ++i) {\n";
            os << "        wolvrix_gsim_store_bits(result, static_cast<std::uint64_t>(i) * operandWidth, value, operandWidth);\n";
            os << "    }\n";
            os << "    return result;\n";
            os << "}\n";
            os << "template <typename T>\n";
            os << "inline void wolvrix_gsim_replicate_into(\n";
            os << "    std::vector<std::uint64_t>& result,\n";
            os << "    const T& value,\n";
            os << "    std::uint32_t operandWidth,\n";
            os << "    std::uint32_t rep) {\n";
            os << "    const auto requiredWords = static_cast<std::size_t>((static_cast<std::uint64_t>(operandWidth) * rep + 63ULL) / 64ULL);\n";
            os << "    if (result.size() != requiredWords) { result.assign(requiredWords, 0ULL); }\n";
            os << "    else { std::fill(result.begin(), result.end(), 0ULL); }\n";
            os << "    for (std::uint32_t i = 0; i < rep; ++i) {\n";
            os << "        wolvrix_gsim_store_bits(result, static_cast<std::uint64_t>(i) * operandWidth, value, operandWidth);\n";
            os << "    }\n";
            os << "}\n";
            os << "template <typename T, typename Amount>\n";
            os << "inline void wolvrix_gsim_shift_left_bits_into(\n";
            os << "    std::vector<std::uint64_t>& result,\n";
            os << "    const T& value,\n";
            os << "    const Amount& amountValue,\n";
            os << "    std::uint32_t width) {\n";
            os << "    const auto requiredWords = static_cast<std::size_t>((width + 63U) / 64U);\n";
            os << "    if (result.size() != requiredWords) { result.assign(requiredWords, 0ULL); }\n";
            os << "    else { std::fill(result.begin(), result.end(), 0ULL); }\n";
            os << "    const std::uint64_t amount = wolvrix_gsim_to_u64(amountValue);\n";
            os << "    if (amount >= width) { return; }\n";
            os << "    wolvrix_gsim_store_bits(result, amount, value, width - static_cast<std::uint32_t>(amount));\n";
            os << "}\n";
            os << "template <typename T, typename Amount>\n";
            os << "inline void wolvrix_gsim_shift_right_bits_into(\n";
            os << "    std::vector<std::uint64_t>& result,\n";
            os << "    const T& value,\n";
            os << "    const Amount& amountValue,\n";
            os << "    std::uint32_t width) {\n";
            os << "    const auto requiredWords = static_cast<std::size_t>((width + 63U) / 64U);\n";
            os << "    if (result.size() != requiredWords) { result.assign(requiredWords, 0ULL); }\n";
            os << "    else { std::fill(result.begin(), result.end(), 0ULL); }\n";
            os << "    const std::uint64_t amount = wolvrix_gsim_to_u64(amountValue);\n";
            os << "    if (amount >= width) { return; }\n";
            os << "    if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {\n";
            os << "        if (amount < 64U && !result.empty()) { result[0] = static_cast<std::uint64_t>(value) >> amount; }\n";
            os << "    } else {\n";
            os << "        const auto wordCount = result.size();\n";
            os << "        for (std::size_t i = 0; i < wordCount; ++i) {\n";
            os << "            result[i] = wolvrix_gsim_load_shifted_word(value, amount + static_cast<std::uint64_t>(i) * 64ULL);\n";
            os << "        }\n";
            os << "    }\n";
            os << "    if ((width % 64U) != 0U && !result.empty()) { result.back() &= wolvrix_gsim_low_mask(width % 64U); }\n";
            os << "}\n";
            os << "template <typename T, typename Amount>\n";
            os << "inline std::vector<std::uint64_t> wolvrix_gsim_shift_left_bits(\n";
            os << "    const T& value,\n";
            os << "    const Amount& amountValue,\n";
            os << "    std::uint32_t width) {\n";
            os << "    const std::uint64_t amount = wolvrix_gsim_to_u64(amountValue);\n";
            os << "    std::vector<std::uint64_t> result((width + 63U) / 64U, 0ULL);\n";
            os << "    if (amount >= width) {\n";
            os << "        return result;\n";
            os << "    }\n";
            os << "    wolvrix_gsim_store_bits(result, amount, value, width - static_cast<std::uint32_t>(amount));\n";
            os << "    return result;\n";
            os << "}\n";
            os << "template <typename T, typename Amount>\n";
            os << "inline std::vector<std::uint64_t> wolvrix_gsim_shift_right_bits(\n";
            os << "    const T& value,\n";
            os << "    const Amount& amountValue,\n";
            os << "    std::uint32_t width) {\n";
            os << "    const std::uint64_t amount = wolvrix_gsim_to_u64(amountValue);\n";
            os << "    std::vector<std::uint64_t> result((width + 63U) / 64U, 0ULL);\n";
            os << "    if (amount >= width) {\n";
            os << "        return result;\n";
            os << "    }\n";
            os << "    if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {\n";
            os << "        if (amount < 64U && !result.empty()) {\n";
            os << "            result[0] = static_cast<std::uint64_t>(value) >> amount;\n";
            os << "        }\n";
            os << "    } else {\n";
            os << "        const auto wordCount = result.size();\n";
            os << "        for (std::size_t i = 0; i < wordCount; ++i) {\n";
            os << "            result[i] = wolvrix_gsim_load_shifted_word(value, amount + static_cast<std::uint64_t>(i) * 64ULL);\n";
            os << "        }\n";
            os << "    }\n";
            os << "    if ((width % 64U) != 0U && !result.empty()) {\n";
            os << "        result.back() &= wolvrix_gsim_low_mask(width % 64U);\n";
            os << "    }\n";
            os << "    return result;\n";
            os << "}\n\n";
            os << "template <typename T, typename Amount>\n";
            os << "inline std::vector<std::uint64_t> wolvrix_gsim_arith_shift_right_bits(\n";
            os << "    const T& value,\n";
            os << "    const Amount& amountValue,\n";
            os << "    std::uint32_t operandWidth,\n";
            os << "    std::uint32_t resultWidth,\n";
            os << "    bool signExtend) {\n";
            os << "    const std::uint64_t amount = wolvrix_gsim_to_u64(amountValue);\n";
            os << "    std::vector<std::uint64_t> result((resultWidth + 63U) / 64U, 0ULL);\n";
            os << "    if (operandWidth == 0U || resultWidth == 0U) {\n";
            os << "        return result;\n";
            os << "    }\n";
            os << "    std::uint64_t fillBit = 0ULL;\n";
            os << "    if (signExtend) {\n";
            os << "        const std::uint64_t signBit = static_cast<std::uint64_t>(operandWidth - 1U);\n";
            os << "        if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {\n";
            os << "            if (signBit < 64U) {\n";
            os << "                fillBit = (static_cast<std::uint64_t>(value) >> signBit) & 1ULL;\n";
            os << "            }\n";
            os << "        } else {\n";
            os << "            const auto signWord = static_cast<std::size_t>(signBit / 64ULL);\n";
            os << "            const auto signOffset = static_cast<std::uint32_t>(signBit % 64ULL);\n";
            os << "            const std::uint64_t word = signWord < value.size() ? value[signWord] : 0ULL;\n";
            os << "            fillBit = (word >> signOffset) & 1ULL;\n";
            os << "        }\n";
            os << "    }\n";
            os << "    for (std::uint32_t i = 0; i < resultWidth; ++i) {\n";
            os << "        const std::uint64_t srcBit = amount + i;\n";
            os << "        std::uint64_t bit = fillBit;\n";
            os << "        if (srcBit < operandWidth) {\n";
            os << "            if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {\n";
            os << "                if (srcBit < 64U) {\n";
            os << "                    bit = (static_cast<std::uint64_t>(value) >> srcBit) & 1ULL;\n";
            os << "                } else {\n";
            os << "                    bit = 0ULL;\n";
            os << "                }\n";
            os << "            } else {\n";
            os << "                const auto srcWord = static_cast<std::size_t>(srcBit / 64ULL);\n";
            os << "                const auto srcOffset = static_cast<std::uint32_t>(srcBit % 64ULL);\n";
            os << "                const std::uint64_t word = srcWord < value.size() ? value[srcWord] : 0ULL;\n";
            os << "                bit = (word >> srcOffset) & 1ULL;\n";
            os << "            }\n";
            os << "        }\n";
            os << "        if (bit != 0ULL) {\n";
            os << "            result[static_cast<std::size_t>(i / 64U)] |= (1ULL << static_cast<std::uint32_t>(i % 64U));\n";
            os << "        }\n";
            os << "    }\n";
            os << "    return result;\n";
            os << "}\n";
            os << "template <typename T, typename Amount>\n";
            os << "inline auto wolvrix_gsim_arith_shift_right(\n";
            os << "    const T& value,\n";
            os << "    const Amount& amountValue,\n";
            os << "    std::uint32_t operandWidth,\n";
            os << "    std::uint32_t resultWidth,\n";
            os << "    bool signExtend) {\n";
            os << "    auto bits = wolvrix_gsim_arith_shift_right_bits(value, amountValue, operandWidth, resultWidth, signExtend);\n";
            os << "    if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {\n";
            os << "        return bits.empty() ? 0ULL : bits.front();\n";
            os << "    } else {\n";
            os << "        return bits;\n";
            os << "    }\n";
            os << "}\n\n";
        }

        void writeHeader(std::ostream &os,
                         const EmitTarget &target,
                         const GsimScratchpadMetadata &metadata,
                         const CodegenState& state,
                         const std::vector<SequentialChunkPlan>& sequentialChunks,
                         bool emitMetadata)
        {
            const std::string ns = sanitizeIdentifier(target.scratchGraphSymbol);
            const std::string structName = "GsimMetadata_" + ns;

            os << "#pragma once\n\n";
            os << "#include <cstdint>\n";
            os << "#include <map>\n";
            os << "#include <memory>\n";
            os << "#include <string>\n";
            os << "#include <vector>\n\n";
            os << "struct SSimTopState;\n";
            os << "struct SSimTopEvalTemps;\n\n";
            // Generated Simulator Class (use SSimTop for compatibility)
            os << "class SSimTop {\n";
            os << "public:\n";
            os << "    SSimTop();\n";
            os << "    ~SSimTop();\n";
            os << "    SSimTop(const SSimTop&) = delete;\n";
            os << "    SSimTop& operator=(const SSimTop&) = delete;\n\n";

            // Reset and set_reset (for compatibility)
            os << "    void set_reset(unsigned reset);\n";
            os << "    void reset();\n";
            os << "    void settle();\n";
            os << "    void commit_step();\n";
            os << "    void step();\n\n";

            // Input port setters
            std::set<std::string> sequentialClockInputs;
            std::set<std::string> headerSequentialDomains;
            for (const auto &domain : state.sequentialPreStmts) {
                headerSequentialDomains.insert(domain.first);
            }
            for (const auto &domain : state.sequentialStmts) {
                headerSequentialDomains.insert(domain.first);
            }
            for (const auto &domain : state.sequentialRegStmts) {
                headerSequentialDomains.insert(domain.first);
            }
            auto prevClockStateNameForDomain = [&](const std::string& domainKey) -> std::string {
                const auto parsedDomain = parseSequentialDomain(domainKey);
                if (!parsedDomain) {
                    return sanitizeIdentifier(domainKey);
                }
                if (state.sequentialClockExprs.find(domainKey) != state.sequentialClockExprs.end()) {
                    return sanitizeIdentifier(parsedDomain->second);
                }
                return resolveSequentialClockStateName(parsedDomain->second, state.inputPorts);
            };
            for (const auto &domainKey : headerSequentialDomains) {
                const auto parsedDomain = parseSequentialDomain(domainKey);
                if (!parsedDomain) {
                    continue;
                }
                sequentialClockInputs.insert(resolveSequentialClockStateName(parsedDomain->second, state.inputPorts));
            }
            auto emitActivateSourceInline = [&](const std::string& sourceKey) {
                if (!state.enableSharding || !state.enableActivityWatermark || state.shardCount() <= 0) {
                    return;
                }
                if (const auto headsIt = state.activitySourceHeadShards.find(sourceKey);
                    headsIt != state.activitySourceHeadShards.end() && !headsIt->second.empty()) {
                    os << " static constexpr std::uint32_t kActivityHeads_" << sanitizeIdentifier(sourceKey) << "[] = {";
                    bool first = true;
                    for (int shard : headsIt->second) {
                        os << (first ? "" : ", ") << shard << "U";
                        first = false;
                    }
                    os << "}; activate_shards(kActivityHeads_" << sanitizeIdentifier(sourceKey) << ", "
                       << headsIt->second.size() << "U);";
                    return;
                }
                const auto shardIt = state.activitySourceFirstShard.find(sourceKey);
                if (shardIt == state.activitySourceFirstShard.end() || shardIt->second < 0) {
                    return;
                }
                os << " activate_shard_range(" << shardIt->second << "U);";
            };
            for (const auto& [name, type] : state.inputPorts) {
                const std::string sanitizedName = sanitizeIdentifier(name);
                if (sanitizedName == "reset") {
                    continue;
                }
                const bool isSequentialClockInput = sequentialClockInputs.count(name) > 0;
                std::string methodName = "set_" + sanitizedName;
                os << "    void " << methodName << "(" << type << " value) { if (!(input_"
                   << sanitizedName << "_ == value)) { input_" << sanitizedName << "_ = value; ";
                if (isSequentialClockInput && state.enableSharding && state.shardCount() > 0) {
                    os << "clock_inputs_dirty_ = true;";
                } else {
                    os << "non_clock_inputs_dirty_ = true;";
                }
                emitActivateSourceInline("input_" + sanitizedName);
                os << " } }\n";
            }
            if (!state.inputPorts.empty()) os << "\n";

            // Output port getters
            for (const auto& [name, type] : state.outputPorts) {
                std::string methodName = "get_" + sanitizeIdentifier(name);
                os << "    " << type << " " << methodName << "() const { return output_" << sanitizeIdentifier(name) << "_; }\n";
            }
            if (!state.outputPorts.empty()) os << "\n";

            // Difftest stub accessors (for compatibility)
            auto hasInputPort = [&](std::string_view portName) {
                return std::any_of(state.inputPorts.begin(), state.inputPorts.end(), [&](const auto& port) {
                    return port.first == portName;
                });
            };
            auto hasOutputPort = [&](std::string_view portName) {
                return std::any_of(state.outputPorts.begin(), state.outputPorts.end(), [&](const auto& port) {
                    return port.first == portName;
                });
            };
            os << "    unsigned get_difftest__DOT__uart__DOT__out__DOT__valid() const { ";
            os << (hasOutputPort("difftest_uart_out_valid") ? "return get_difftest_uart_out_valid();" : "return 0;");
            os << " }\n";
            os << "    std::uint8_t get_difftest__DOT__uart__DOT__out__DOT__ch() const { ";
            os << (hasOutputPort("difftest_uart_out_ch") ? "return get_difftest_uart_out_ch();" : "return 0;");
            os << " }\n";
            os << "    unsigned get_difftest__DOT__uart__DOT__in__DOT__valid() const { ";
            os << (hasOutputPort("difftest_uart_in_valid") ? "return get_difftest_uart_in_valid();" : "return 0;");
            os << " }\n";
            os << "    void set_difftest__DOT__uart__DOT__in__DOT__ch(std::uint8_t ch) { ";
            os << (hasInputPort("difftest_uart_in_ch") ? "set_difftest_uart_in_ch(ch);" : "(void)ch;");
            os << " }\n";
            os << "    std::uint64_t get_difftest__DOT__exit() const { ";
            os << (hasOutputPort("difftest_exit") ? "return get_difftest_exit();" : "return difftest_exit_;");
            os << " }\n";
            os << "    std::uint64_t get_difftest__DOT__step() const { ";
            os << (hasOutputPort("difftest_step") ? "return get_difftest_step();" : "return difftest_step_;");
            os << " }\n";
            os << "    void set_difftest__DOT__perfCtrl__DOT__clean(unsigned clean) { ";
            os << (hasInputPort("difftest_perfCtrl_clean") ? "set_difftest_perfCtrl_clean(static_cast<std::uint8_t>(clean));" : "perf_clean_ = clean;");
            os << " }\n";
            os << "    void set_difftest__DOT__perfCtrl__DOT__dump(unsigned dump) { ";
            os << (hasInputPort("difftest_perfCtrl_dump") ? "set_difftest_perfCtrl_dump(static_cast<std::uint8_t>(dump));" : "perf_dump_ = dump;");
            os << " }\n";
            os << "    void set_difftest__DOT__logCtrl__DOT__begin(std::uint64_t begin) { ";
            os << (hasInputPort("difftest_logCtrl_begin") ? "set_difftest_logCtrl_begin(begin);" : "log_begin_ = begin;");
            os << " }\n";
            os << "    void set_difftest__DOT__logCtrl__DOT__end(std::uint64_t end) { ";
            os << (hasInputPort("difftest_logCtrl_end") ? "set_difftest_logCtrl_end(end);" : "log_end_ = end;");
            os << " }\n\n";

            os << "private:\n";
            if (state.enableSharding && state.shardCount() > 0) {
                for (int i = 0; i < state.shardCount(); ++i) {
                    os << "    void sched_" << i << "();\n";
                }
                os << "    void replay_dirty_mask_shards(std::uint8_t replay_mask_);\n";
                os << "    void replay_clock_input_shards();\n";
                os << "    void replay_non_clock_input_shards();\n";
                os << "    void replay_pending_for_commit(bool& dirty_replayed_, bool include_clock_, bool include_non_clock_);\n";
                if (state.enableActivityWatermark) {
                    os << "    void activate_all_shards();\n";
                    os << "    void activate_shards(const std::uint32_t* indices, std::size_t count);\n";
                    os << "    void activate_shard(std::uint32_t shard);\n";
                    os << "    void activate_shard_mask(std::uint32_t word, std::uint64_t mask);\n";
                    os << "    void activate_shard_range(std::uint32_t firstShard);\n";
                }
                os << "\n";
            }
            os << "    bool reset_ = false;\n";

            auto publicHeaderZeroInitializerForWidth = [](int32_t width) {
                if (width > 64) {
                    const auto chunkCount = static_cast<int32_t>((width + 63) / 64);
                    return "std::vector<std::uint64_t>(" + std::to_string(chunkCount) + "U, 0ULL)";
                }
                return zeroInitializerForType(getCppTypeForWidth(width));
            };

            // Input port storage
            for (const auto& [name, type] : state.inputPorts) {
                const auto widthIt = state.inputPortWidths.find(name);
                const int32_t width = widthIt != state.inputPortWidths.end() ? widthIt->second : 0;
                os << "    " << type << " input_" << sanitizeIdentifier(name) << "_ = "
                   << publicHeaderZeroInitializerForWidth(width) << ";\n";
            }

            // Output port storage
            for (const auto& [name, type] : state.outputPorts) {
                const auto widthIt = state.outputPortWidths.find(name);
                const int32_t width = widthIt != state.outputPortWidths.end() ? widthIt->second : 0;
                os << "    " << type << " output_" << sanitizeIdentifier(name) << "_ = "
                   << publicHeaderZeroInitializerForWidth(width) << ";\n";
            }

            // Persistent design state lives behind state_ in the internal header.

            // Difftest state (for compatibility)
            os << "    std::uint64_t difftest_exit_ = 0;\n";
            os << "    std::uint64_t difftest_step_ = 0;\n";
            if (state.enableSharding && state.enableActivityWatermark && state.emitsDpicCalls &&
                state.dpicGlobalWarmupSteps > 0) {
                os << "    std::uint64_t gsim_pre_dpic_steps_ = 0;\n";
            }
            os << "    unsigned perf_clean_ = 0;\n";
            os << "    unsigned perf_dump_ = 0;\n";
            os << "    std::uint64_t log_begin_ = 0;\n";
            os << "    std::uint64_t log_end_ = 0;\n";
            os << "    SSimTopState* state_;\n";
            os << "    SSimTopEvalTemps* evalTemps_;\n";
            if (state.enableSharding && state.shardCount() > 0) {
                os << "    bool clock_inputs_dirty_ = true;\n";
                os << "    bool committed_state_dirty_ = true;\n";
            }
            os << "    bool non_clock_inputs_dirty_ = true;\n";
            if (state.enableSharding && state.enableActivityWatermark && state.shardCount() > 0) {
                os << "    std::vector<std::uint64_t> active_shard_words_;\n";
                os << "    std::vector<std::uint8_t> active_word_queued_;\n";
                os << "    std::vector<std::uint32_t> active_word_queue_;\n";
            }
            for (const auto &chunk : sequentialChunks) {
                os << "    void " << chunk.methodName << "(bool& committed_, bool& dirty_on_commit_);\n";
            }
            std::set<std::string> prevClockNames;
            for (const auto &domainKey : headerSequentialDomains) {
                const auto parsedDomain = parseSequentialDomain(domainKey);
                if (parsedDomain) {
                    const std::string prevClockName = prevClockStateNameForDomain(domainKey);
                    if (prevClockNames.insert(prevClockName).second) {
                        os << "    bool prev_" << prevClockName << "_ = false;\n";
                    }
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
                         const CodegenState& state,
                         const std::vector<SequentialChunkPlan>& sequentialChunks,
                         std::string_view headerFilename,
                         bool emitMetadata)
        {
            const std::string ns = sanitizeIdentifier(target.scratchGraphSymbol);
            const std::string structName = "GsimMetadata_" + ns;
            const std::string factoryName = "make_" + sanitizeIdentifier(target.scratchGraphSymbol) + "_metadata";
            const std::string validateName = "validate_" + sanitizeIdentifier(target.scratchGraphSymbol) + "_metadata";
            std::map<std::string, std::vector<std::string>> sequentialRegChunkMethods;
            std::map<std::string, std::vector<std::string>> sequentialStmtChunkMethods;
            std::set<std::string> sequentialDomains;
            for (const auto &domain : state.sequentialPreStmts) {
                sequentialDomains.insert(domain.first);
            }
            for (const auto &domain : state.sequentialStmts) {
                sequentialDomains.insert(domain.first);
            }
            for (const auto &chunk : sequentialChunks) {
                if (chunk.preReg) {
                    sequentialRegChunkMethods[chunk.domainKey].push_back(chunk.methodName);
                } else if (chunk.regNames.empty() && !chunk.stmts.empty()) {
                    sequentialStmtChunkMethods[chunk.domainKey].push_back(chunk.methodName);
                } else {
                    sequentialRegChunkMethods[chunk.domainKey].push_back(chunk.methodName);
                }
                sequentialDomains.insert(chunk.domainKey);
            }

            auto prevClockStateNameForDomain = [&](const std::string& domainKey) -> std::string {
                const auto parsedDomain = parseSequentialDomain(domainKey);
                if (!parsedDomain) {
                    return sanitizeIdentifier(domainKey);
                }
                if (state.sequentialClockExprs.find(domainKey) != state.sequentialClockExprs.end()) {
                    return sanitizeIdentifier(parsedDomain->second);
                }
                return resolveSequentialClockStateName(parsedDomain->second, state.inputPorts);
            };

            std::string internalHeader = std::filesystem::path(std::string(headerFilename)).stem().string() + "_internal.hpp";
            os << "#include \"" << internalHeader << "\"\n\n";
            os << "#include <algorithm>\n";
            if (state.emitsDpicCalls) {
                os << "#include <cstring>\n";
                if (state.enableDpicTrace) {
                    os << "#include <iostream>\n";
                }
                os << "#include \"difftest-dpic.h\"\n";
            }
            os << "#include <set>\n\n";
            auto emitPoolCtor = [&](std::string_view ctorName,
                                    std::size_t u8Count,
                                    std::size_t u16Count,
                                    std::size_t u32Count,
                                    std::size_t u64Count,
                                    const std::vector<int32_t>& vecWidths,
                                    std::string_view prefix,
                                    bool rawScalarPools) {
                os << ctorName;
                bool wroteInitList = false;
                auto appendInit = [&](const std::string &text) {
                    os << (wroteInitList ? ", " : " : ") << text;
                    wroteInitList = true;
                };
                if (u8Count > 0) {
                    appendInit(rawScalarPools
                                   ? std::string(prefix) + "U8(new std::uint8_t[" + std::to_string(u8Count) + "]())"
                                   : std::string(prefix) + "U8(" + std::to_string(u8Count) + ", 0)");
                }
                if (u16Count > 0) {
                    appendInit(rawScalarPools
                                   ? std::string(prefix) + "U16(new std::uint16_t[" + std::to_string(u16Count) + "]())"
                                   : std::string(prefix) + "U16(" + std::to_string(u16Count) + ", 0)");
                }
                if (u32Count > 0) {
                    appendInit(rawScalarPools
                                   ? std::string(prefix) + "U32(new std::uint32_t[" + std::to_string(u32Count) + "]())"
                                   : std::string(prefix) + "U32(" + std::to_string(u32Count) + ", 0)");
                }
                if (u64Count > 0) {
                    appendInit(rawScalarPools
                                   ? std::string(prefix) + "U64(new std::uint64_t[" + std::to_string(u64Count) + "]())"
                                   : std::string(prefix) + "U64(" + std::to_string(u64Count) + ", 0)");
                }
                if (!vecWidths.empty()) {
                    appendInit(std::string(prefix) + "Vec(" + std::to_string(vecWidths.size()) + ")");
                }
                os << " {\n";
                if (!vecWidths.empty()) {
                    for (std::size_t i = 0; i < vecWidths.size(); ++i) {
                        const int32_t width = vecWidths[i];
                        os << "    " << prefix << "Vec[" << i << "] = " << zeroInitializerForWidth(width) << ";\n";
                    }
                }
                os << "}\n\n";
            };
            emitPoolCtor("SSimTopState::SSimTopState()",
                         state.stateU8Count,
                         state.stateU16Count,
                         state.stateU32Count,
                         state.stateU64Count,
                         state.stateVecWidths,
                         "state",
                         false);
            os << "SSimTopEvalTemps::SSimTopEvalTemps()";
            bool wroteInitList = false;
            auto appendInit = [&](const std::string &text) {
                os << (wroteInitList ? ", " : " : ") << text;
                wroteInitList = true;
            };
            if (state.tempU8Count > 0) {
                appendInit("tempU8(new std::uint8_t[" + std::to_string(state.tempU8Count) + "]())");
            }
            if (state.tempU16Count > 0) {
                appendInit("tempU16(new std::uint16_t[" + std::to_string(state.tempU16Count) + "]())");
            }
            if (state.tempU32Count > 0) {
                appendInit("tempU32(new std::uint32_t[" + std::to_string(state.tempU32Count) + "]())");
            }
            if (state.tempU64Count > 0) {
                appendInit("tempU64(new std::uint64_t[" + std::to_string(state.tempU64Count) + "]())");
            }
            if (!state.tempVecWidths.empty()) {
                appendInit("tempVec(" + std::to_string(state.tempVecWidths.size()) + ")");
            }
            os << " {\n";
            if (!state.tempVecWidths.empty()) {
                for (std::size_t i = 0; i < state.tempVecWidths.size(); ++i) {
                    const int32_t width = state.tempVecWidths[i];
                    os << "    tempVec[" << i << "] = " << zeroInitializerForWidth(width) << ";\n";
                }
            }
            os << "}\n\n";
            os << "SSimTopEvalTemps::~SSimTopEvalTemps() {";
            if (state.tempU8Count > 0) {
                os << " delete[] tempU8;";
            }
            if (state.tempU16Count > 0) {
                os << " delete[] tempU16;";
            }
            if (state.tempU32Count > 0) {
                os << " delete[] tempU32;";
            }
            if (state.tempU64Count > 0) {
                os << " delete[] tempU64;";
            }
            os << " }\n\n";

            if (state.enableSharding && state.enableActivityWatermark && state.shardCount() > 0) {
                os << "SSimTop::SSimTop() : state_(new SSimTopState()), evalTemps_(new SSimTopEvalTemps()), ";
                os << "active_shard_words_((" << state.shardCount() << "U + 63U) / 64U, UINT64_C(0)), ";
                os << "active_word_queued_((" << state.shardCount() << "U + 63U) / 64U, 0), active_word_queue_() { reset(); }\n";
            } else {
                os << "SSimTop::SSimTop() : state_(new SSimTopState()), evalTemps_(new SSimTopEvalTemps()) { reset(); }\n";
            }
            os << "SSimTop::~SSimTop() { delete evalTemps_; delete state_; }\n\n";
            if (hasInputPortNamed(state.inputPorts, "reset")) {
                os << "void SSimTop::set_reset(unsigned reset) { const auto value = static_cast<std::uint8_t>(reset); ";
                os << "if (!(input_reset_ == value)) { input_reset_ = value; non_clock_inputs_dirty_ = true;";
                if (state.enableSharding && state.enableActivityWatermark && state.shardCount() > 0) {
                    if (const auto headsIt = state.activitySourceHeadShards.find("input_reset");
                        headsIt != state.activitySourceHeadShards.end() && !headsIt->second.empty()) {
                        os << " static constexpr std::uint32_t kActivityHeads_input_reset[] = {";
                        bool first = true;
                        for (int shard : headsIt->second) {
                            os << (first ? "" : ", ") << shard << "U";
                            first = false;
                        }
                        os << "}; activate_shards(kActivityHeads_input_reset, " << headsIt->second.size() << "U);";
                    } else if (const auto shardIt = state.activitySourceFirstShard.find("input_reset");
                               shardIt != state.activitySourceFirstShard.end() && shardIt->second >= 0) {
                        os << " activate_shard_range(" << shardIt->second << "U);";
                    }
                }
                os << " } }\n\n";
            } else {
                os << "void SSimTop::set_reset(unsigned reset) { reset_ = reset; }\n\n";
            }
            if (state.enableSharding && state.enableActivityWatermark && state.shardCount() > 0) {
                os << "void SSimTop::activate_all_shards() {\n";
                os << "    active_word_queue_.clear();\n";
                os << "    std::fill(active_shard_words_.begin(), active_shard_words_.end(), ~UINT64_C(0));\n";
                if ((state.shardCount() % 64U) == 0U) {
                    os << "    if (!active_shard_words_.empty()) { active_shard_words_.back() = ~UINT64_C(0); }\n";
                } else {
                    const std::uint64_t lastActiveShardWordMask =
                        (UINT64_C(1) << (state.shardCount() % 64U)) - UINT64_C(1);
                    os << "    if (!active_shard_words_.empty()) { active_shard_words_.back() = UINT64_C("
                       << lastActiveShardWordMask << "); }\n";
                }
                os << "    std::fill(active_word_queued_.begin(), active_word_queued_.end(), 1);\n";
                os << "    active_word_queue_.reserve(active_shard_words_.size());\n";
                os << "    for (std::uint32_t word = 0; word < active_shard_words_.size(); ++word) { active_word_queue_.push_back(word); }\n";
                os << "}\n\n";
                os << "void SSimTop::activate_shards(const std::uint32_t* indices, std::size_t count) {\n";
                os << "    for (std::size_t i = 0; i < count; ++i) { activate_shard(indices[i]); }\n";
                os << "}\n\n";
                os << "void SSimTop::activate_shard(std::uint32_t shard) {\n";
                os << "    if (shard >= " << state.shardCount() << "U) { return; }\n";
                os << "    const std::uint32_t word = shard / 64U;\n";
                os << "    const std::uint64_t mask = (UINT64_C(1) << (shard % 64U));\n";
                os << "    activate_shard_mask(word, mask);\n";
                os << "}\n\n";
                os << "void SSimTop::activate_shard_mask(std::uint32_t word, std::uint64_t mask) {\n";
                os << "    if (word >= active_shard_words_.size() || mask == UINT64_C(0)) { return; }\n";
                os << "    const std::uint64_t old_bits_ = active_shard_words_[word];\n";
                os << "    const std::uint64_t new_bits_ = old_bits_ | mask;\n";
                os << "    if (new_bits_ == old_bits_) { return; }\n";
                os << "    active_shard_words_[word] = new_bits_;\n";
                os << "    if (active_word_queued_[word] == 0) { active_word_queued_[word] = 1; active_word_queue_.push_back(word); }\n";
                os << "}\n\n";
                os << "void SSimTop::activate_shard_range(std::uint32_t firstShard) {\n";
                os << "    for (std::uint32_t i = firstShard; i < " << state.shardCount() << "U; ++i) { activate_shard(i); }\n";
                os << "}\n\n";
            }
            os << "void SSimTop::reset() {\n";
            os << "    reset_ = false;\n";
            os << "    *state_ = SSimTopState();\n";
            for (const auto& stmt : state.storageResetStmts) {
                os << "    " << stmt << "\n";
            }
            for (const auto& [name, type] : state.outputPorts) {
                (void)type;
                const auto widthIt = state.outputPortWidths.find(name);
                const int32_t width = widthIt != state.outputPortWidths.end() ? widthIt->second : 0;
                os << "    output_" << sanitizeIdentifier(name) << "_ = " << zeroInitializerForWidth(width) << ";\n";
            }
            if (state.enableSharding && state.shardCount() > 0) {
                os << "    clock_inputs_dirty_ = true;\n";
                os << "    committed_state_dirty_ = true;\n";
            }
            os << "    non_clock_inputs_dirty_ = true;\n";
            if (state.enableSharding && state.enableActivityWatermark && state.emitsDpicCalls &&
                state.dpicGlobalWarmupSteps > 0) {
                os << "    gsim_pre_dpic_steps_ = 0;\n";
            }
            if (state.enableSharding && state.enableActivityWatermark && state.shardCount() > 0) {
                os << "    activate_all_shards();\n";
            }
                    {
                        std::set<std::string> resetClockNames;
                        for (const auto &domainKey : sequentialDomains) {
                            const auto parsedDomain = parseSequentialDomain(domainKey);
                            if (parsedDomain) {
                                const std::string resetClock = prevClockStateNameForDomain(domainKey);
                                if (resetClockNames.insert(resetClock).second) {
                            os << "    prev_" << resetClock << "_ = false;\n";
                        }
                    }
                }
            }
            os << "}\n\n";

            os << "void SSimTop::settle() {\n";
            if (state.enableSharding && state.shardCount() > 0) {
                os << "    if (clock_inputs_dirty_) {\n";
                os << "        replay_dirty_mask_shards(UINT8_C(" << static_cast<unsigned>(kDirtyReplayClock) << "));\n";
                os << "        clock_inputs_dirty_ = false;\n";
                os << "    }\n";
                if (state.enableActivityWatermark) {
                    os << "    std::size_t active_cursor_ = 0;\n";
                    os << "    while (active_cursor_ < active_word_queue_.size()) {\n";
                    os << "        const std::uint32_t active_word_ = active_word_queue_[active_cursor_++];\n";
                    os << "        if (active_word_ >= active_shard_words_.size()) { continue; }\n";
                    os << "        active_word_queued_[active_word_] = 0;\n";
                    os << "        std::uint64_t active_bits_ = active_shard_words_[active_word_];\n";
                    os << "        active_shard_words_[active_word_] = UINT64_C(0);\n";
                    os << "        while (active_bits_ != UINT64_C(0)) {\n";
                    os << "            const std::uint32_t active_bit_ = static_cast<std::uint32_t>(__builtin_ctzll(active_bits_));\n";
                    os << "            active_bits_ &= ~(UINT64_C(1) << active_bit_);\n";
                    os << "            const std::uint32_t active_shard_ = active_word_ * 64U + active_bit_;\n";
                    os << "            switch (active_shard_) {\n";
                    for (int i = 0; i < state.shardCount(); ++i) {
                        os << "            case " << i << "U: { sched_" << i << "();";
                        const auto& succ = (i < static_cast<int>(state.shardSuccessors.size())) ? state.shardSuccessors[static_cast<std::size_t>(i)] : std::set<int>{};
                        if (!succ.empty()) {
                            std::map<int, std::uint64_t> successorWordMasks;
                            for (int successor : succ) {
                                if (successor < 0) {
                                    continue;
                                }
                                const int word = successor / 64;
                                const int bit = successor % 64;
                                successorWordMasks[word] |= (std::uint64_t{1} << bit);
                            }
                            for (const auto& [word, mask] : successorWordMasks) {
                                os << " const std::uint64_t kShardSuccessorMask" << i << "_" << word
                                   << " = UINT64_C(" << mask << ");";
                                os << " if (" << word << "U == active_word_) { active_bits_ |= kShardSuccessorMask"
                                   << i << "_" << word << "; }";
                                os << " else { activate_shard_mask(" << word << "U, kShardSuccessorMask"
                                   << i << "_" << word << "); }";
                            }
                        }
                        os << " break; }\n";
                    }
                    os << "            default: break;\n";
                    os << "            }\n";
                    os << "            const std::uint64_t local_bits_ = active_shard_words_[active_word_];\n";
                    os << "            if (local_bits_ != UINT64_C(0)) {\n";
                    os << "                active_bits_ |= local_bits_;\n";
                    os << "                active_shard_words_[active_word_] = UINT64_C(0);\n";
                    os << "                active_word_queued_[active_word_] = 0;\n";
                    os << "            }\n";
                    os << "        }\n";
                    os << "    }\n";
                    os << "    active_word_queue_.clear();\n";
                    os << "    std::fill(active_shard_words_.begin(), active_shard_words_.end(), UINT64_C(0));\n";
                    os << "    std::fill(active_word_queued_.begin(), active_word_queued_.end(), 0);\n";
                } else {
                    for (int i = 0; i < state.shardCount(); ++i) {
                        os << "    sched_" << i << "();\n";
                    }
                }
            }
            if (!state.latchStmts.empty()) {
                os << "    if (!reset_) {\n";
                for (const auto &stmt : state.latchStmts) {
                    std::string s = stmt;
                    if (s.rfind("        ", 0) == 0) s.erase(0, 8);
                    os << "        " << s << "\n";
                }
                os << "    }\n";
            }
            if (!state.outputPorts.empty()) {
                for (const auto &[valueId, portInfo] : state.outputPortValues) {
                    auto valueIt = state.valueVars.find(valueId);
                    const std::string expr = valueIt != state.valueVars.end() ? valueIt->second : "0";
                    os << "    output_" << sanitizeIdentifier(portInfo.first) << "_ = " << expr << ";\n";
                }
            }
            if (state.enableSharding && state.shardCount() > 0) {
                os << "    clock_inputs_dirty_ = false;\n";
                os << "    committed_state_dirty_ = false;\n";
            }
            os << "    non_clock_inputs_dirty_ = false;\n";
            os << "}\n\n";

            auto emitReplayShardBody = [&](int shard) {
                os << "        sched_" << shard << "();";
                if (state.enableActivityWatermark && shard < static_cast<int>(state.shardSuccessors.size())) {
                    const auto& succ = state.shardSuccessors[static_cast<std::size_t>(shard)];
                    if (!succ.empty()) {
                        std::map<int, std::uint64_t> successorWordMasks;
                        for (int successor : succ) {
                            if (successor < 0) {
                                continue;
                            }
                            const int word = successor / 64;
                            const int bit = successor % 64;
                            successorWordMasks[word] |= (std::uint64_t{1} << bit);
                        }
                        for (const auto& [word, mask] : successorWordMasks) {
                            os << " activate_shard_mask(" << word << "U, UINT64_C(" << mask << "));";
                        }
                    }
                }
                os << "\n";
            };
            auto emitReplayMaskBody = [&]() {
                int shard = 0;
                while (shard < state.shardCount()) {
                    const std::uint8_t shardMask =
                        shard < static_cast<int>(state.shardDirtyReplayMask.size())
                            ? state.shardDirtyReplayMask[static_cast<std::size_t>(shard)]
                            : 0;
                    if (shardMask == 0) {
                        ++shard;
                        continue;
                    }
                    int runEnd = shard + 1;
                    while (runEnd < state.shardCount()) {
                        const std::uint8_t nextMask =
                            runEnd < static_cast<int>(state.shardDirtyReplayMask.size())
                                ? state.shardDirtyReplayMask[static_cast<std::size_t>(runEnd)]
                                : 0;
                        if (nextMask != shardMask) {
                            break;
                        }
                        ++runEnd;
                    }
                    os << "    if ((replay_mask_ & UINT8_C(" << static_cast<unsigned>(shardMask)
                       << ")) != UINT8_C(0)) {\n";
                    for (int replayShard = shard; replayShard < runEnd; ++replayShard) {
                        emitReplayShardBody(replayShard);
                    }
                    os << "    }\n";
                    shard = runEnd;
                }
            };
            if (state.enableSharding && state.shardCount() > 0) {
                os << "void SSimTop::replay_dirty_mask_shards(std::uint8_t replay_mask_) {\n";
                os << "    if (replay_mask_ == UINT8_C(0)) { return; }\n";
                emitReplayMaskBody();
                os << "}\n\n";
                os << "void SSimTop::replay_clock_input_shards() { replay_dirty_mask_shards(UINT8_C(" << static_cast<unsigned>(kDirtyReplayClock) << ")); clock_inputs_dirty_ = false; }\n\n";
                os << "void SSimTop::replay_non_clock_input_shards() { replay_dirty_mask_shards(UINT8_C(" << static_cast<unsigned>(kDirtyReplayNonClock) << ")); non_clock_inputs_dirty_ = false; }\n\n";
                os << "void SSimTop::replay_pending_for_commit(bool& dirty_replayed_, bool include_clock_, bool include_non_clock_) {\n";
                os << "    std::uint8_t replay_mask_ = UINT8_C(0);\n";
                os << "    if (include_clock_ && clock_inputs_dirty_) { replay_mask_ |= UINT8_C("
                   << static_cast<unsigned>(kDirtyReplayClock) << "); }\n";
                os << "    if (include_non_clock_ && non_clock_inputs_dirty_) { replay_mask_ |= UINT8_C("
                   << static_cast<unsigned>(kDirtyReplayNonClock) << "); }\n";
                os << "    if (replay_mask_ != UINT8_C(0)) {\n";
                os << "        dirty_replayed_ = true;\n";
                os << "        replay_dirty_mask_shards(replay_mask_);\n";
                os << "        if (include_clock_) { clock_inputs_dirty_ = false; }\n";
                os << "        if (include_non_clock_) { non_clock_inputs_dirty_ = false; }\n";
                os << "    }\n";
                os << "    if (committed_state_dirty_) { dirty_replayed_ = true; settle(); }\n";
                os << "}\n\n";
            }

            auto emitPendingReplay = [&](std::string_view indent, bool includeClock, bool includeNonClock, bool includeCommitted) {
                if (includeCommitted) {
                    os << indent << "replay_pending_for_commit(dirty_replayed_, "
                       << (includeClock ? "true" : "false") << ", "
                       << (includeNonClock ? "true" : "false") << ");\n";
                    return;
                }
                os << indent << "std::uint8_t replay_mask_ = UINT8_C(0);\n";
                if (includeClock) {
                    os << indent << "if (clock_inputs_dirty_) { replay_mask_ |= UINT8_C("
                       << static_cast<unsigned>(kDirtyReplayClock) << "); }\n";
                }
                if (includeNonClock) {
                    os << indent << "if (non_clock_inputs_dirty_) { replay_mask_ |= UINT8_C("
                       << static_cast<unsigned>(kDirtyReplayNonClock) << "); }\n";
                }
                os << indent << "if (replay_mask_ != UINT8_C(0)) {\n";
                os << indent << "    dirty_replayed_ = true;\n";
                os << indent << "    replay_dirty_mask_shards(replay_mask_);\n";
                if (includeClock) {
                    os << indent << "    clock_inputs_dirty_ = false;\n";
                }
                if (includeNonClock) {
                    os << indent << "    non_clock_inputs_dirty_ = false;\n";
                }
                os << indent << "}\n";
            };

            os << "void SSimTop::commit_step() {\n";
            os << "    bool committed_ = false;\n";
            if (state.enableSharding && state.shardCount() > 0) {
                os << "    bool dirty_replayed_ = false;\n";
            }
            os << "    bool post_commit_settled_ = false;\n";
            if (state.tempU8Count > 0) {
                os << "    const auto* tempU8_data_ = evalTemps_->tempU8;\n";
                os << "    (void)tempU8_data_;\n";
            }
            auto commitStepClockExpr = [&](std::string expr) {
                if (state.tempU8Count > 0) {
                    const std::string needle = "evalTemps_->tempU8[";
                    std::size_t pos = 0;
                    while ((pos = expr.find(needle, pos)) != std::string::npos) {
                        expr.replace(pos, needle.size(), "tempU8_data_[");
                        pos += std::string("tempU8_data_[").size();
                    }
                }
                return expr;
            };
            if (!sequentialDomains.empty()) {
                std::vector<std::pair<std::string, std::string>> domainClockExprs;
                domainClockExprs.reserve(sequentialDomains.size());
                bool domainMetadataOk = true;
                for (const auto &domainKey : sequentialDomains) {
                    const auto parsedDomain = parseSequentialDomain(domainKey);
                    if (!parsedDomain) {
                        domainMetadataOk = false;
                        break;
                    }
                    const std::string edge = parsedDomain->first;
                    if (edge != "posedge" && edge != "negedge") {
                        domainMetadataOk = false;
                        break;
                    }
                    std::string resolvedClock =
                        resolveSequentialClockStateName(parsedDomain->second, state.inputPorts);
                    std::string currClockExpr;
                    auto exprIt = state.sequentialClockExprs.find(domainKey);
                    if (exprIt != state.sequentialClockExprs.end()) {
                        currClockExpr = exprIt->second;
                    } else {
                        currClockExpr = "input_" + resolvedClock + "_";
                    }
                    currClockExpr = commitStepClockExpr(std::move(currClockExpr));
                    domainClockExprs.emplace_back(domainKey, currClockExpr);
                }
                if (!domainMetadataOk) {
                    os << "    throw std::runtime_error(\"GSIM emitted runtime encountered unsupported clock-domain metadata\");\n";
                } else {
                    os << "    if (reset_) {\n";
                    if (!state.outputPorts.empty()) {
                        os << "        settle();\n";
                    }
                    os << "        reset_ = false;\n";
                    {
                        std::set<std::string> resetClockAssignments;
                        for (const auto &domainState : domainClockExprs) {
                            const auto parsedDomain = parseSequentialDomain(domainState.first);
                            const std::string prevClockState = prevClockStateNameForDomain(domainState.first);
                            const std::string prevClock = "prev_" + prevClockState + "_";
                            if (resetClockAssignments.insert(prevClock).second) {
                                os << "        " << prevClock << " = static_cast<bool>(" << domainState.second << ");\n";
                            }
                        }
                        }
                        os << "        difftest_exit_ = 0;\n";
                        if (state.enableSharding && state.shardCount() > 0) {
                            os << "        clock_inputs_dirty_ = true;\n";
                            os << "        committed_state_dirty_ = true;\n";
                        }
                        os << "        non_clock_inputs_dirty_ = true;\n";
                        if (state.enableSharding && state.enableActivityWatermark) {
                            os << "        activate_all_shards();\n";
                        }
                        os << "        return;\n";
                        os << "    }\n";
                    if (state.enableSharding && state.shardCount() > 0) {
                        os << "    if (clock_inputs_dirty_) {\n";
                        if (!state.latchStmts.empty()) {
                            os << "        settle();\n";
                            os << "        dirty_replayed_ = true;\n";
                            os << "        replay_clock_input_shards();\n";
                        } else {
                            os << "        const bool had_non_clock_inputs_dirty_ = non_clock_inputs_dirty_;\n";
                            emitPendingReplay("        ", true, false, true);
                            os << "        non_clock_inputs_dirty_ = had_non_clock_inputs_dirty_;\n";
                        }
                        os << "    }\n";
                    }
                    os << "    if (non_clock_inputs_dirty_) {\n";
                    if (state.enableSharding && state.shardCount() > 0) {
                        if (!state.latchStmts.empty()) {
                            os << "        settle();\n";
                            os << "        dirty_replayed_ = true;\n";
                            os << "        replay_non_clock_input_shards();\n";
                        } else {
                            emitPendingReplay("        ", false, true, true);
                        }
                    } else {
                        if (!state.outputPorts.empty()) {
                            os << "        settle();\n";
                        } else {
                            os << "        non_clock_inputs_dirty_ = false;\n";
                        }
                    }
                    os << "    }\n";
                    if (state.enableSharding && state.shardCount() > 0) {
                        os << "    if (committed_state_dirty_) {\n";
                        emitPendingReplay("        ", false, false, true);
                        os << "    }\n";
                    }
                    os << "    bool any_domain_reg_committed_ = false;\n";
                    for (const auto &domainKey : sequentialDomains) {
                        const auto parsedDomain = parseSequentialDomain(domainKey);
                        const std::string edge = parsedDomain->first;
                        std::string currClockExpr;
                        auto exprIt = state.sequentialClockExprs.find(domainKey);
                        if (exprIt != state.sequentialClockExprs.end()) {
                            currClockExpr = exprIt->second;
                        } else {
                            const std::string resolvedClock =
                                resolveSequentialClockStateName(parsedDomain->second, state.inputPorts);
                            currClockExpr = "input_" + resolvedClock + "_";
                        }
                        const bool clockNeedsPreEdgeReplay = exprIt != state.sequentialClockExprs.end();
                        const std::string prevClockState = prevClockStateNameForDomain(domainKey);
                        const std::string prevClock = "prev_" + prevClockState + "_";
                        currClockExpr = commitStepClockExpr(std::move(currClockExpr));
                        const std::string edgeExpr = edge == "posedge"
                                                         ? "(!" + prevClock + " && static_cast<bool>(" + currClockExpr + "))"
                                                         : "(" + prevClock + " && !static_cast<bool>(" + currClockExpr + "))";
                        if (state.enableSharding && state.shardCount() > 0 && clockNeedsPreEdgeReplay) {
                            os << "    if ((non_clock_inputs_dirty_ || committed_state_dirty_) && !dirty_replayed_) {\n";
                            if (!state.latchStmts.empty()) {
                                os << "        settle();\n";
                                emitPendingReplay("        ", false, true, true);
                            } else {
                                emitPendingReplay("        ", false, true, true);
                            }
                            os << "    }\n";
                        }
                        os << "    if (" << edgeExpr << ") {\n";
                        if (state.enableSharding && state.shardCount() > 0) {
                            os << "        if ((non_clock_inputs_dirty_ || committed_state_dirty_) && !dirty_replayed_) {\n";
                            if (!state.latchStmts.empty()) {
                                os << "            settle();\n";
                                emitPendingReplay("            ", false, true, true);
                            } else {
                                emitPendingReplay("            ", false, true, true);
                            }
                            os << "        }\n";
                        }
                        if (auto regChunkIt = sequentialRegChunkMethods.find(domainKey); regChunkIt != sequentialRegChunkMethods.end()) {
                            os << "        bool domain_reg_committed_ = false;\n";
                            os << "        bool domain_reg_dirty_ = false;\n";
                            for (const auto &methodName : regChunkIt->second) {
                                os << "        " << methodName << "(domain_reg_committed_, domain_reg_dirty_);\n";
                            }
                            os << "        if (domain_reg_committed_) {\n";
                            os << "            committed_ = true;\n";
                            os << "            any_domain_reg_committed_ = true;\n";
                            if (state.enableSharding && state.shardCount() > 0) {
                                os << "            committed_state_dirty_ = true;\n";
                                os << "            dirty_replayed_ = false;\n";
                            }
                            os << "        }\n";
                        }
                        os << "    }\n";
                    }
                    os << "    if (any_domain_reg_committed_) {\n";
                    if (state.emitsNoDiffGuardedDpicCalls && !state.emitsRuntimeDpicCalls) {
                        os << "#ifndef CONFIG_NO_DIFFTEST\n";
                    }
                    if (state.enableSharding && state.enableActivityWatermark && state.emitsDpicCalls &&
                        state.dpicGlobalWarmupSteps > 0) {
                        os << "        if (gsim_pre_dpic_steps_ < " << state.dpicGlobalWarmupSteps << "U) {\n";
                        os << "            activate_all_shards();\n";
                        os << "        } else {\n";
                        if (!state.dpicPreSettleShards.empty()) {
                            os << "            static constexpr std::uint32_t kDpicPreSettleShards[] = {";
                            bool firstShard = true;
                            for (int shard : state.dpicPreSettleShards) {
                                os << (firstShard ? "" : ", ") << shard << "U";
                                firstShard = false;
                            }
                            os << "};\n";
                            os << "            activate_shards(kDpicPreSettleShards, " << state.dpicPreSettleShards.size() << "U);\n";
                        }
                        os << "        }\n";
                    } else if (state.enableSharding && state.enableActivityWatermark && state.emitsDpicCalls &&
                               !state.dpicPreSettleShards.empty()) {
                        os << "        static constexpr std::uint32_t kDpicPreSettleShards[] = {";
                        bool firstShard = true;
                        for (int shard : state.dpicPreSettleShards) {
                            os << (firstShard ? "" : ", ") << shard << "U";
                            firstShard = false;
                        }
                        os << "};\n";
                        os << "        activate_shards(kDpicPreSettleShards, " << state.dpicPreSettleShards.size() << "U);\n";
                    }
                    os << "        settle();\n";
                    os << "        post_commit_settled_ = true;\n";
                    if (state.emitsNoDiffGuardedDpicCalls && !state.emitsRuntimeDpicCalls) {
                        os << "#endif\n";
                    }
                    os << "    }\n";
                    for (const auto &domainKey : sequentialDomains) {
                        const auto parsedDomain = parseSequentialDomain(domainKey);
                        const std::string edge = parsedDomain->first;
                        std::string currClockExpr;
                        auto exprIt = state.sequentialClockExprs.find(domainKey);
                        if (exprIt != state.sequentialClockExprs.end()) {
                            currClockExpr = exprIt->second;
                        } else {
                            const std::string resolvedClock =
                                resolveSequentialClockStateName(parsedDomain->second, state.inputPorts);
                            currClockExpr = "input_" + resolvedClock + "_";
                        }
                        const std::string prevClockState = prevClockStateNameForDomain(domainKey);
                        const std::string prevClock = "prev_" + prevClockState + "_";
                        currClockExpr = commitStepClockExpr(std::move(currClockExpr));
                        const std::string edgeExpr = edge == "posedge"
                                                         ? "(!" + prevClock + " && static_cast<bool>(" + currClockExpr + "))"
                                                         : "(" + prevClock + " && !static_cast<bool>(" + currClockExpr + "))";
                        os << "    if (" << edgeExpr << ") {\n";
                        if (state.enableSharding && state.shardCount() > 0) {
                            os << "        if ((non_clock_inputs_dirty_ || committed_state_dirty_) && !dirty_replayed_) {\n";
                            if (!state.latchStmts.empty()) {
                                os << "            settle();\n";
                                emitPendingReplay("            ", false, true, true);
                            } else {
                                emitPendingReplay("            ", false, true, true);
                            }
                            os << "        }\n";
                        }
                        if (auto stmtChunkIt = sequentialStmtChunkMethods.find(domainKey); stmtChunkIt != sequentialStmtChunkMethods.end()) {
                            os << "        bool domain_stmt_committed_ = false;\n";
                            os << "        bool domain_stmt_dirty_ = false;\n";
                            for (const auto &methodName : stmtChunkIt->second) {
                                os << "        " << methodName << "(domain_stmt_committed_, domain_stmt_dirty_);\n";
                            }
                            os << "        if (domain_stmt_committed_) {\n";
                            os << "            committed_ = true;\n";
                            os << "        }\n";
                            if (state.enableSharding && state.shardCount() > 0) {
                                os << "        if (domain_stmt_dirty_) {\n";
                                os << "            committed_state_dirty_ = true;\n";
                                os << "            dirty_replayed_ = false;\n";
                                os << "        }\n";
                            }
                        } else if (auto stmtIt = state.sequentialStmts.find(domainKey); stmtIt != state.sequentialStmts.end()) {
                            const auto dirtyIt = state.sequentialStmtDirtyOnCommit.find(domainKey);
                            const std::vector<bool>* dirtyOnCommit =
                                dirtyIt != state.sequentialStmtDirtyOnCommit.end() ? &dirtyIt->second : nullptr;
                            if (state.enableSharding && state.shardCount() > 0) {
                                os << "        bool domain_stmt_dirty_ = false;\n";
                            }
                            for (std::size_t stmtIndex = 0; stmtIndex < stmtIt->second.size(); ++stmtIndex) {
                                const auto& stmt = stmtIt->second[stmtIndex];
                                const bool stmtDirtyOnCommit =
                                    dirtyOnCommit == nullptr || stmtIndex >= dirtyOnCommit->size() ||
                                    (*dirtyOnCommit)[stmtIndex];
                                std::string s = stmt;
                                if (s.rfind("        ", 0) == 0) s.erase(0, 8);
                                if (state.enableSharding && state.shardCount() > 0 && stmtDirtyOnCommit) {
                                    const std::string needle = "committed_ = true;";
                                    const std::string replacement = "domain_stmt_dirty_ = true; committed_ = true;";
                                    std::size_t pos = 0;
                                    while ((pos = s.find(needle, pos)) != std::string::npos) {
                                        s.replace(pos, needle.size(), replacement);
                                        pos += replacement.size();
                                    }
                                }
                                os << "        " << s << "\n";
                            }
                            if (state.enableSharding && state.shardCount() > 0) {
                                os << "        if (domain_stmt_dirty_) {\n";
                                os << "            committed_state_dirty_ = true;\n";
                                os << "            dirty_replayed_ = false;\n";
                                if (state.enableActivityWatermark) {
                                    os << "            activate_all_shards();\n";
                                }
                                os << "        }\n";
                            }
                        }
                        os << "    }\n";
                    }
                    {
                        std::set<std::string> postDomainClockAssignments;
                        for (const auto &domainState : domainClockExprs) {
                            const auto parsedDomain = parseSequentialDomain(domainState.first);
                            const std::string prevClockState = prevClockStateNameForDomain(domainState.first);
                            const std::string prevClock = "prev_" + prevClockState + "_";
                            if (postDomainClockAssignments.insert(prevClock).second) {
                                os << "    " << prevClock << " = static_cast<bool>(" << domainState.second << ");\n";
                            }
                        }
                    }
                }
                } else {
                    os << "    if (reset_) {\n";
                    os << "        settle();\n";
                    os << "        reset_ = false;\n";
                    os << "        difftest_exit_ = 0;\n";
                    if (state.enableSharding && state.shardCount() > 0) {
                        os << "        clock_inputs_dirty_ = true;\n";
                        os << "        committed_state_dirty_ = true;\n";
                    }
                    os << "        non_clock_inputs_dirty_ = true;\n";
                    if (state.enableSharding && state.enableActivityWatermark) {
                        os << "        activate_all_shards();\n";
                    }
                    os << "        return;\n";
                    os << "    }\n";
            }
            if (state.enableSharding && state.shardCount() > 0) {
                os << "    if (!post_commit_settled_ && (non_clock_inputs_dirty_ || committed_state_dirty_ || dirty_replayed_)) {\n";
            } else {
                os << "    if (!post_commit_settled_ && (committed_ || non_clock_inputs_dirty_)) {\n";
            }
            os << "        settle();\n";
            os << "    }\n";
            if (state.enableSharding && state.enableActivityWatermark && state.emitsDpicCalls &&
                state.dpicGlobalWarmupSteps > 0) {
                os << "    if (committed_) { ++gsim_pre_dpic_steps_; }\n";
            }
            os << "    if (committed_) { ++difftest_step_; }\n";
            os << "    difftest_exit_ = 0;\n";
            os << "}\n\n";

            os << "void SSimTop::step() {\n";
            os << "    commit_step();\n";
            os << "}\n\n";

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

        void writeSequentialChunkSource(std::ostream &os,
                                        const CodegenState& state,
                                        const SequentialChunkPlan& chunk,
                                        std::string_view internalHeaderFilename)
        {
            os << "#include \"" << internalHeaderFilename << "\"\n";
            if (state.emitsDpicCalls) {
                os << "#include <cstring>\n";
                if (state.enableDpicTrace) {
                    os << "#include <iostream>\n";
                }
                os << "#include \"difftest-dpic.h\"\n";
            }
            os << "\n";
            os << "void SSimTop::" << chunk.methodName << "(bool& committed_, bool& dirty_on_commit_) {\n";
            const bool tracksStatementDirty =
                !chunk.stmts.empty() && chunk.stmtDirtyOnCommit.size() == chunk.stmts.size();
            if (!tracksStatementDirty) {
                os << "    (void)dirty_on_commit_;\n";
            }
            if (tracksStatementDirty) {
                os << "    bool chunk_dirty_on_commit_ = false;\n";
            }
            if (!chunk.regNames.empty()) {
                os << "    bool chunk_updated_ = false;\n";
            }
            auto isWideReg = [&](const std::string& regName, const std::vector<std::string>& regStmts) {
                const auto regWidthIt = state.storageWidths.find(regName);
                const std::string updateFlag = "next_" + regName + "_updated_";
                return (regWidthIt != state.storageWidths.end() && regWidthIt->second > 64) ||
                       std::any_of(regStmts.begin(), regStmts.end(), [&](const std::string& stmt) {
                           return stmt.find(updateFlag) != std::string::npos ||
                                  stmt.find("wolvrix_gsim_mask_merge") != std::string::npos;
                       });
            };
            auto emitActivitySourceTouches = [&](const std::vector<std::string>& sources, std::string_view indent) {
                if (!state.enableSharding || !state.enableActivityWatermark || state.shardCount() <= 0) {
                    return;
                }
                std::set<int> firstShards;
                for (const auto& source : sources) {
                    if (const auto headsIt = state.activitySourceHeadShards.find(source);
                        headsIt != state.activitySourceHeadShards.end()) {
                        firstShards.insert(headsIt->second.begin(), headsIt->second.end());
                        continue;
                    }
                    if (const auto shardIt = state.activitySourceFirstShard.find(source);
                        shardIt != state.activitySourceFirstShard.end() && shardIt->second >= 0) {
                        firstShards.insert(shardIt->second);
                    }
                }
                if (!firstShards.empty()) {
                    os << indent << "static constexpr std::uint32_t kTouchedStmtFirstShards[] = {";
                    bool first = true;
                    for (int firstShard : firstShards) {
                        os << (first ? "" : ", ") << firstShard << "U";
                        first = false;
                    }
                    os << "}; activate_shards(kTouchedStmtFirstShards, "
                       << firstShards.size() << "U); ";
                } else {
                    os << indent << "activate_all_shards(); ";
                }
            };
            auto emitStatement = [&](std::string s, bool dirtyOnCommit = false) {
                if (s.rfind("        ", 0) == 0) s.erase(0, 8);
                if (dirtyOnCommit) {
                    const std::string needle = "committed_ = true;";
                    const std::string replacement = "chunk_dirty_on_commit_ = true; dirty_on_commit_ = true; committed_ = true;";
                    std::size_t pos = 0;
                    while ((pos = s.find(needle, pos)) != std::string::npos) {
                        s.replace(pos, needle.size(), replacement);
                        pos += replacement.size();
                    }
                }
                if (!chunk.regNames.empty()) {
                    const std::string needle = "committed_ = true;";
                    const std::string replacement = "chunk_updated_ = true; committed_ = true;";
                    std::size_t pos = 0;
                    while ((pos = s.find(needle, pos)) != std::string::npos) {
                        s.replace(pos, needle.size(), replacement);
                        pos += replacement.size();
                    }
                }
                os << "    " << s << "\n";
            };
            struct ParsedNextAssignment {
                std::string condition;
                std::string rhs;
            };
            auto splitTopLevelArgs = [](std::string_view text) {
                auto trim = [](std::string arg) {
                    const auto first = arg.find_first_not_of(" \\t\\n\\r");
                    if (first == std::string::npos) {
                        return std::string{};
                    }
                    const auto last = arg.find_last_not_of(" \\t\\n\\r");
                    return arg.substr(first, last - first + 1);
                };
                std::vector<std::string> args;
                std::size_t start = 0;
                int depth = 0;
                for (std::size_t i = 0; i < text.size(); ++i) {
                    const char ch = text[i];
                    if (ch == '(' || ch == '[' || ch == '{') {
                        ++depth;
                    } else if (ch == ')' || ch == ']' || ch == '}') {
                        --depth;
                    } else if (ch == ',' && depth == 0) {
                        args.push_back(trim(std::string(text.substr(start, i - start))));
                        start = i + 1;
                    }
                }
                args.push_back(trim(std::string(text.substr(start))));
                return args;
            };
            auto parseMaskMergeArgs = [&](const std::string& rhs) -> std::optional<std::vector<std::string>> {
                constexpr std::string_view prefix = "wolvrix_gsim_mask_merge(";
                if (rhs.rfind(prefix, 0) != 0 || rhs.empty() || rhs.back() != ')') {
                    return std::nullopt;
                }
                auto args = splitTopLevelArgs(std::string_view(rhs).substr(prefix.size(), rhs.size() - prefix.size() - 1));
                if (args.size() != 3) {
                    return std::nullopt;
                }
                return args;
            };
            auto isSimpleWideLvalue = [](const std::string& rhs) {
                return rhs.rfind("evalTemps_->tempVec[", 0) == 0 ||
                       rhs.rfind("state_->stateVec[", 0) == 0 ||
                       (rhs.find('[') != std::string::npos && rhs.find('(') == std::string::npos);
            };
            auto parseSingleNextAssignment = [&](const std::string& regName, std::string s) -> std::optional<ParsedNextAssignment> {
                if (s.rfind("        ", 0) == 0) s.erase(0, 8);
                const std::string lhs = "next_" + regName + " = ";
                if (s.rfind("if (", 0) != 0) {
                    return std::nullopt;
                }
                const std::string marker = ") { " + lhs;
                const auto markerPos = s.find(marker);
                if (markerPos == std::string::npos) {
                    return std::nullopt;
                }
                const auto rhsBegin = markerPos + marker.size();
                const auto rhsEnd = s.find(";", rhsBegin);
                if (rhsEnd == std::string::npos) {
                    return std::nullopt;
                }
                ParsedNextAssignment parsed;
                parsed.condition = s.substr(4, markerPos - 4);
                parsed.rhs = s.substr(rhsBegin, rhsEnd - rhsBegin);
                return parsed;
            };
            auto canDirectScalarWrite = [&](const std::string& regName, const std::vector<std::string>& regStmts) {
                if (regStmts.size() != 1) {
                    return false;
                }
                const auto parsed = parseSingleNextAssignment(regName, regStmts.front());
                return parsed.has_value() && parsed->rhs.find("state_->") == std::string::npos;
            };

            for (const auto &regName : chunk.regNames) {
                const auto regStmtIt = chunk.regStmts.find(regName);
                const std::vector<std::string>& regStmts = regStmtIt != chunk.regStmts.end() ? regStmtIt->second : chunk.stmts;
                const bool wideReg = isWideReg(regName, regStmts);
                const bool directWideWrite = wideReg && regStmts.size() == 1 && parseSingleNextAssignment(regName, regStmts.front()).has_value();
                const bool directScalarWrite = !wideReg && canDirectScalarWrite(regName, regStmts);
                if (directWideWrite || directScalarWrite) {
                    continue;
                }
                if (wideReg) {
                    os << "    std::vector<std::uint64_t> next_" << regName << ";\n";
                    os << "    bool next_" << regName << "_updated_ = false;\n";
                } else {
                    os << "    auto next_" << regName << " = " << state.persistentStorageExpr(regName) << ";\n";
                }
            }
            for (const auto &regName : chunk.regNames) {
                const auto regStmtIt = chunk.regStmts.find(regName);
                if (regStmtIt == chunk.regStmts.end()) {
                    continue;
                }
                const bool wideReg = isWideReg(regName, regStmtIt->second);
                const auto parsed = regStmtIt->second.size() == 1 ? parseSingleNextAssignment(regName, regStmtIt->second.front()) : std::optional<ParsedNextAssignment>{};
                const bool directWideWrite = wideReg && parsed.has_value();
                const bool directScalarWrite = !wideReg && parsed.has_value() && parsed->rhs.find("state_->") == std::string::npos;
                if (directWideWrite) {
                    const std::string stateExpr = state.persistentStorageExpr(regName);
                    if (const auto maskMergeArgs = parseMaskMergeArgs(parsed->rhs);
                        maskMergeArgs && (*maskMergeArgs)[0] == stateExpr) {
                        os << "    if (" << parsed->condition << ") { if (wolvrix_gsim_mask_merge_in_place("
                           << stateExpr << ", " << (*maskMergeArgs)[1] << ", " << (*maskMergeArgs)[2]
                           << ")) { chunk_updated_ = true; committed_ = true; } }\n";
                    } else if (isSimpleWideLvalue(parsed->rhs)) {
                        os << "    if (" << parsed->condition << ") { if (" << stateExpr << " != " << parsed->rhs
                           << ") { " << stateExpr << " = " << parsed->rhs
                           << "; chunk_updated_ = true; committed_ = true; } }\n";
                    } else {
                        const std::string tempName = "direct_next_" + regName;
                        os << "    if (" << parsed->condition << ") { auto " << tempName << " = " << parsed->rhs
                           << "; if (" << stateExpr << " != " << tempName << ") { " << stateExpr
                           << " = std::move(" << tempName
                           << "); chunk_updated_ = true; committed_ = true; } }\n";
                    }
                    continue;
                }
                if (directScalarWrite) {
                    const std::string stateExpr = state.persistentStorageExpr(regName);
                    const std::string tempName = "direct_next_" + regName;
                    os << "    if (" << parsed->condition << ") { const auto " << tempName << " = " << parsed->rhs
                       << "; if (" << stateExpr << " != " << tempName << ") { " << stateExpr
                       << " = " << tempName << "; chunk_updated_ = true; committed_ = true; } }\n";
                    continue;
                }
                for (const auto &stmt : regStmtIt->second) {
                    emitStatement(stmt);
                }
            }
            for (std::size_t stmtIndex = 0; stmtIndex < chunk.stmts.size(); ++stmtIndex) {
                emitStatement(chunk.stmts[stmtIndex],
                              tracksStatementDirty && chunk.stmtDirtyOnCommit[stmtIndex]);
            }
            if (tracksStatementDirty && state.enableSharding && state.enableActivityWatermark && state.shardCount() > 0) {
                std::vector<std::string> dirtyActivitySources;
                std::set<std::string> seenSources;
                bool hasDirtyStatementWithoutSources = false;
                for (std::size_t stmtIndex = 0; stmtIndex < chunk.stmts.size(); ++stmtIndex) {
                    if (!chunk.stmtDirtyOnCommit[stmtIndex]) {
                        continue;
                    }
                    if (stmtIndex >= chunk.stmtActivitySources.size() ||
                        chunk.stmtActivitySources[stmtIndex].empty()) {
                        hasDirtyStatementWithoutSources = true;
                        continue;
                    }
                    for (const auto& source : chunk.stmtActivitySources[stmtIndex]) {
                        if (seenSources.insert(source).second) {
                            dirtyActivitySources.push_back(source);
                        }
                    }
                }
                if (hasDirtyStatementWithoutSources) {
                    dirtyActivitySources.clear();
                }
                os << "    if (chunk_dirty_on_commit_) { ";
                emitActivitySourceTouches(dirtyActivitySources, "");
                os << "}\n";
            }
            if (!chunk.regNames.empty()) {
                os << "    if (chunk_updated_) {\n";
                if (state.enableSharding && state.enableActivityWatermark && state.shardCount() > 0) {
                    std::set<int> firstShards;
                    for (const auto &regName : chunk.regNames) {
                        if (const auto headsIt = state.activitySourceHeadShards.find(regName);
                            headsIt != state.activitySourceHeadShards.end()) {
                            firstShards.insert(headsIt->second.begin(), headsIt->second.end());
                            continue;
                        }
                        if (const auto shardIt = state.activitySourceFirstShard.find(regName);
                            shardIt != state.activitySourceFirstShard.end() && shardIt->second >= 0) {
                            firstShards.insert(shardIt->second);
                        }
                    }
                    if (!firstShards.empty()) {
                        os << "        static constexpr std::uint32_t kTouchedStateFirstShards[] = {";
                        bool first = true;
                        for (int firstShard : firstShards) {
                            os << (first ? "" : ", ") << firstShard << "U";
                            first = false;
                        }
                        os << "};\n";
                        os << "        activate_shards(kTouchedStateFirstShards, "
                           << firstShards.size() << "U);\n";
                    } else {
                        os << "        activate_all_shards();\n";
                    }
                }
                for (const auto &regName : chunk.regNames) {
                    const auto regStmtIt = chunk.regStmts.find(regName);
                    const std::vector<std::string>& regStmts = regStmtIt != chunk.regStmts.end() ? regStmtIt->second : chunk.stmts;
                    const bool wideReg = isWideReg(regName, regStmts);
                    if ((wideReg && regStmts.size() == 1) || (!wideReg && canDirectScalarWrite(regName, regStmts))) {
                        continue;
                    }
                    if (wideReg) {
                        os << "        if (next_" << regName << "_updated_) { "
                           << state.persistentStorageExpr(regName) << " = std::move(next_" << regName << "); }\n";
                    } else {
                        os << "        " << state.persistentStorageExpr(regName) << " = next_" << regName << ";\n";
                    }
                }
                os << "    }\n";
            }
            os << "}\n";
        }

        void writeInternalHeader(std::ostream &os,
                                 const CodegenState& state,
                                 std::string_view publicHeaderFilename)
        {
            os << "#pragma once\n\n";
            os << "#include \"" << publicHeaderFilename << "\"\n\n";
            writeRuntimeHelpers(os);
            if (state.emitsDpicCalls) {
                for (const auto& [symbol, importInfo] : state.dpicImports) {
                    if (symbol.empty() || startsWith(symbol, "v_difftest_")) {
                        continue;
                    }
                    const std::string returnType = importInfo.hasReturn
                                                       ? dpicReturnTypeForImport(importInfo)
                                                       : std::string("void");
                    os << "extern \"C\" " << returnType << " " << symbol << "(";
                    for (std::size_t i = 0; i < importInfo.argNames.size(); ++i) {
                        if (i != 0) {
                            os << ", ";
                        }
                        const int64_t width =
                            i < importInfo.argWidths.size() ? importInfo.argWidths[i] : 1;
                        os << dpicCppTypeForWidth(width);
                        if (i < importInfo.argDirs.size() && importInfo.argDirs[i] == "output") {
                            os << " *";
                        }
                    }
                    os << ");\n";
                }
                os << "\n";
            }
            os << "struct SSimTopState {\n";
            os << "    SSimTopState();\n";
            if (state.stateU8Count > 0) {
                os << "    std::vector<std::uint8_t> stateU8;\n";
            }
            if (state.stateU16Count > 0) {
                os << "    std::vector<std::uint16_t> stateU16;\n";
            }
            if (state.stateU32Count > 0) {
                os << "    std::vector<std::uint32_t> stateU32;\n";
            }
            if (state.stateU64Count > 0) {
                os << "    std::vector<std::uint64_t> stateU64;\n";
            }
            if (!state.stateVecWidths.empty()) {
                os << "    std::vector<std::vector<std::uint64_t>> stateVec;\n";
            }
            for (const auto& decl : state.storageDecls) {
                os << "    " << decl << "\n";
            }
            os << "};\n\n";
            os << "struct SSimTopEvalTemps {\n";
            os << "    SSimTopEvalTemps();\n";
            os << "    ~SSimTopEvalTemps();\n";
            os << "    SSimTopEvalTemps(const SSimTopEvalTemps&) = delete;\n";
            os << "    SSimTopEvalTemps& operator=(const SSimTopEvalTemps&) = delete;\n";
            if (state.tempU8Count > 0) {
                os << "    std::uint8_t* tempU8 = nullptr;\n";
            }
            if (state.tempU16Count > 0) {
                os << "    std::uint16_t* tempU16 = nullptr;\n";
            }
            if (state.tempU32Count > 0) {
                os << "    std::uint32_t* tempU32 = nullptr;\n";
            }
            if (state.tempU64Count > 0) {
                os << "    std::uint64_t* tempU64 = nullptr;\n";
            }
            if (!state.tempVecWidths.empty()) {
                os << "    std::vector<std::vector<std::uint64_t>> tempVec;\n";
            }
            os << "};\n";
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
        const std::filesystem::path internalHeaderPath = outputDir / (baseName + "_internal.hpp");
        const std::filesystem::path sourcePath = outputDir / (baseName + ".cpp");
        const std::filesystem::path manifestPath = outputDir / (baseName + ".manifest");

        // Generate code from GRH operations
        CodegenState state;
        state.maxShardSize = parsePositiveIntAttr(options, "behavior_shard_max_bytes", state.maxShardSize);
        state.commitShardSize = parsePositiveIntAttr(options, "commit_shard_max_bytes", state.commitShardSize);
        state.enableActivityWatermark = attrEnabled(options, "activity_shard_watermark", false);
        state.enableDpicTrace = attrEnabled(options, "dpic_trace", false);
        state.enableXsZeroRetireTrace =
            state.enableDpicTrace && attrEnabled(options, "xs_zero_retire_trace", false);
        state.dpicGlobalWarmupSteps = parsePositiveIntAttr(options, "dpic_global_warmup_steps", 0);
        const bool emitMetadata = attrEnabled(options, "emit_metadata", true);
        std::vector<std::string> outputKeepPrefixes;
        if (auto keepPrefixesAttr = attrValue(options, "output_keep_prefixes"))
        {
            outputKeepPrefixes = splitCsv(*keepPrefixesAttr);
        }

        // Determine if sharding should be enabled based on operation count.
        // HDLBits contains medium-size graphs that still explode into giant inline
        // expressions well below XiangShan scale, so keep the threshold low enough
        // to materialize those graphs into sched shards before C++ compile time or
        // emitted file size becomes pathological.
        state.enableSharding = metadata->opCount > 128;
        const std::size_t reserveOps = static_cast<std::size_t>(std::max<std::int64_t>(metadata->opCount, 0));
        state.valueVars.reserve(reserveOps);
        state.valueDirtyReplayMasks.reserve(reserveOps);
        state.dirtyReplayRootMasks.reserve(target->graph->inputPorts().size());
        if (state.enableSharding) {
            state.valueProducerShard.reserve(reserveOps / 4U + 1024U);
            state.valueProducerOpIndex.reserve(reserveOps / 4U + 1024U);
        }
        if (state.enableActivityWatermark) {
            state.valueActivityFirstShard.reserve(reserveOps / 4U + 1024U);
            state.inputActivitySourceNames.reserve(target->graph->inputPorts().size());
        }

        collectPorts(*target->graph, state, options.portOrderStrategy, options.portOrderNames, outputKeepPrefixes);
        collectRegisters(*target->graph, state);
        collectLatches(*target->graph, state);
        collectMemories(*target->graph, state);
        if (state.enableActivityWatermark) {
            state.activitySourceFirstShard.reserve(state.storageWidths.size() + state.inputPorts.size() + state.memories.size());
        }

        std::set<std::string> sequentialClockInputs;
        std::set<std::string> sequentialDomains;
        for (const auto &domain : state.sequentialStmts) {
            sequentialDomains.insert(domain.first);
        }
        for (const auto &domain : state.sequentialRegStmts) {
            sequentialDomains.insert(domain.first);
        }
        for (const auto &domainKey : sequentialDomains) {
            const auto parsedDomain = parseSequentialDomain(domainKey);
            if (!parsedDomain) {
                continue;
            }
            sequentialClockInputs.insert(resolveSequentialClockStateName(parsedDomain->second, state.inputPorts));
        }
        for (const auto &port : target->graph->inputPorts()) {
            const std::uint8_t replayMask = sequentialClockInputs.count(port.name) > 0
                ? kDirtyReplayClock
                : kDirtyReplayNonClock;
            state.dirtyReplayRootMasks[port.value] = replayMask;
            state.valueDirtyReplayMasks[port.value] = replayMask;
            if (state.enableActivityWatermark) {
                state.inputActivitySourceNames[port.value] = "input_" + sanitizeIdentifier(port.name);
            }
        }

        std::uint32_t maxOpIndex = 0;
        for (const auto &opId : target->graph->operations()) {
            maxOpIndex = std::max(maxOpIndex, opId.index);
        }
        std::vector<wolvrix::lib::grh::OperationId> opIdByIndex(static_cast<std::size_t>(maxOpIndex) + 1U,
                                                                wolvrix::lib::grh::OperationId::invalid());
        for (const auto &opId : target->graph->operations()) {
            opIdByIndex[opId.index] = opId;
        }
        if (state.enableSharding) {
            state.opProducerFirstShardByIndex.assign(opIdByIndex.size(), -1);
            state.opProducerLastShardByIndex.assign(opIdByIndex.size(), -1);
        }

        // Traverse operations in topo order. Operation indices are dense after graph
        // freeze, so a flat vector avoids millions of unordered_map lookups during
        // XiangShan-scale C++ emission.
        for (int64_t opIdx : metadata->topoOrder) {
            if (opIdx < 0 || static_cast<std::uint64_t>(opIdx) >= opIdByIndex.size()) {
                continue;
            }
            const auto opId = opIdByIndex[static_cast<std::size_t>(opIdx)];
            if (!opId) {
                continue;
            }
            auto op = target->graph->getOperation(opId);
            lowerOperation(*target->graph, op, *metadata, state, diagnostics());
        }

        if (state.enableSharding && !state.dirtyReplayProducerOpSeeds.empty()) {
            std::vector<std::int64_t> replayOps;
            replayOps.reserve(state.dirtyReplayProducerOpSeeds.size());
            std::unordered_set<std::int64_t> seenReplayOps;
            for (const auto seed : state.dirtyReplayProducerOpSeeds) {
                if (seed >= 0 && seenReplayOps.insert(seed).second) {
                    replayOps.push_back(seed);
                }
            }
            for (std::size_t cursor = 0; cursor < replayOps.size(); ++cursor) {
                const auto opIndex = replayOps[cursor];
                if (opIndex >= 0 && static_cast<std::size_t>(opIndex) < state.opProducerLastShardByIndex.size()) {
                    const int firstShard = state.opProducerFirstShardByIndex[static_cast<std::size_t>(opIndex)];
                    const int lastShard = state.opProducerLastShardByIndex[static_cast<std::size_t>(opIndex)];
                    if (firstShard >= 0 && lastShard >= firstShard) {
                        const int maxShard = static_cast<int>(state.shardDirtyReplayMask.size()) - 1;
                        for (int shard = firstShard; shard <= lastShard && shard <= maxShard; ++shard) {
                            state.shardDirtyReplayMask[static_cast<std::size_t>(shard)] |= kDirtyReplayDpicProducer;
                        }
                    }
                }
                const auto predIt = metadata->predecessors.find(opIndex);
                if (predIt == metadata->predecessors.end()) {
                    continue;
                }
                for (const auto predecessor : predIt->second) {
                    if (predecessor >= 0 && seenReplayOps.insert(predecessor).second) {
                        replayOps.push_back(predecessor);
                    }
                }
            }
        }

        // Sequential commit chunks can depend on constants and register-read temps
        // materialized in the first behavior shard.  Dirty-input replay must refresh
        // that base shard before a later clock edge, otherwise a low-phase input
        // step can clear the dirty flag while leaving commit conditions/RHS temps
        // at reset defaults.
        if (state.enableSharding && (!state.sequentialStmts.empty() || !state.sequentialRegStmts.empty()) && !state.shardDirtyReplayMask.empty()) {
            state.shardDirtyReplayMask.front() |= kDirtyReplayAll;
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

        const auto sequentialChunks = buildSequentialChunkPlans(state);

        auto header = openOutputFile(headerPath);
        auto internalHeader = openOutputFile(internalHeaderPath);
        auto source = openOutputFile(sourcePath);
        if (!header || !internalHeader || !source)
        {
            result.success = false;
            return result;
        }

        writeHeader(*header, *target, *metadata, state, sequentialChunks, emitMetadata);
        writeInternalHeader(*internalHeader, state, headerPath.filename().string());
        writeSource(*source, *target, *metadata, state, sequentialChunks, headerPath.filename().string(), emitMetadata);

        std::vector<std::string> manifestEntries;
        manifestEntries.push_back(sourcePath.filename().string());

        for (const auto &chunk : sequentialChunks) {
            std::string chunkFileName = baseName + "_" + chunk.methodName + ".cpp";
            std::filesystem::path chunkPath = outputDir / chunkFileName;
            auto chunkFile = openOutputFile(chunkPath);
            if (!chunkFile) {
                result.success = false;
                return result;
            }
            writeSequentialChunkSource(*chunkFile, state, chunk, internalHeaderPath.filename().string());
            result.artifacts.push_back(chunkPath.string());
            manifestEntries.push_back(chunkFileName);
        }

        // Write out all shard files if sharding is enabled and there are any
        if (state.enableSharding && !state.shardBuffers.empty()) {
            for (size_t i = 0; i < state.shardBuffers.size(); ++i) {
                std::string shardFileName = baseName + "_sched_" + std::to_string(i) + ".cpp";
                std::filesystem::path shardPath = outputDir / shardFileName;

                auto shardFile = openOutputFile(shardPath);
                if (shardFile) {
                    *shardFile << "#include \"" << internalHeaderPath.filename().string() << "\"\n\n";
                    if (state.emitsNoDiffGuardedDpicCalls) {
                        *shardFile << "#include <cstring>\n";
                        *shardFile << "#include \"difftest-dpic.h\"\n\n";
                    }
                    *shardFile << "void SSimTop::sched_" << i << "() {\n";
                    *shardFile << state.shardBuffers[i];
                    *shardFile << "}\n";
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
