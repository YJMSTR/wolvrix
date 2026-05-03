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
#include <limits>
#include <unordered_set>
#include <utility>
#include <array>
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

        std::string trimCopy(std::string text)
        {
            const auto first = text.find_first_not_of(" \t\n\r");
            if (first == std::string::npos) {
                return {};
            }
            const auto last = text.find_last_not_of(" \t\n\r");
            return text.substr(first, last - first + 1);
        }

        std::string stripOuterParens(std::string text)
        {
            text = trimCopy(std::move(text));
            bool changed = true;
            while (changed && text.size() >= 2 && text.front() == '(' && text.back() == ')') {
                changed = false;
                int depth = 0;
                bool wraps = true;
                for (std::size_t i = 0; i < text.size(); ++i) {
                    const char ch = text[i];
                    if (ch == '(') {
                        ++depth;
                    } else if (ch == ')') {
                        --depth;
                        if (depth == 0 && i + 1 != text.size()) {
                            wraps = false;
                            break;
                        }
                    }
                    if (depth < 0) {
                        wraps = false;
                        break;
                    }
                }
                if (wraps && depth == 0) {
                    text = trimCopy(text.substr(1, text.size() - 2));
                    changed = true;
                }
            }
            return text;
        }

        bool isSimpleWideStorageExpr(std::string_view text)
        {
            return (text.rfind("evalTemps_->tempVec[", 0) == 0 ||
                    text.rfind("state_->stateVec[", 0) == 0 ||
                    text.rfind("input_", 0) == 0 ||
                    text.rfind("output_", 0) == 0) &&
                   text.find('(') == std::string::npos;
        }

        std::optional<std::size_t> parseEvalTempVecIndex(std::string_view expr)
        {
            constexpr std::string_view prefix = "evalTemps_->tempVec[";
            if (expr.rfind(prefix, 0) != 0) {
                return std::nullopt;
            }
            const std::size_t end = expr.find(']', prefix.size());
            if (end == std::string_view::npos) {
                return std::nullopt;
            }
            std::size_t value = 0;
            for (std::size_t i = prefix.size(); i < end; ++i) {
                const char ch = expr[i];
                if (ch < '0' || ch > '9') {
                    return std::nullopt;
                }
                value = value * 10U + static_cast<std::size_t>(ch - '0');
            }
            return value;
        }

        std::optional<std::size_t> fixedWideAssignWordCount(std::string_view lhs,
                                                            const std::vector<int32_t>& tempVecWidths)
        {
            const auto index = parseEvalTempVecIndex(lhs);
            if (!index || *index >= tempVecWidths.size()) {
                return std::nullopt;
            }
            const int32_t width = tempVecWidths[*index];
            if (width <= 0) {
                return std::nullopt;
            }
            const auto wordCount = static_cast<std::size_t>((static_cast<std::uint64_t>(width) + 63ULL) / 64ULL);
            if (wordCount == 0U || wordCount > 4U) {
                return std::nullopt;
            }
            return wordCount;
        }

        bool tempVecHasAtLeastWords(std::string_view expr,
                                    const std::vector<int32_t>& tempVecWidths,
                                    std::size_t wordCount)
        {
            const auto index = parseEvalTempVecIndex(expr);
            if (!index || *index >= tempVecWidths.size()) {
                return false;
            }
            const int32_t width = tempVecWidths[*index];
            if (width <= 0) {
                return false;
            }
            const auto availableWords =
                static_cast<std::size_t>((static_cast<std::uint64_t>(width) + 63ULL) / 64ULL);
            return availableWords >= wordCount;
        }

        std::string wideAssignFunctionName(std::size_t wordCount)
        {
            if (wordCount >= 1U && wordCount <= 4U) {
                return "wolvrix_gsim_assign_bits_" + std::to_string(wordCount);
            }
            return "wolvrix_gsim_assign_bits";
        }

        std::optional<std::size_t> fixedWideWordCountForWidth(std::int64_t width)
        {
            if (width <= 0) {
                return std::nullopt;
            }
            const auto wordCount = static_cast<std::size_t>((static_cast<std::uint64_t>(width) + 63ULL) / 64ULL);
            if (wordCount == 0U || wordCount > 4U) {
                return std::nullopt;
            }
            return wordCount;
        }

        std::string fixedWideLastMaskExpr(std::int64_t width)
        {
            const auto usedBits = static_cast<std::uint32_t>(static_cast<std::uint64_t>(width) % 64ULL);
            if (usedBits == 0U) {
                return "~0ULL";
            }
            return "wolvrix_gsim_low_mask(" + std::to_string(usedBits) + "U)";
        }

        bool isWideStorageTernaryExpr(const std::string& expr)
        {
            const std::string text = stripOuterParens(expr);
            int depth = 0;
            std::size_t question = std::string::npos;
            std::size_t colon = std::string::npos;
            for (std::size_t i = 0; i < text.size(); ++i) {
                const char ch = text[i];
                if (ch == '(' || ch == '[' || ch == '{') {
                    ++depth;
                } else if (ch == ')' || ch == ']' || ch == '}') {
                    --depth;
                } else if (ch == '?' && depth == 0) {
                    question = i;
                } else if (ch == ':' && depth == 0 && question != std::string::npos) {
                    colon = i;
                    break;
                }
            }
            if (question == std::string::npos || colon == std::string::npos) {
                return false;
            }
            const std::string trueExpr = trimCopy(text.substr(question + 1, colon - question - 1));
            const std::string falseExpr = trimCopy(text.substr(colon + 1));
            return isSimpleWideStorageExpr(trueExpr) && isSimpleWideStorageExpr(falseExpr);
        }

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
            std::map<std::string, std::vector<std::string>> sequentialGlobalPreStmts;
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
            std::set<std::string> dpicTraceTargets;
            std::size_t dpicTraceCallSite = 0;
            std::size_t dpicMaterializedCallSite = 0;
            std::set<int> dpicPreSettleShards;
            int dpicGlobalWarmupSteps = 0;
            bool enablePendingWriteStats = false;
            bool enableActivityBatchStats = false;
            std::string activityBatchMode = "legacy";
            std::string activityBatchFallbackReason = "none";
            bool activityBatchMetadataPresent = false;
            bool activityBatchMetadataValid = false;
            bool activityBatchDispatchEnabled = false;
            bool activityBatchStrictDispatch = false;
            int64_t activityBatchCount = 0;
            int64_t activityBatchAvgOps = 0;
            int64_t activityBatchMaxOps = 0;
            int64_t activityBatchAvgEstimatedLines = 0;
            int64_t activityBatchMaxEstimatedLines = 0;
            int64_t activityBatchMaxSuccessorFanout = 0;
            int64_t activityBatchSuccessorEdges = 0;
            int64_t activityBatchEntryCount = 0;
            int64_t activitySupernodeActiveWords = 0;
            int64_t activitySupernodeBodyCount = 0;
            int64_t activitySupernodeShardCount = 0;
            int64_t activityBatchToShardMinSpan = 0;
            int64_t activityBatchToShardMaxSpan = 0;

            bool shouldTraceDpicTarget(std::string_view target) const
            {
                return dpicTraceTargets.empty() ||
                       dpicTraceTargets.count("*") > 0 ||
                       dpicTraceTargets.count(std::string(target)) > 0;
            }

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
            int activityBoundaryShardSoftBytes = 0;

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
            std::unordered_map<wolvrix::lib::grh::ValueId, std::string, wolvrix::lib::grh::ValueIdHash> valueChangeFanoutTokens;
            std::unordered_map<wolvrix::lib::grh::ValueId, int, wolvrix::lib::grh::ValueIdHash> valueChangeFanoutTokenShard;
            std::unordered_map<wolvrix::lib::grh::ValueId, std::string, wolvrix::lib::grh::ValueIdHash> valueChangeFanoutLhs;
            std::unordered_map<wolvrix::lib::grh::ValueId, std::string, wolvrix::lib::grh::ValueIdHash> valueChangeFanoutRhs;
            std::unordered_map<wolvrix::lib::grh::ValueId, std::set<int>, wolvrix::lib::grh::ValueIdHash> valueChangeFanoutHeadShards;
            std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> changeTrackedValues;
            std::vector<std::pair<std::uint32_t, std::uint64_t>> changedFanoutMasks;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> changedFanoutRanges;
            std::unordered_map<std::string, std::uint32_t> changedFanoutRangeIds;
            std::vector<std::pair<std::uint32_t, std::uint64_t>> shardActivationMasks;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> shardActivationRanges;
            std::unordered_map<std::string, std::uint32_t> shardActivationRangeIds;
            int changedValueFanoutInlineMaskLimit = 1;
            int shardActivationInlineMaskLimit = 1;
            int changedValueFanoutEstimateBytes = 128;
            std::vector<int> opProducerFirstShardByIndex;
            std::vector<int> opProducerLastShardByIndex;
            std::vector<std::int32_t> opActivityOrdinalByIndex;
            std::vector<std::int32_t> opBatchOrdinalByIndex;
            std::vector<std::uint8_t> activityClassBitsByOrdinal;
            std::vector<std::int64_t> dirtyReplayProducerOpSeeds;
            std::vector<std::set<int>> shardSuccessors;
            std::vector<std::array<std::set<int>, 4>> shardDirtyReplaySuccessors;
            std::vector<std::string> currentOpDirectActivitySources;
            int currentOpActivityFirstShard = -1;
            int currentOpActivityOrdinal = -1;
            int currentOpBatchOrdinal = -1;
            std::uint8_t currentOpActivityClassBit = 0;
            int currentOpFirstEmittedShard = -1;
            int lastEmittedShard = -1;
            int currentShardLastActivityOrdinal = -1;
            std::uint8_t currentShardActivityClassMask = 0;
            std::unordered_map<std::string, int> activitySourceFirstShard;
            std::unordered_map<std::string, std::set<int>> activitySourceHeadShards;
            std::unordered_map<std::string, std::set<int>> activitySourceHeadBatches;
            std::vector<std::pair<int64_t, int64_t>> activityBatchShardSpans;
            std::size_t changeFanoutTokenCount = 0;

            void resetCurrentShardActivityBoundaryState() {
                currentShardLastActivityOrdinal = -1;
                currentShardActivityClassMask = 0;
            }

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
                    shardDirtyReplaySuccessors.emplace_back();
                    currentShard = static_cast<int>(shardBuffers.size()) - 1;
                    currentShardSize = 0;
                    resetCurrentShardActivityBoundaryState();
                }
                return &shardBuffers[currentShard];
            }

            void startNewShard() {
                if (!enableSharding) {
                    return;
                }
                currentShard++;
                currentShardSize = 0;
                resetCurrentShardActivityBoundaryState();
                if (currentShard >= static_cast<int>(shardBuffers.size())) {
                    shardBuffers.emplace_back();
                    shardBuffers.back().reserve(static_cast<std::size_t>(maxShardSize) + 1024U);
                    shardDirtyReplayMask.push_back(0);
                    shardSuccessors.emplace_back();
                    shardDirtyReplaySuccessors.emplace_back();
                }
            }

            // Helper to create a new shard when current one gets too large
            void ensureShardSpace(int estimatedSize) {
                if (!enableSharding) {
                    return; // Don't shard for small designs
                }

                if (currentShardSize + estimatedSize > maxShardSize) {
                    startNewShard();
                }
            }

            void setCurrentOpActivity(std::uint32_t opIndex) {
                currentOpActivityOrdinal = -1;
                currentOpBatchOrdinal = -1;
                currentOpActivityClassBit = 0;
                if (opIndex < opBatchOrdinalByIndex.size()) {
                    currentOpBatchOrdinal = opBatchOrdinalByIndex[opIndex];
                }
                if (opIndex >= opActivityOrdinalByIndex.size()) {
                    return;
                }
                const int ordinal = opActivityOrdinalByIndex[opIndex];
                if (ordinal < 0 || ordinal >= static_cast<int>(activityClassBitsByOrdinal.size())) {
                    return;
                }
                currentOpActivityOrdinal = ordinal;
                currentOpActivityClassBit = activityClassBitsByOrdinal[static_cast<std::size_t>(ordinal)];
            }

            void maybeCutShardForCurrentOpActivity() {
                if (!enableSharding || !enableActivityWatermark ||
                    activityBoundaryShardSoftBytes <= 0 ||
                    currentShardSize < activityBoundaryShardSoftBytes ||
                    currentOpActivityOrdinal < 0 || currentOpActivityClassBit == 0) {
                    return;
                }
                if (currentShardSize <= 0) {
                    return;
                }
                const bool incomingNewClass =
                    (currentShardActivityClassMask & currentOpActivityClassBit) == 0;
                const bool currentShardAlreadyMixed =
                    (currentShardActivityClassMask & (currentShardActivityClassMask - 1U)) != 0;
                const bool activityChanged =
                    currentShardLastActivityOrdinal >= 0 &&
                    currentShardLastActivityOrdinal != currentOpActivityOrdinal;
                if (incomingNewClass || (currentShardAlreadyMixed && activityChanged)) {
                    startNewShard();
                }
            }

            void addShardDirtyReplaySuccessor(int fromShard, int toShard, std::uint8_t dirtyReplayMask) {
                if (dirtyReplayMask == 0 || fromShard < 0 || toShard < 0 ||
                    fromShard >= static_cast<int>(shardDirtyReplaySuccessors.size())) {
                    return;
                }
                shardDirtyReplaySuccessors[static_cast<std::size_t>(fromShard)][dirtyReplayMask].insert(toShard);
            }

            void addShardSuccessor(int fromShard, int toShard, std::uint8_t dirtyReplayMask = 0) {
                if (fromShard < 0 || toShard < 0 || fromShard >= static_cast<int>(shardSuccessors.size())) {
                    return;
                }
                shardSuccessors[static_cast<std::size_t>(fromShard)].insert(toShard);
                addShardDirtyReplaySuccessor(fromShard, toShard, dirtyReplayMask);
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
                    if (currentOpActivityClassBit != 0) {
                        currentShardActivityClassMask |= currentOpActivityClassBit;
                    }
                    if (currentOpActivityOrdinal >= 0) {
                        currentShardLastActivityOrdinal = currentOpActivityOrdinal;
                    }
                    for (const auto& source : currentOpDirectActivitySources) {
                        auto [it, inserted] = activitySourceFirstShard.emplace(source, currentShard);
                        if (!inserted && currentShard < it->second) {
                            it->second = currentShard;
                        }
                        activitySourceHeadShards[source].insert(currentShard);
                    }
                }
                if (activityBatchDispatchEnabled && currentOpBatchOrdinal >= 0) {
                    for (const auto& source : currentOpDirectActivitySources) {
                        activitySourceHeadBatches[source].insert(currentOpBatchOrdinal);
                    }
                }
            }

            void emitShardStatement(const std::string& stmt) {
                ensureShardSpace(static_cast<int>(stmt.length()));
                std::string* shard = getCurrentShardBuffer();
                markCurrentShardActivity();
                shard->append(stmt);
                shard->push_back('\n');
                currentShardSize += static_cast<int>(stmt.length() + 1U);
            }

            void emitShardAssignment(const std::string& lhs, const std::string& rhs) {
                std::string stmt;
                const bool tempVecLhs = lhs.rfind("evalTemps_->tempVec[", 0) == 0;
                const bool simpleWideRhs = rhs.rfind("evalTemps_->tempVec[", 0) == 0 ||
                                           rhs.rfind("state_->stateVec[", 0) == 0;
                if (tempVecLhs && simpleWideRhs && rhs.find('(') == std::string::npos) {
                    const auto fixedWordCount = fixedWideAssignWordCount(lhs, tempVecWidths);
                    stmt.append(fixedWordCount ? wideAssignFunctionName(*fixedWordCount) : "wolvrix_gsim_assign_bits");
                    stmt.append("(");
                    stmt.append(lhs);
                    stmt.append(", ");
                    stmt.append(rhs);
                    stmt.append(")");
                } else if (tempVecLhs && isWideStorageTernaryExpr(rhs)) {
                    const bool ternaryUsesVariableWidthPort = rhs.find("input_") != std::string::npos ||
                                                              rhs.find("output_") != std::string::npos;
                    const auto fixedWordCount = ternaryUsesVariableWidthPort
                        ? std::optional<std::size_t>{}
                        : fixedWideAssignWordCount(lhs, tempVecWidths);
                    stmt.append(fixedWordCount ? wideAssignFunctionName(*fixedWordCount) : "wolvrix_gsim_assign_bits");
                    stmt.append("(");
                    stmt.append(lhs);
                    stmt.append(", ");
                    stmt.append(rhs);
                    stmt.append(")");
                } else if (tempVecLhs && rhs.rfind("std::vector<std::uint64_t>{", 0) == 0) {
                    const auto bracePos = rhs.find('{');
                    stmt.append("wolvrix_gsim_assign_bits(");
                    stmt.append(lhs);
                    stmt.append(", ");
                    stmt.append(rhs.substr(bracePos));
                    stmt.append(")");
                } else {
                    stmt.append(lhs);
                    stmt.append(" = ");
                    stmt.append(rhs);
                }
                stmt.append(";\n");
                ensureShardSpace(static_cast<int>(stmt.size()));
                std::string* shard = getCurrentShardBuffer();
                markCurrentShardActivity();
                shard->append(stmt);
                currentShardSize += static_cast<int>(stmt.size());
            }

            void emitChangedScalarShardAssignment(const wolvrix::lib::grh::ValueId& valueId,
                                                  const std::string& lhs,
                                                  const std::string& rhs) {
                const std::string token = "/*WOLVRIX_GSIM_VALUE_FANOUT_" + std::to_string(changeFanoutTokenCount++) + "*/";
                valueChangeFanoutTokens[valueId] = token;
                valueChangeFanoutLhs[valueId] = lhs;
                valueChangeFanoutRhs[valueId] = rhs;
                changeTrackedValues.insert(valueId);

                const int estimatedSize = std::max(
                    static_cast<int>(token.size() + 1U),
                    static_cast<int>(lhs.size() + rhs.size() + static_cast<std::size_t>(changedValueFanoutEstimateBytes)));
                ensureShardSpace(estimatedSize);
                std::string* shard = getCurrentShardBuffer();
                markCurrentShardActivity();
                valueChangeFanoutTokenShard[valueId] = currentShard;
                shard->append(token);
                shard->push_back('\n');
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
                if (enableActivityWatermark && !activityBatchStrictDispatch && width <= 64) {
                    emitChangedScalarShardAssignment(valueId, resultRef, expr);
                    return;
                }
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
            bool globalPreReg = false;
        };

        struct SequentialStoragePools
        {
            std::size_t stateU8 = 0;
            std::size_t stateU16 = 0;
            std::size_t stateU32 = 0;
            std::size_t stateU64 = 0;
            std::size_t stateVec = 0;
        };

        std::map<int, std::uint64_t> shardWordMasksFor(const std::set<int>& shards)
        {
            std::map<int, std::uint64_t> masks;
            for (int shard : shards) {
                if (shard < 0) {
                    continue;
                }
                masks[shard / 64] |= (std::uint64_t{1} << (shard % 64));
            }
            return masks;
        }

        void emitShardWordMaskActivation(std::ostream& os,
                                          const std::map<int, std::uint64_t>& masks,
                                          std::string_view indent,
                                          std::string_view suffix)
        {
            for (const auto& [word, mask] : masks) {
                os << indent << "activate_shard_mask(" << word << "U, UINT64_C(" << mask << "));" << suffix;
            }
        }

        std::string buildShardWordMaskActivation(const std::map<int, std::uint64_t>& masks)
        {
            std::ostringstream os;
            emitShardWordMaskActivation(os, masks, " ", "");
            return os.str();
        }

        std::uint32_t internChangedFanoutRange(CodegenState& state,
                                               const std::map<int, std::uint64_t>& masks)
        {
            std::ostringstream key;
            for (const auto& [word, mask] : masks) {
                key << word << ':' << mask << ';';
            }
            const std::string keyText = key.str();
            if (const auto it = state.changedFanoutRangeIds.find(keyText);
                it != state.changedFanoutRangeIds.end()) {
                return it->second;
            }

            const auto rangeId = static_cast<std::uint32_t>(state.changedFanoutRanges.size());
            const auto offset = static_cast<std::uint32_t>(state.changedFanoutMasks.size());
            const auto count = static_cast<std::uint32_t>(masks.size());
            state.changedFanoutRanges.emplace_back(offset, count);
            for (const auto& [word, mask] : masks) {
                state.changedFanoutMasks.emplace_back(static_cast<std::uint32_t>(word), mask);
            }
            state.changedFanoutRangeIds.emplace(keyText, rangeId);
            return rangeId;
        }

        std::uint32_t internShardActivationRange(CodegenState& state,
                                                 const std::map<int, std::uint64_t>& masks)
        {
            std::ostringstream key;
            for (const auto& [word, mask] : masks) {
                key << word << ':' << mask << ';';
            }
            const std::string keyText = key.str();
            if (const auto it = state.shardActivationRangeIds.find(keyText);
                it != state.shardActivationRangeIds.end()) {
                return it->second;
            }

            const auto rangeId = static_cast<std::uint32_t>(state.shardActivationRanges.size());
            const auto offset = static_cast<std::uint32_t>(state.shardActivationMasks.size());
            const auto count = static_cast<std::uint32_t>(masks.size());
            state.shardActivationRanges.emplace_back(offset, count);
            for (const auto& [word, mask] : masks) {
                state.shardActivationMasks.emplace_back(static_cast<std::uint32_t>(word), mask);
            }
            state.shardActivationRangeIds.emplace(keyText, rangeId);
            return rangeId;
        }

        std::string buildShardActivation(const CodegenState& state,
                                         const std::map<int, std::uint64_t>& masks)
        {
            if (masks.empty()) {
                return {};
            }
            if (static_cast<int>(masks.size()) <= state.shardActivationInlineMaskLimit) {
                return buildShardWordMaskActivation(masks);
            }
            std::ostringstream key;
            for (const auto& [word, mask] : masks) {
                key << word << ':' << mask << ';';
            }
            if (const auto it = state.shardActivationRangeIds.find(key.str());
                it != state.shardActivationRangeIds.end()) {
                return " activate_shard_mask_range(" + std::to_string(it->second) + "U);";
            }
            return buildShardWordMaskActivation(masks);
        }

        void emitBatchWordMaskActivation(std::ostream& os,
                                         const std::map<int, std::uint64_t>& masks,
                                         std::string_view indent,
                                         std::string_view suffix)
        {
            for (const auto& [word, mask] : masks) {
                os << indent << "activate_batch_mask(" << word << "U, UINT64_C(" << mask << "));" << suffix;
            }
        }

        std::string buildChangedFanoutActivation(CodegenState& state,
                                                 const std::map<int, std::uint64_t>& masks)
        {
            if (masks.empty()) {
                return {};
            }
            if (static_cast<int>(masks.size()) <= state.changedValueFanoutInlineMaskLimit) {
                return buildShardWordMaskActivation(masks);
            }
            const std::uint32_t rangeId = internChangedFanoutRange(state, masks);
            return " activate_changed_fanout(" + std::to_string(rangeId) + "U);";
        }

        void precomputeShardSuccessorActivationRanges(CodegenState& state)
        {
            if (!state.enableSharding || !state.enableActivityWatermark ||
                state.shardCount() <= 0 || state.shardSuccessors.empty()) {
                return;
            }
            for (int shard = 0; shard < static_cast<int>(state.shardSuccessors.size()); ++shard) {
                std::map<int, std::uint64_t> crossWordMasks;
                const int activeWord = shard / 64;
                for (int successor : state.shardSuccessors[static_cast<std::size_t>(shard)]) {
                    if (successor < 0) {
                        continue;
                    }
                    const int word = successor / 64;
                    if (word == activeWord) {
                        continue;
                    }
                    crossWordMasks[word] |= (std::uint64_t{1} << (successor % 64));
                }
                if (static_cast<int>(crossWordMasks.size()) > state.shardActivationInlineMaskLimit) {
                    internShardActivationRange(state, crossWordMasks);
                }
            }
        }

        void precomputeActivitySourceActivationRange(CodegenState& state,
                                                     const std::vector<std::string>& sources)
        {
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
            const auto masks = shardWordMasksFor(firstShards);
            if (static_cast<int>(masks.size()) > state.shardActivationInlineMaskLimit) {
                internShardActivationRange(state, masks);
            }
        }

        void precomputeSequentialChunkActivityActivationRanges(CodegenState& state,
                                                              const std::vector<SequentialChunkPlan>& sequentialChunks)
        {
            if (!state.enableSharding || !state.enableActivityWatermark || state.shardCount() <= 0) {
                return;
            }
            for (const auto& chunk : sequentialChunks) {
                if (!chunk.regNames.empty()) {
                    precomputeActivitySourceActivationRange(state, chunk.regNames);
                }
                const bool tracksStatementDirty =
                    !chunk.stmts.empty() && chunk.stmtDirtyOnCommit.size() == chunk.stmts.size();
                if (!tracksStatementDirty) {
                    continue;
                }
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
                if (!hasDirtyStatementWithoutSources) {
                    precomputeActivitySourceActivationRange(state, dirtyActivitySources);
                }
            }
        }

        void removeStaticallyCoveredFanoutMasks(CodegenState& state,
                                                int shard,
                                                std::map<int, std::uint64_t>& masks)
        {
            if (shard < 0 || shard >= static_cast<int>(state.shardSuccessors.size()) || masks.empty()) {
                return;
            }
            const auto coveredMasks = shardWordMasksFor(state.shardSuccessors[static_cast<std::size_t>(shard)]);
            for (const auto& [word, coveredMask] : coveredMasks) {
                auto it = masks.find(word);
                if (it == masks.end()) {
                    continue;
                }
                it->second &= ~coveredMask;
                if (it->second == 0) {
                    masks.erase(it);
                }
            }
        }

        void replaceShardTokensInOnePass(
            std::string& text,
            const std::unordered_map<std::string, std::string>& replacements)
        {
            if (text.empty() || replacements.empty()) {
                return;
            }
            constexpr std::string_view marker = "/*WOLVRIX_GSIM_VALUE_FANOUT_";
            std::string result;
            result.reserve(text.size());
            std::size_t cursor = 0;
            while (true) {
                const std::size_t tokenStart = text.find(marker, cursor);
                if (tokenStart == std::string::npos) {
                    result.append(text, cursor, std::string::npos);
                    break;
                }
                result.append(text, cursor, tokenStart - cursor);
                const std::size_t tokenEnd = text.find("*/", tokenStart + marker.size());
                if (tokenEnd == std::string::npos) {
                    result.append(text, tokenStart, std::string::npos);
                    break;
                }
                const std::string token = text.substr(tokenStart, tokenEnd + 2U - tokenStart);
                if (const auto it = replacements.find(token); it != replacements.end()) {
                    result.append(it->second);
                } else {
                    result.append(token);
                }
                cursor = tokenEnd + 2U;
            }
            text.swap(result);
        }

        struct ParsedLatchStatement
        {
            std::string condition;
            std::string body;
        };

        std::optional<ParsedLatchStatement> parseGeneratedLatchStatement(std::string_view stmt)
        {
            const std::string trimmed = trimCopy(std::string(stmt));
            constexpr std::string_view prefix = "if (";
            constexpr std::string_view separator = ") { ";
            constexpr std::string_view suffix = " }";
            if (trimmed.rfind(prefix, 0) != 0 ||
                trimmed.rfind(suffix) != trimmed.size() - suffix.size()) {
                return std::nullopt;
            }
            const std::size_t separatorPos = trimmed.find(separator, prefix.size());
            if (separatorPos == std::string::npos || separatorPos <= prefix.size()) {
                return std::nullopt;
            }
            const std::size_t bodyBegin = separatorPos + separator.size();
            ParsedLatchStatement parsed;
            parsed.condition = trimmed.substr(prefix.size(), separatorPos - prefix.size());
            parsed.body = trimmed.substr(bodyBegin, trimmed.size() - suffix.size() - bodyBegin);
            if (parsed.condition.empty() || parsed.body.empty()) {
                return std::nullopt;
            }
            return parsed;
        }

        void emitCoalescedLatchStatements(std::ostream& os,
                                          const std::vector<std::string>& latchStmts,
                                          std::string_view indent)
        {
            std::string activeCondition;
            std::vector<std::string> activeBodies;
            auto flush = [&]() {
                if (activeBodies.empty()) {
                    return;
                }
                if (activeBodies.size() == 1U) {
                    os << indent << "if (" << activeCondition << ") { " << activeBodies.front() << " }\n";
                } else {
                    os << indent << "if (" << activeCondition << ") {\n";
                    for (const auto& body : activeBodies) {
                        os << indent << "    " << body << "\n";
                    }
                    os << indent << "}\n";
                }
                activeCondition.clear();
                activeBodies.clear();
            };

            for (const auto& stmt : latchStmts) {
                std::string body = stmt;
                if (body.rfind("        ", 0) == 0) {
                    body.erase(0, 8);
                }
                const auto parsed = parseGeneratedLatchStatement(body);
                if (!parsed) {
                    flush();
                    os << indent << trimCopy(std::move(body)) << "\n";
                    continue;
                }
                if (activeBodies.empty()) {
                    activeCondition = parsed->condition;
                } else if (parsed->condition != activeCondition) {
                    flush();
                    activeCondition = parsed->condition;
                }
                activeBodies.push_back(parsed->body);
            }
            flush();
        }

        struct ChangedStateSpanLine
        {
            std::string hitVar;
            std::string typeSuffix;
            std::size_t tempOffset = 0;
            std::size_t stateOffset = 0;
            std::string original;
        };

        bool parseChangedStateSpanLine(std::string_view line, ChangedStateSpanLine& parsed)
        {
            constexpr std::string_view opPrefix = " |= wolvrix_gsim_assign_if_changed(evalTemps_->temp";
            constexpr std::string_view statePrefix = "], state_->state";
            constexpr std::string_view suffix = "]);";
            const auto opPos = line.find(opPrefix);
            if (opPos == std::string_view::npos || opPos == 0) {
                return false;
            }
            const auto typeBegin = opPos + opPrefix.size();
            const auto typeEnd = line.find('[', typeBegin);
            if (typeEnd == std::string_view::npos) {
                return false;
            }
            const std::string_view typeSuffix = line.substr(typeBegin, typeEnd - typeBegin);
            if (typeSuffix != "U8" && typeSuffix != "U16" && typeSuffix != "U32" && typeSuffix != "U64") {
                return false;
            }
            const auto tempBegin = typeEnd + 1U;
            const auto tempEnd = line.find(statePrefix, tempBegin);
            if (tempEnd == std::string_view::npos) {
                return false;
            }
            const auto stateTypeBegin = tempEnd + statePrefix.size();
            const auto stateTypeEnd = line.find('[', stateTypeBegin);
            if (stateTypeEnd == std::string_view::npos ||
                line.substr(stateTypeBegin, stateTypeEnd - stateTypeBegin) != typeSuffix) {
                return false;
            }
            const auto stateBegin = stateTypeEnd + 1U;
            const auto stateEnd = line.find(suffix, stateBegin);
            if (stateEnd == std::string_view::npos || stateEnd + suffix.size() != line.size()) {
                return false;
            }
            auto parseUnsigned = [](std::string_view text, std::size_t& value) {
                if (text.empty()) {
                    return false;
                }
                std::size_t result = 0;
                for (char ch : text) {
                    if (ch < '0' || ch > '9') {
                        return false;
                    }
                    result = result * 10U + static_cast<std::size_t>(ch - '0');
                }
                value = result;
                return true;
            };
            std::size_t tempOffset = 0;
            std::size_t stateOffset = 0;
            if (!parseUnsigned(line.substr(tempBegin, tempEnd - tempBegin), tempOffset) ||
                !parseUnsigned(line.substr(stateBegin, stateEnd - stateBegin), stateOffset)) {
                return false;
            }
            parsed.hitVar.assign(line.substr(0, opPos));
            parsed.typeSuffix.assign(typeSuffix);
            parsed.tempOffset = tempOffset;
            parsed.stateOffset = stateOffset;
            parsed.original.assign(line);
            return true;
        }

        void appendChangedStateSpanRun(std::string& out, const std::vector<ChangedStateSpanLine>& run)
        {
            constexpr std::size_t minSpan = 16;
            if (run.size() >= minSpan) {
                out.append(run.front().hitVar);
                out.append(" |= wolvrix_gsim_assign_scalar_span_if_changed(&evalTemps_->temp");
                out.append(run.front().typeSuffix);
                out.push_back('[');
                out.append(std::to_string(run.front().tempOffset));
                out.append("], &state_->state");
                out.append(run.front().typeSuffix);
                out.push_back('[');
                out.append(std::to_string(run.front().stateOffset));
                out.append("], ");
                out.append(std::to_string(run.size()));
                out.append("U);\n");
                return;
            }
            for (const auto& line : run) {
                out.append(line.original);
                out.push_back('\n');
            }
        }

        void coalesceChangedStateSpanAssignments(std::string& text)
        {
            if (text.find("wolvrix_gsim_assign_if_changed(evalTemps_->temp") == std::string::npos ||
                text.find("state_->state") == std::string::npos) {
                return;
            }

            std::string result;
            result.reserve(text.size());
            std::vector<ChangedStateSpanLine> run;
            auto flushRun = [&]() {
                if (!run.empty()) {
                    appendChangedStateSpanRun(result, run);
                    run.clear();
                }
            };

            std::size_t cursor = 0;
            while (cursor < text.size()) {
                const std::size_t newline = text.find('\n', cursor);
                const bool hasNewline = newline != std::string::npos;
                const std::size_t lineEnd = hasNewline ? newline : text.size();
                const std::string_view line(text.data() + cursor, lineEnd - cursor);

                ChangedStateSpanLine parsed;
                if (parseChangedStateSpanLine(line, parsed)) {
                    const bool extends = !run.empty() &&
                        parsed.hitVar == run.back().hitVar &&
                        parsed.typeSuffix == run.back().typeSuffix &&
                        parsed.tempOffset == run.back().tempOffset + 1U &&
                        parsed.stateOffset == run.back().stateOffset + 1U;
                    if (!extends) {
                        flushRun();
                    }
                    run.push_back(std::move(parsed));
                } else {
                    flushRun();
                    result.append(line);
                    if (hasNewline) {
                        result.push_back('\n');
                    }
                }

                if (!hasNewline) {
                    break;
                }
                cursor = newline + 1U;
            }
            flushRun();
            text.swap(result);
        }

        struct ScalarStorageRef
        {
            std::string storagePrefix;
            std::string typeSuffix;
            std::size_t offset = 0;
        };

        bool parseUnsignedSize(std::string_view text, std::size_t& value)
        {
            if (text.empty()) {
                return false;
            }
            std::size_t result = 0;
            for (char ch : text) {
                if (ch < '0' || ch > '9') {
                    return false;
                }
                result = result * 10U + static_cast<std::size_t>(ch - '0');
            }
            value = result;
            return true;
        }

        bool parseScalarStorageRef(std::string_view expr, ScalarStorageRef& ref)
        {
            constexpr std::array<std::string_view, 2> prefixes = {
                "evalTemps_->temp",
                "state_->state",
            };
            for (const auto prefix : prefixes) {
                if (expr.rfind(prefix, 0) != 0) {
                    continue;
                }
                const auto typeBegin = prefix.size();
                const auto typeEnd = expr.find('[', typeBegin);
                if (typeEnd == std::string_view::npos) {
                    return false;
                }
                const std::string_view typeSuffix = expr.substr(typeBegin, typeEnd - typeBegin);
                if (typeSuffix != "U8" && typeSuffix != "U16" &&
                    typeSuffix != "U32" && typeSuffix != "U64") {
                    return false;
                }
                const auto indexBegin = typeEnd + 1U;
                const auto indexEnd = expr.find(']', indexBegin);
                if (indexEnd == std::string_view::npos || indexEnd + 1U != expr.size()) {
                    return false;
                }
                std::size_t offset = 0;
                if (!parseUnsignedSize(expr.substr(indexBegin, indexEnd - indexBegin), offset)) {
                    return false;
                }
                ref.storagePrefix.assign(prefix);
                ref.typeSuffix.assign(typeSuffix);
                ref.offset = offset;
                return true;
            }
            return false;
        }

        bool rangesOverlap(std::size_t lhsOffset, std::size_t rhsOffset, std::size_t count)
        {
            return lhsOffset < rhsOffset + count && rhsOffset < lhsOffset + count;
        }

        bool isChangedFanoutHitVar(std::string_view text)
        {
            constexpr std::string_view prefix = "wolvrix_gsim_changed_fanout_hit_";
            if (text.rfind(prefix, 0) != 0 || text.size() == prefix.size()) {
                return false;
            }
            for (std::size_t i = prefix.size(); i < text.size(); ++i) {
                const char ch = text[i];
                if (ch < '0' || ch > '9') {
                    return false;
                }
            }
            return true;
        }

        std::size_t minScalarCopySpan(std::string_view typeSuffix)
        {
            return typeSuffix == "U8" ? 8U : 4U;
        }

        std::size_t minScalarMuxSpan(std::string_view typeSuffix)
        {
            return typeSuffix == "U8" ? 4U : 2U;
        }

        struct ChangedTempCopySpanLine
        {
            std::string hitVar;
            std::string typeSuffix;
            std::size_t dstOffset = 0;
            std::size_t srcOffset = 0;
            std::string original;
        };

        bool parseChangedTempCopySpanLine(std::string_view line, ChangedTempCopySpanLine& parsed)
        {
            constexpr std::string_view opPrefix = " |= wolvrix_gsim_assign_if_changed(";
            constexpr std::string_view separator = ", ";
            constexpr std::string_view suffix = ");";
            const auto opPos = line.find(opPrefix);
            if (opPos == std::string_view::npos || opPos == 0) {
                return false;
            }
            const auto lhsBegin = opPos + opPrefix.size();
            const auto lhsEnd = line.find(separator, lhsBegin);
            if (lhsEnd == std::string_view::npos) {
                return false;
            }
            const auto rhsBegin = lhsEnd + separator.size();
            const auto rhsEnd = line.find(suffix, rhsBegin);
            if (rhsEnd == std::string_view::npos || rhsEnd + suffix.size() != line.size()) {
                return false;
            }

            const std::string_view hitVar = line.substr(0, opPos);
            if (!isChangedFanoutHitVar(hitVar)) {
                return false;
            }
            ScalarStorageRef lhs;
            ScalarStorageRef rhs;
            if (!parseScalarStorageRef(line.substr(lhsBegin, lhsEnd - lhsBegin), lhs) ||
                !parseScalarStorageRef(line.substr(rhsBegin, rhsEnd - rhsBegin), rhs)) {
                return false;
            }
            if (lhs.storagePrefix != "evalTemps_->temp" ||
                rhs.storagePrefix != "evalTemps_->temp" ||
                lhs.typeSuffix != rhs.typeSuffix) {
                return false;
            }

            parsed.hitVar.assign(hitVar);
            parsed.typeSuffix = lhs.typeSuffix;
            parsed.dstOffset = lhs.offset;
            parsed.srcOffset = rhs.offset;
            parsed.original.assign(line);
            return true;
        }

        void appendChangedTempCopySpanRun(std::string& out, const std::vector<ChangedTempCopySpanLine>& run)
        {
            if (run.size() >= minScalarCopySpan(run.front().typeSuffix) &&
                !rangesOverlap(run.front().dstOffset, run.front().srcOffset, run.size())) {
                out.append(run.front().hitVar);
                out.append(" |= wolvrix_gsim_assign_scalar_span_if_changed(&evalTemps_->temp");
                out.append(run.front().typeSuffix);
                out.push_back('[');
                out.append(std::to_string(run.front().dstOffset));
                out.append("], &evalTemps_->temp");
                out.append(run.front().typeSuffix);
                out.push_back('[');
                out.append(std::to_string(run.front().srcOffset));
                out.append("], ");
                out.append(std::to_string(run.size()));
                out.append("U);\n");
                return;
            }
            for (const auto& line : run) {
                out.append(line.original);
                out.push_back('\n');
            }
        }

        struct ChangedMuxSpanLine
        {
            std::string hitVar;
            std::string typeSuffix;
            std::size_t dstOffset = 0;
            ScalarStorageRef condition;
            std::string conditionExpr;
            ScalarStorageRef trueValue;
            ScalarStorageRef falseValue;
            std::string original;
        };

        bool parseChangedMuxSpanLine(std::string_view line, ChangedMuxSpanLine& parsed)
        {
            constexpr std::string_view opPrefix = " |= wolvrix_gsim_assign_if_changed(";
            constexpr std::string_view separator = ", ";
            constexpr std::string_view suffix = ");";
            const auto opPos = line.find(opPrefix);
            if (opPos == std::string_view::npos || opPos == 0) {
                return false;
            }
            const auto lhsBegin = opPos + opPrefix.size();
            const auto lhsEnd = line.find(separator, lhsBegin);
            if (lhsEnd == std::string_view::npos) {
                return false;
            }
            const auto rhsBegin = lhsEnd + separator.size();
            const auto rhsEnd = line.find(suffix, rhsBegin);
            if (rhsEnd == std::string_view::npos || rhsEnd + suffix.size() != line.size()) {
                return false;
            }
            const std::string_view hitVar = line.substr(0, opPos);
            if (!isChangedFanoutHitVar(hitVar)) {
                return false;
            }
            std::string_view rhs = line.substr(rhsBegin, rhsEnd - rhsBegin);
            if (rhs.size() < 5 || rhs.front() != '(' || rhs.back() != ')') {
                return false;
            }
            rhs.remove_prefix(1);
            rhs.remove_suffix(1);
            const auto question = rhs.find(" ? ");
            const auto colon = question == std::string_view::npos
                ? std::string_view::npos
                : rhs.find(" : ", question + 3U);
            if (question == std::string_view::npos || colon == std::string_view::npos ||
                rhs.find(" ? ", question + 3U) != std::string_view::npos ||
                rhs.find(" : ", colon + 3U) != std::string_view::npos) {
                return false;
            }

            ScalarStorageRef lhs;
            ScalarStorageRef condition;
            ScalarStorageRef trueValue;
            ScalarStorageRef falseValue;
            const auto conditionExpr = rhs.substr(0, question);
            if (!parseScalarStorageRef(line.substr(lhsBegin, lhsEnd - lhsBegin), lhs) ||
                !parseScalarStorageRef(conditionExpr, condition) ||
                !parseScalarStorageRef(rhs.substr(question + 3U, colon - (question + 3U)), trueValue) ||
                !parseScalarStorageRef(rhs.substr(colon + 3U), falseValue)) {
                return false;
            }
            if (lhs.storagePrefix != "evalTemps_->temp" ||
                lhs.typeSuffix != trueValue.typeSuffix ||
                lhs.typeSuffix != falseValue.typeSuffix) {
                return false;
            }

            parsed.hitVar.assign(hitVar);
            parsed.typeSuffix = lhs.typeSuffix;
            parsed.dstOffset = lhs.offset;
            parsed.condition = std::move(condition);
            parsed.conditionExpr.assign(conditionExpr);
            parsed.trueValue = std::move(trueValue);
            parsed.falseValue = std::move(falseValue);
            parsed.original.assign(line);
            return true;
        }

        bool changedMuxSpanRunIsSafe(const std::vector<ChangedMuxSpanLine>& run)
        {
            const auto& first = run.front();
            const auto count = run.size();
            if ((first.trueValue.storagePrefix == "evalTemps_->temp" &&
                 first.trueValue.typeSuffix == first.typeSuffix &&
                 rangesOverlap(first.dstOffset, first.trueValue.offset, count)) ||
                (first.falseValue.storagePrefix == "evalTemps_->temp" &&
                 first.falseValue.typeSuffix == first.typeSuffix &&
                 rangesOverlap(first.dstOffset, first.falseValue.offset, count))) {
                return false;
            }
            if (first.condition.storagePrefix == "evalTemps_->temp" &&
                first.condition.typeSuffix == first.typeSuffix &&
                first.condition.offset >= first.dstOffset &&
                first.condition.offset < first.dstOffset + count) {
                return false;
            }
            return true;
        }

        void appendScalarRefAddress(std::string& out, const ScalarStorageRef& ref)
        {
            out.push_back('&');
            out.append(ref.storagePrefix);
            out.append(ref.typeSuffix);
            out.push_back('[');
            out.append(std::to_string(ref.offset));
            out.push_back(']');
        }

        void appendChangedMuxSpanRun(std::string& out, const std::vector<ChangedMuxSpanLine>& run)
        {
            if (run.size() >= minScalarMuxSpan(run.front().typeSuffix) && changedMuxSpanRunIsSafe(run)) {
                out.append(run.front().hitVar);
                out.append(" |= wolvrix_gsim_assign_mux_span_if_changed(&evalTemps_->temp");
                out.append(run.front().typeSuffix);
                out.push_back('[');
                out.append(std::to_string(run.front().dstOffset));
                out.append("], ");
                out.append(run.front().conditionExpr);
                out.append(" != 0, ");
                appendScalarRefAddress(out, run.front().trueValue);
                out.append(", ");
                appendScalarRefAddress(out, run.front().falseValue);
                out.append(", ");
                out.append(std::to_string(run.size()));
                out.append("U);\n");
                return;
            }
            for (const auto& line : run) {
                out.append(line.original);
                out.push_back('\n');
            }
        }

        void coalesceChangedTempCopyAndMuxSpanAssignments(std::string& text)
        {
            if (text.find("wolvrix_gsim_assign_if_changed(evalTemps_->temp") == std::string::npos ||
                text.find("evalTemps_->temp") == std::string::npos) {
                return;
            }

            std::string result;
            result.reserve(text.size());
            std::vector<ChangedTempCopySpanLine> copyRun;
            std::vector<ChangedMuxSpanLine> muxRun;
            auto flushCopyRun = [&]() {
                if (!copyRun.empty()) {
                    appendChangedTempCopySpanRun(result, copyRun);
                    copyRun.clear();
                }
            };
            auto flushMuxRun = [&]() {
                if (!muxRun.empty()) {
                    appendChangedMuxSpanRun(result, muxRun);
                    muxRun.clear();
                }
            };
            auto flushAll = [&]() {
                flushCopyRun();
                flushMuxRun();
            };

            std::size_t cursor = 0;
            while (cursor < text.size()) {
                const std::size_t newline = text.find('\n', cursor);
                const bool hasNewline = newline != std::string::npos;
                const std::size_t lineEnd = hasNewline ? newline : text.size();
                const std::string_view line(text.data() + cursor, lineEnd - cursor);

                ChangedTempCopySpanLine copyLine;
                ChangedMuxSpanLine muxLine;
                if (parseChangedTempCopySpanLine(line, copyLine)) {
                    flushMuxRun();
                    const bool extends = !copyRun.empty() &&
                        copyLine.hitVar == copyRun.back().hitVar &&
                        copyLine.typeSuffix == copyRun.back().typeSuffix &&
                        copyLine.dstOffset == copyRun.back().dstOffset + 1U &&
                        copyLine.srcOffset == copyRun.back().srcOffset + 1U;
                    if (!extends) {
                        flushCopyRun();
                    }
                    copyRun.push_back(std::move(copyLine));
                } else if (parseChangedMuxSpanLine(line, muxLine)) {
                    flushCopyRun();
                    const bool extends = !muxRun.empty() &&
                        muxLine.hitVar == muxRun.back().hitVar &&
                        muxLine.typeSuffix == muxRun.back().typeSuffix &&
                        muxLine.conditionExpr == muxRun.back().conditionExpr &&
                        muxLine.trueValue.storagePrefix == muxRun.back().trueValue.storagePrefix &&
                        muxLine.falseValue.storagePrefix == muxRun.back().falseValue.storagePrefix &&
                        muxLine.dstOffset == muxRun.back().dstOffset + 1U &&
                        muxLine.trueValue.offset == muxRun.back().trueValue.offset + 1U &&
                        muxLine.falseValue.offset == muxRun.back().falseValue.offset + 1U;
                    if (!extends) {
                        flushMuxRun();
                    }
                    muxRun.push_back(std::move(muxLine));
                } else {
                    flushAll();
                    result.append(line);
                    if (hasNewline) {
                        result.push_back('\n');
                    }
                }

                if (!hasNewline) {
                    break;
                }
                cursor = newline + 1U;
            }
            flushAll();
            text.swap(result);
        }

        void finalizeChangeTrackedFanout(CodegenState& state)
        {
            if (!state.enableSharding || !state.enableActivityWatermark ||
                state.valueChangeFanoutTokens.empty()) {
                return;
            }

            struct PendingFanoutReplacement {
                std::string token;
                std::string lhs;
                std::string rhs;
                std::string activation;
                bool hasActivation = false;
            };
            struct FanoutActivationGroup {
                std::string activation;
                std::map<int, std::uint64_t> masks;
                std::size_t count = 0;
                std::string hitVar;
            };
            struct ShardFanoutReplacements {
                std::vector<PendingFanoutReplacement> replacements;
                std::vector<FanoutActivationGroup> groups;
                std::unordered_map<std::string, std::size_t> groupByActivation;
            };

            std::vector<ShardFanoutReplacements> replacementsByShard(state.shardBuffers.size());
            for (const auto& [valueId, token] : state.valueChangeFanoutTokens) {
                const auto lhsIt = state.valueChangeFanoutLhs.find(valueId);
                const auto rhsIt = state.valueChangeFanoutRhs.find(valueId);
                const auto shardIt = state.valueChangeFanoutTokenShard.find(valueId);
                if (lhsIt == state.valueChangeFanoutLhs.end() ||
                    rhsIt == state.valueChangeFanoutRhs.end() ||
                    shardIt == state.valueChangeFanoutTokenShard.end() ||
                    shardIt->second < 0 ||
                    shardIt->second >= static_cast<int>(state.shardBuffers.size())) {
                    continue;
                }

                PendingFanoutReplacement replacement;
                replacement.token = token;
                replacement.lhs = lhsIt->second;
                replacement.rhs = rhsIt->second;
                if (const auto fanoutIt = state.valueChangeFanoutHeadShards.find(valueId);
                    fanoutIt != state.valueChangeFanoutHeadShards.end() && !fanoutIt->second.empty()) {
                    auto fanoutMasks = shardWordMasksFor(fanoutIt->second);
                    removeStaticallyCoveredFanoutMasks(state, shardIt->second, fanoutMasks);
                    replacement.activation = buildChangedFanoutActivation(state, fanoutMasks);
                    replacement.hasActivation = !replacement.activation.empty();
                    if (replacement.hasActivation) {
                        auto& shardReplacements = replacementsByShard[static_cast<std::size_t>(shardIt->second)];
                        auto groupIt = shardReplacements.groupByActivation.find(replacement.activation);
                        if (groupIt == shardReplacements.groupByActivation.end()) {
                            const std::size_t groupIndex = shardReplacements.groups.size();
                            groupIt = shardReplacements.groupByActivation.emplace(replacement.activation, groupIndex).first;
                            shardReplacements.groups.push_back({replacement.activation, fanoutMasks, 0, {}});
                        }
                        ++shardReplacements.groups[groupIt->second].count;
                    }
                }
                replacementsByShard[static_cast<std::size_t>(shardIt->second)].replacements.push_back(std::move(replacement));
            }
            for (auto& shardReplacements : replacementsByShard) {
                std::size_t localHitIndex = 0;
                for (auto& group : shardReplacements.groups) {
                    if (group.count > 1) {
                        group.hitVar = "wolvrix_gsim_changed_fanout_hit_" + std::to_string(localHitIndex++);
                    }
                }
            }
            for (std::size_t shard = 0; shard < replacementsByShard.size(); ++shard) {
                auto& shardReplacements = replacementsByShard[shard];
                if (shardReplacements.replacements.empty()) {
                    continue;
                }
                std::unordered_map<std::string, std::string> replacementsByToken;
                replacementsByToken.reserve(shardReplacements.replacements.size());
                for (const auto& entry : shardReplacements.replacements) {
                    std::string replacement;
                    if (entry.hasActivation) {
                        const auto groupIt = shardReplacements.groupByActivation.find(entry.activation);
                        const FanoutActivationGroup* group = nullptr;
                        if (groupIt != shardReplacements.groupByActivation.end() &&
                            groupIt->second < shardReplacements.groups.size()) {
                            group = &shardReplacements.groups[groupIt->second];
                        }
                        if (group != nullptr && !group->hitVar.empty()) {
                            replacement.append(group->hitVar);
                            replacement.append(" |= wolvrix_gsim_assign_if_changed(");
                            replacement.append(entry.lhs);
                            replacement.append(", ");
                            replacement.append(entry.rhs);
                            replacement.append(");");
                        } else {
                            replacement.append("if (wolvrix_gsim_assign_if_changed(");
                            replacement.append(entry.lhs);
                            replacement.append(", ");
                            replacement.append(entry.rhs);
                            replacement.append(")) {");
                            replacement.append(entry.activation);
                            replacement.append(" }");
                        }
                    } else {
                        replacement.append(entry.lhs);
                        replacement.append(" = ");
                        replacement.append(entry.rhs);
                        replacement.append(";");
                    }
                    replacementsByToken.emplace(entry.token, std::move(replacement));
                }
                replaceShardTokensInOnePass(state.shardBuffers[shard], replacementsByToken);
                coalesceChangedStateSpanAssignments(state.shardBuffers[shard]);
                coalesceChangedTempCopyAndMuxSpanAssignments(state.shardBuffers[shard]);

                std::string declarations;
                std::string flushes;
                std::map<int, std::size_t> batchedWordIndexes;
                auto batchMaskName = [](std::size_t index) {
                    return "wolvrix_gsim_changed_fanout_mask_" + std::to_string(index);
                };
                for (const auto& group : shardReplacements.groups) {
                    if (group.hitVar.empty()) {
                        continue;
                    }
                    declarations.append("bool ");
                    declarations.append(group.hitVar);
                    declarations.append(" = false;\n");
                    for (const auto& [word, mask] : group.masks) {
                        if (mask == 0 || batchedWordIndexes.find(word) != batchedWordIndexes.end()) {
                            continue;
                        }
                        const std::size_t index = batchedWordIndexes.size();
                        batchedWordIndexes.emplace(word, index);
                        declarations.append("std::uint64_t ");
                        declarations.append(batchMaskName(index));
                        declarations.append(" = 0;\n");
                    }
                }
                for (const auto& group : shardReplacements.groups) {
                    if (group.hitVar.empty()) {
                        continue;
                    }
                    flushes.append("if (");
                    flushes.append(group.hitVar);
                    flushes.append(") {");
                    if (group.masks.empty()) {
                        flushes.append(group.activation);
                    } else {
                        for (const auto& [word, mask] : group.masks) {
                            if (mask == 0) {
                                continue;
                            }
                            const auto batchIt = batchedWordIndexes.find(word);
                            if (batchIt == batchedWordIndexes.end()) {
                                continue;
                            }
                            flushes.append(" ");
                            flushes.append(batchMaskName(batchIt->second));
                            flushes.append(" |= UINT64_C(");
                            flushes.append(std::to_string(mask));
                            flushes.append(");");
                        }
                    }
                    flushes.append(" }\n");
                }
                for (const auto& [word, index] : batchedWordIndexes) {
                    const auto maskName = batchMaskName(index);
                    flushes.append("if (");
                    flushes.append(maskName);
                    flushes.append(" != UINT64_C(0)) { activate_shard_mask(");
                    flushes.append(std::to_string(word));
                    flushes.append("U, ");
                    flushes.append(maskName);
                    flushes.append("); }\n");
                }
                if (!declarations.empty()) {
                    state.shardBuffers[shard].insert(0, declarations);
                    state.shardBuffers[shard].append(flushes);
                }
            }
        }

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

        std::optional<std::uint64_t> parseUnsignedScalarLiteralExpr(std::string_view text)
        {
            std::string value = trimCopy(std::string(text));
            while (!value.empty() && (value.back() == 'U' || value.back() == 'u' ||
                                      value.back() == 'L' || value.back() == 'l')) {
                value.pop_back();
            }
            if (value.empty()) {
                return std::nullopt;
            }
            int radix = 10;
            std::size_t digitBegin = 0;
            const auto apostrophe = value.find('\'');
            if (apostrophe != std::string::npos && apostrophe + 1 < value.size()) {
                digitBegin = apostrophe + 1;
                const char base = value[digitBegin];
                if (base == 'b' || base == 'B') {
                    radix = 2;
                    ++digitBegin;
                } else if (base == 'h' || base == 'H') {
                    radix = 16;
                    ++digitBegin;
                } else if (base == 'd' || base == 'D') {
                    radix = 10;
                    ++digitBegin;
                } else if (base == 'o' || base == 'O') {
                    radix = 8;
                    ++digitBegin;
                }
            } else if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
                radix = 16;
                digitBegin = 2;
            }
            bool sawDigit = false;
            std::uint64_t result = 0;
            auto digitValue = [](char ch) {
                if (ch >= '0' && ch <= '9') {
                    return ch - '0';
                }
                if (ch >= 'a' && ch <= 'f') {
                    return 10 + (ch - 'a');
                }
                if (ch >= 'A' && ch <= 'F') {
                    return 10 + (ch - 'A');
                }
                return -1;
            };
            for (std::size_t i = digitBegin; i < value.size(); ++i) {
                const char ch = value[i];
                if (ch == '_' || std::isspace(static_cast<unsigned char>(ch))) {
                    continue;
                }
                if (ch == 'x' || ch == 'X' || ch == 'z' || ch == 'Z' || ch == '?') {
                    return std::nullopt;
                }
                const int digit = digitValue(ch);
                if (digit < 0 || digit >= radix) {
                    return std::nullopt;
                }
                if (result > (std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(digit)) /
                                 static_cast<std::uint64_t>(radix)) {
                    return std::nullopt;
                }
                result = result * static_cast<std::uint64_t>(radix) + static_cast<std::uint64_t>(digit);
                sawDigit = true;
            }
            if (!sawDigit) {
                return std::nullopt;
            }
            return result;
        }

        bool isZeroScalarMaskExpr(std::string_view text)
        {
            const auto value = parseUnsignedScalarLiteralExpr(text);
            return value.has_value() && *value == 0;
        }

        bool isAllOnesScalarMaskExpr(std::string_view text, int64_t width)
        {
            if (width <= 0 || width > 64) {
                return false;
            }
            const auto value = parseUnsignedScalarLiteralExpr(text);
            if (!value) {
                return false;
            }
            const std::uint64_t allOnes = width == 64 ? std::numeric_limits<std::uint64_t>::max()
                                                      : ((1ULL << width) - 1ULL);
            return *value == allOnes;
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

        bool isGlobalPreEdgeDpicTarget(std::string_view target)
        {
            // XiangShan difftest monitor modules are observational edge hooks:
            // they should sample one global pre-register snapshot, while still
            // reusing the no-diff guard classification for CONFIG_NO_DIFFTEST.
            return isNoDiffGuardedDpicTarget(target);
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

        struct GsimScheduleBatchMetadata
        {
            std::string kind;
            int64_t version = 0;
            std::string contract;
            int64_t count = 0;
            std::vector<std::string> names;
            std::vector<int64_t> classIds;
            std::vector<std::string> classNames;
            std::vector<int64_t> flags;
            std::vector<int64_t> topoBatchByPos;
            std::vector<int64_t> firstTopoPos;
            std::vector<int64_t> lastTopoPos;
            std::vector<int64_t> opCounts;
            std::vector<int64_t> succOffsets;
            std::vector<int64_t> succTargets;
            std::vector<int64_t> entryBatches;
            std::vector<int64_t> estimatedLines;
        };

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
            std::set<int> opDirtyReplayDependencyProducerShards;
            std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> changeTrackedDependencyValues;
            const bool opCanUseChangedOperandFanout =
                !results.empty() &&
                kind != OperationKind::kDpicCall &&
                kind != OperationKind::kSystemTask &&
                kind != OperationKind::kLatchWritePort &&
                kind != OperationKind::kRegisterWritePort &&
                kind != OperationKind::kMemoryWritePort &&
                kind != OperationKind::kXMRWrite;
            auto operandUsesChangeTrackedFanout = [&](const wolvrix::lib::grh::ValueId& valueId) {
                return opCanUseChangedOperandFanout &&
                       state.enableSharding && state.enableActivityWatermark &&
                       state.changeTrackedValues.find(valueId) != state.changeTrackedValues.end();
            };
            for (const auto operand : operands)
            {
                const bool changeTrackedOperand = operandUsesChangeTrackedFanout(operand);
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
                        if (changeTrackedOperand) {
                            changeTrackedDependencyValues.insert(operand);
                            opDirtyReplayDependencyProducerShards.insert(producerIt->second);
                        } else {
                            opDependencyProducerShards.insert(producerIt->second);
                        }
                    }
                }
                if (state.enableActivityWatermark)
                {
                    if (const auto shardIt = state.valueActivityFirstShard.find(operand);
                        shardIt != state.valueActivityFirstShard.end())
                    {
                        mergeFirstShard(shardIt->second);
                        if (shardIt->second >= 0) {
                            if (changeTrackedOperand) {
                                opDirtyReplayDependencyProducerShards.insert(shardIt->second);
                            } else {
                                opDependencyProducerShards.insert(shardIt->second);
                            }
                        }
                    }
                    if (const auto producerIt = state.valueProducerShard.find(operand);
                        producerIt != state.valueProducerShard.end() && producerIt->second >= 0)
                    {
                        mergeFirstShard(producerIt->second);
                        if (const auto opIt = state.valueProducerOpIndex.find(operand);
                            opIt != state.valueProducerOpIndex.end() && opIt->second >= 0 &&
                            static_cast<std::size_t>(opIt->second) < state.opProducerFirstShardByIndex.size())
                        {
                            const int producerFirstShard =
                                state.opProducerFirstShardByIndex[static_cast<std::size_t>(opIt->second)];
                            if (producerFirstShard >= 0) {
                                if (changeTrackedOperand) {
                                    opDirtyReplayDependencyProducerShards.insert(producerFirstShard);
                                } else {
                                    opDependencyProducerShards.insert(producerFirstShard);
                                }
                            }
                        }
                    }
                }
            }
            state.currentOpDirtyReplayMask = opDirtyReplayMask;
            state.currentOpFirstEmittedShard = -1;
            state.setCurrentOpActivity(opId.index);
            state.maybeCutShardForCurrentOpActivity();
            if (state.enableActivityWatermark || state.activityBatchDispatchEnabled) {
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
            auto getScalarConstantOperandExpr = [&](size_t idx) -> std::optional<std::string> {
                if (idx >= operands.size() || idx >= operandWidths.size() || operandWidths[idx] > 64) {
                    return std::nullopt;
                }
                const auto defOpId = graph.valueDef(operands[idx]);
                if (!defOpId.valid()) {
                    return std::nullopt;
                }
                const auto defOp = graph.getOperation(defOpId);
                if (defOp.kind() != OperationKind::kConstant) {
                    return std::nullopt;
                }
                auto valueAttr = defOp.attr("constValue");
                if (!valueAttr) {
                    valueAttr = defOp.attr("value");
                }
                if (!valueAttr) {
                    return std::nullopt;
                }
                if (auto *strVal = std::get_if<std::string>(&*valueAttr)) {
                    return convertVerilogConstant(*strVal);
                }
                if (auto *intVal = std::get_if<int64_t>(&*valueAttr)) {
                    return std::to_string(*intVal);
                }
                return std::nullopt;
            };
            std::vector<std::string> operandScalarExprCache(operands.size());
            std::vector<std::uint8_t> operandScalarExprCached(operands.size(), 0);
            auto getOperandScalarExprPreferLiteral = [&](size_t idx) -> const std::string& {
                static const std::string zero = "0";
                if (idx >= operands.size()) {
                    return zero;
                }
                if (operandScalarExprCached[idx] == 0) {
                    if (auto constantExpr = getScalarConstantOperandExpr(idx)) {
                        operandScalarExprCache[idx] = std::move(*constantExpr);
                    } else {
                        operandScalarExprCache[idx] = getOperandExpr(idx);
                    }
                    operandScalarExprCached[idx] = 1;
                }
                return operandScalarExprCache[idx];
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
                const int resultFirstShard = (state.enableSharding && state.currentOpFirstEmittedShard >= 0)
                                                 ? state.currentOpFirstEmittedShard
                                                 : (state.enableSharding ? state.lastEmittedShard : -1);
                const int resultProducerShard = state.enableSharding ? state.lastEmittedShard : -1;
                if (resultProducerShard >= 0) {
                    state.valueProducerShard[results[idx]] = resultProducerShard;
                    state.valueProducerOpIndex[results[idx]] = opId.index;
                    if (opId.index >= 0 && static_cast<std::size_t>(opId.index) < state.opProducerLastShardByIndex.size()) {
                        state.opProducerLastShardByIndex[static_cast<std::size_t>(opId.index)] = resultProducerShard;
                        state.opProducerFirstShardByIndex[static_cast<std::size_t>(opId.index)] =
                            resultFirstShard >= 0 ? resultFirstShard : resultProducerShard;
                    }
                    if (resultFirstShard >= 0 && resultFirstShard != resultProducerShard) {
                        // A single GRH operation can lower to multiple C++ shard
                        // statements (for example wide concat zeroing plus several
                        // OR/store chunks). Activity replay must enter the first
                        // statement and then walk through the operation in order;
                        // jumping directly to the last producer shard leaves stale
                        // partial tempVec contents on indirect replays.
                        for (int shard = resultFirstShard; shard < resultProducerShard; ++shard) {
                            state.addShardSuccessor(shard, shard + 1, opDirtyReplayMask);
                        }
                    }
                    for (const int dependencyShard : opDependencyProducerShards) {
                        if (dependencyShard >= 0 && dependencyShard < resultFirstShard) {
                            state.addShardSuccessor(dependencyShard, resultFirstShard, opDirtyReplayMask);
                        }
                    }
                    if (opDirtyReplayMask != 0) {
                        for (const int dependencyShard : opDirtyReplayDependencyProducerShards) {
                            if (dependencyShard >= 0 && dependencyShard < resultFirstShard) {
                                state.addShardDirtyReplaySuccessor(dependencyShard, resultFirstShard, opDirtyReplayMask);
                            }
                        }
                    }
                    for (const auto& operand : changeTrackedDependencyValues) {
                        const auto producerIt = state.valueProducerShard.find(operand);
                        if (producerIt != state.valueProducerShard.end() && producerIt->second >= 0 &&
                            producerIt->second < resultFirstShard) {
                            state.valueChangeFanoutHeadShards[operand].insert(resultFirstShard);
                        }
                    }
                }
                if (state.enableActivityWatermark) {
                    int activityFirstShard = state.currentOpActivityFirstShard;
                    if (!state.currentOpDirectActivitySources.empty() && resultFirstShard >= 0 &&
                        (activityFirstShard < 0 || resultFirstShard < activityFirstShard)) {
                        activityFirstShard = resultFirstShard;
                    }
                    state.setValueActivityFirstShard(results[idx], activityFirstShard);
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
                                if (const auto wordCount = fixedWideWordCountForWidth(resultWidth)) {
                                    if (tempVecHasAtLeastWords(resultRef, state.tempVecWidths, *wordCount) &&
                                        tempVecHasAtLeastWords(getOperandExpr(0), state.tempVecWidths, *wordCount) &&
                                        tempVecHasAtLeastWords(getOperandExpr(1), state.tempVecWidths, *wordCount)) {
                                        return "wolvrix_gsim_bitwise_and_" + std::to_string(*wordCount) + "_fast(" +
                                               resultRef + ", " + getOperandExpr(0) + ", " + getOperandExpr(1) +
                                               ", " + fixedWideLastMaskExpr(resultWidth) + ");";
                                    }
                                    return "wolvrix_gsim_bitwise_and_" + std::to_string(*wordCount) + "(" +
                                           resultRef + ", " + getOperandExpr(0) + ", " + getOperandExpr(1) +
                                           ", " + fixedWideLastMaskExpr(resultWidth) + ");";
                                }
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
                                if (const auto wordCount = fixedWideWordCountForWidth(resultWidth)) {
                                    if (tempVecHasAtLeastWords(resultRef, state.tempVecWidths, *wordCount) &&
                                        tempVecHasAtLeastWords(getOperandExpr(0), state.tempVecWidths, *wordCount) &&
                                        tempVecHasAtLeastWords(getOperandExpr(1), state.tempVecWidths, *wordCount)) {
                                        return "wolvrix_gsim_bitwise_or_" + std::to_string(*wordCount) + "_fast(" +
                                               resultRef + ", " + getOperandExpr(0) + ", " + getOperandExpr(1) +
                                               ", " + fixedWideLastMaskExpr(resultWidth) + ");";
                                    }
                                    return "wolvrix_gsim_bitwise_or_" + std::to_string(*wordCount) + "(" +
                                           resultRef + ", " + getOperandExpr(0) + ", " + getOperandExpr(1) +
                                           ", " + fixedWideLastMaskExpr(resultWidth) + ");";
                                }
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
                                if (const auto wordCount = fixedWideWordCountForWidth(resultWidth)) {
                                    if (tempVecHasAtLeastWords(resultRef, state.tempVecWidths, *wordCount) &&
                                        tempVecHasAtLeastWords(getOperandExpr(0), state.tempVecWidths, *wordCount) &&
                                        tempVecHasAtLeastWords(getOperandExpr(1), state.tempVecWidths, *wordCount)) {
                                        return "wolvrix_gsim_bitwise_xor_" + std::to_string(*wordCount) + "_fast(" +
                                               resultRef + ", " + getOperandExpr(0) + ", " + getOperandExpr(1) +
                                               ", " + fixedWideLastMaskExpr(resultWidth) + ");";
                                    }
                                    return "wolvrix_gsim_bitwise_xor_" + std::to_string(*wordCount) + "(" +
                                           resultRef + ", " + getOperandExpr(0) + ", " + getOperandExpr(1) +
                                           ", " + fixedWideLastMaskExpr(resultWidth) + ");";
                                }
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
                    if (operands.size() >= 2U && operands[0] == operands[1]) {
                        setResultExpr(0, "1U");
                        break;
                    }
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
                    if (operands.size() >= 2U && operands[0] == operands[1]) {
                        setResultExpr(0, "1U");
                        break;
                    }
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
                    if (operands.size() >= 2U && operands[0] == operands[1]) {
                        setResultExpr(0, "0U");
                        break;
                    }
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
                    if (operands.size() >= 2U && operands[0] == operands[1]) {
                        setResultExpr(0, "0U");
                        break;
                    }
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
                    if (operands.size() >= 2U && operands[0] == operands[1]) {
                        setResultExpr(0, "1U");
                        break;
                    }
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
                    if (operands.size() >= 2U && operands[0] == operands[1]) {
                        setResultExpr(0, "0U");
                        break;
                    }
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
                    if (operands.size() >= 2U && operands[0] == operands[1]) {
                        setResultExpr(0, "1U");
                        break;
                    }
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
                    if (operands.size() >= 2U && operands[0] == operands[1]) {
                        setResultExpr(0, "0U");
                        break;
                    }
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
                                    if (const auto wordCount = fixedWideWordCountForWidth(resultValue.width())) {
                                        if (tempVecHasAtLeastWords(resultRef, state.tempVecWidths, *wordCount) &&
                                            tempVecHasAtLeastWords(getOperandExpr(0), state.tempVecWidths, *wordCount)) {
                                            return "wolvrix_gsim_bitwise_not_" + std::to_string(*wordCount) + "_fast(" +
                                                   resultRef + ", " + getOperandExpr(0) + ", " +
                                                   fixedWideLastMaskExpr(resultValue.width()) + ");";
                                        }
                                        return "wolvrix_gsim_bitwise_not_" + std::to_string(*wordCount) + "(" +
                                               resultRef + ", " + getOperandExpr(0) + ", " +
                                               fixedWideLastMaskExpr(resultValue.width()) + ");";
                                    }
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
                        state.emitShardStatement(
                            "wolvrix_gsim_clear_bits(" + resultRef + ", " +
                            std::to_string(static_cast<std::uint64_t>((resultValue.width() + 63) / 64)) + "U);");

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
                            recordResultMetadata(0);
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
                        recordResultMetadata(0);
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
                        recordResultMetadata(0);
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

                    if (operandWidth > 64 && *sliceWidth == 1) {
                        const char* helper = operandWidth <= 128
                            ? "wolvrix_gsim_slice_dynamic_bit_to_u64_u128"
                            : "wolvrix_gsim_slice_dynamic_bit_to_u64";
                        const std::string slicedExpr =
                            std::string(helper) + "(" + getOperandExpr(0) + ", " + indexExpr + ", " +
                            std::to_string(operandWidth) + ")";
                        setResultExpr(0, slicedExpr);
                        break;
                    }

                    if (operandWidth > 64) {
                        const char* helper = operandWidth <= 128
                            ? "wolvrix_gsim_slice_dynamic_to_u64_u128"
                            : "wolvrix_gsim_slice_dynamic_to_u64";
                        const std::string slicedExpr =
                            std::string(helper) + "(" + getOperandExpr(0) + ", " + indexExpr + ", " +
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

                    if (operandWidth > 64 && sliceWidth == 1)
                    {
                        const std::string slicedExpr =
                            "wolvrix_gsim_slice_dynamic_bit_to_u64(" + getOperandExpr(0) + ", " +
                            std::to_string(*sliceStart) + "ULL, " + std::to_string(operandWidth) + ")";
                        setResultExpr(0, slicedExpr);
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
                            if (operandWidth == 1) {
                                return "wolvrix_gsim_replicate_bit_into(" + resultRef + ", " + getOperandExpr(0) + ", " +
                                       std::to_string(totalWidth) + ");";
                            }
                            return "wolvrix_gsim_replicate_into(" + resultRef + ", " + getOperandExpr(0) + ", " +
                                   std::to_string(operandWidth) + ", " + std::to_string(*rep) + ");";
                        });
                        break;
                    }
                    if (operandWidth > 64 || totalWidth > 64) {
                        if (operandWidth == 1) {
                            setResultExpr(0,
                                          "wolvrix_gsim_replicate_bit(" + getOperandExpr(0) + ", " +
                                              std::to_string(totalWidth) + ")");
                            break;
                        }
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
                    const bool hasMaskOperand = operands.size() > 2;
                    const std::string mask = hasMaskOperand ? getOperandExpr(2) : std::string{};

                    const auto latchWidthIt = state.storageWidths.find(latchName);
                    const int64_t latchWidth =
                        latchWidthIt != state.storageWidths.end() ? latchWidthIt->second : 0;
                    const bool wideLatch = latchWidth > 64;
                    const bool fullMask =
                        !hasMaskOperand || (hasMaskOperand && isZeroScalarMaskExpr(mask)) ||
                        (!wideLatch && isAllOnesScalarMaskExpr(mask, latchWidth));

                    if (!fullMask) {
                        if (wideLatch) {
                            state.latchStmts.push_back(
                                "        if (" + condition + ") { wolvrix_gsim_mask_merge_in_place(" +
                                latchExpr + ", " + nextValue + ", " + mask + "); }");
                        } else {
                            state.latchStmts.push_back(
                                "        if (" + condition + ") { " + latchExpr + " = ((" + latchExpr +
                                ") & ~(" + mask + ")) | ((" + nextValue + ") & (" + mask + ")); }");
                        }
                    } else {
                        if (wideLatch) {
                            state.latchStmts.push_back(
                                "        if (" + condition + ") { wolvrix_gsim_assign_bits(" + latchExpr +
                                ", " + nextValue + "); }");
                        } else {
                            state.latchStmts.push_back(
                                "        if (" + condition + ") { " + latchExpr + " = " + nextValue + "; }");
                        }
                    }
                    break;
                }

                case OperationKind::kRegisterWritePort: {
                    // Operands: [updateCond, nextValue, mask, ...]
                    std::string condition = getOperandScalarExprPreferLiteral(0);
                    std::string nextValue = getOperandExpr(1);
                    const bool hasMaskOperand = operands.size() > 2;
                    std::string mask = hasMaskOperand ? getOperandScalarExprPreferLiteral(2) : std::string{};

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
                    std::string clockSymbol = "clock";
                    if (operands.size() > 3) {
                        const auto eventValue = graph.getValue(operands[3]);
                        if (!eventValue.symbolText().empty()) {
                            clockSymbol = std::string(eventValue.symbolText());
                        }
                    }
                    if (auto clockSymAttr = op.attr("clockSymbol")) {
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
                        if (state.storageWidths.find(regName) == state.storageWidths.end()) {
                            const int32_t inferredWidth = operands.size() > 1 ? getOperandWidth(1) : 32;
                            const int32_t storageWidth = inferredWidth > 0 ? inferredWidth : 32;
                            const std::string storageType = getCppTypeForWidth(storageWidth);
                            state.allocatePersistentStorage(regName, storageType, storageWidth);
                            state.storageWidths[regName] = storageWidth;
                        }
                        auto &regStmts = state.sequentialRegStmts[domainKey][regName];
                        const std::string nextRegExpr = "next_" + regName;
                        const auto regWidthIt = state.storageWidths.find(regName);
                        const int64_t regWidth = regWidthIt != state.storageWidths.end() ? regWidthIt->second : 0;
                        const bool wideReg = regWidth > 64;
                        const bool zeroMask = hasMaskOperand && !wideReg && isZeroScalarMaskExpr(mask);
                        const bool fullMask =
                            !hasMaskOperand || (!wideReg && isAllOnesScalarMaskExpr(mask, regWidth));
                        if (zeroMask) {
                            break;
                        }
                        if (!fullMask) {
                            if (wideReg) {
                                const std::string mergedExpr = nextRegExpr + "_merged_";
                                regStmts.push_back(
                                    "        if (" + condition + ") { auto " + mergedExpr +
                                    " = wolvrix_gsim_mask_merge(" + nextRegExpr + ", " + nextValue + ", " + mask +
                                    "); if (" + mergedExpr + " != " + nextRegExpr + ") { " + nextRegExpr +
                                    " = std::move(" + mergedExpr + "); " + nextRegExpr +
                                    "_updated_ = true; committed_ = true; } }");
                            } else {
                                const std::string mergedExpr = nextRegExpr + "_merged_";
                                regStmts.push_back(
                                    "        if (" + condition + ") { const auto " + mergedExpr + " = ((" + nextRegExpr +
                                    ") & ~(" + mask + ")) | ((" + nextValue + ") & (" + mask +
                                    ")); if (" + mergedExpr + " != " + nextRegExpr + ") { " + nextRegExpr +
                                    " = " + mergedExpr + "; " + nextRegExpr + "_updated_ = true; committed_ = true; } }");
                            }
                        } else {
                            const std::string cleanNextValue =
                                wideReg ? nextValue : maskExprForWidth(nextValue, static_cast<int32_t>(regWidth));
                            regStmts.push_back(
                                "        if (" + condition + ") { " + nextRegExpr + " = " + cleanNextValue +
                                "; " + nextRegExpr + "_updated_ = true; committed_ = true; }");
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
                    if (state.enableDpicTrace && state.shouldTraceDpicTarget(*target)) {
                        const std::size_t callSite = state.dpicTraceCallSite++;
                        const std::string condName = "dpic_cond_" + std::to_string(callSite) + "_";
                        const std::string seenName = "dpic_seen_" + std::to_string(callSite) + "_";
                        const std::string hitsName = "dpic_hits_" + std::to_string(callSite) + "_";
                        const std::string missesName = "dpic_misses_" + std::to_string(callSite) + "_";
                        const bool traceFalseSamples = true;
                        auto appendXsZeroRetireTrace = [&]() {
                            if (!state.enableXsZeroRetireTrace) {
                                return;
                            }
                            if (*target != "v_difftest_InstrCommit") {
                                return;
                            }
                            auto appendTempU8 = [&](std::string_view prefix, std::string_view label, std::size_t index) {
                                const bool present = index < state.tempU8Count;
                                const std::string fullLabel = std::string(prefix) + "_" + std::string(label);
                                stmt += " << \" " + fullLabel +
                                        "=\" << static_cast<std::uint64_t>(";
                                stmt += present ? "evalTemps_->tempU8[" + std::to_string(index) + "]"
                                                : "UINT64_C(0)";
                                stmt += ")";
                                stmt += " << \" " + fullLabel + "_present="
                                      + (present ? "1" : "0") + "\"";
                            };
                            auto appendStateU8 = [&](std::string_view prefix, std::string_view label, std::size_t index) {
                                const bool present = index < state.stateU8Count;
                                const std::string fullLabel = std::string(prefix) + "_" + std::string(label);
                                stmt += " << \" " + fullLabel +
                                        "=\" << static_cast<std::uint64_t>(";
                                stmt += present ? "state_->stateU8[" + std::to_string(index) + "]"
                                                : "UINT64_C(0)";
                                stmt += ")";
                                stmt += " << \" " + fullLabel + "_present="
                                      + (present ? "1" : "0") + "\"";
                            };
                            appendTempU8("r36", "t1903395", 1903395);
                            appendTempU8("r36", "t2516282", 2516282);
                            appendStateU8("r36", "s78873", 78873);
                            appendStateU8("r36", "hc0", 92257);
                            appendStateU8("r36", "hc1", 92258);
                            appendStateU8("r36", "hc2", 92259);
                            appendStateU8("r36", "hc3", 92260);
                            appendStateU8("r36", "hc4", 92261);
                            appendStateU8("r36", "deq0_v", 92274);
                            appendStateU8("r36", "deq0_w", 92275);
                            appendStateU8("r36", "deq1_v", 92304);
                            appendStateU8("r36", "deq1_w", 92305);
                            appendStateU8("r36", "redirectValid", 93751);
                            appendStateU8("r36", "redirectAll", 93752);
                            appendStateU8("r36", "flushLast", 77454);
                            appendStateU8("r36", "endpointValid", 236509);

                            appendTempU8("r38", "t2648875", 2648875);
                            appendTempU8("r38", "t2206312", 2206312);
                            appendTempU8("r38", "t2581973", 2581973);
                            appendTempU8("r38", "t1903395", 1903395);
                            appendTempU8("r38", "t2516282", 2516282);
                            appendTempU8("r38", "t2020255", 2020255);
                            appendTempU8("r38", "t2020254", 2020254);
                            appendStateU8("r38", "endpointPipe0", 236477);
                            appendStateU8("r38", "endpointPipe1", 236493);
                            appendStateU8("r38", "endpointPipe2", 236509);
                            appendStateU8("r38", "crossFtqCommit", 79792);
                            appendStateU8("r38", "deq0_v", 92274);
                            appendStateU8("r38", "deq0_w", 92275);
                            appendStateU8("r38", "deq1_v", 92304);
                            appendStateU8("r38", "deq1_w", 92305);
                            appendStateU8("r38", "redirectValid", 93751);
                            appendStateU8("r38", "redirectAll", 93752);
                            appendStateU8("r38", "flushLast", 77454);
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
                        appendXsZeroRetireTrace();
                        stmt += " << \"\\n\"; " + seenName + " = true; } ";
                        if (traceFalseSamples) {
                            stmt += "if (!" + condName + ") { ++" + missesName + "; if (" + missesName + " <= 16U || (" + missesName + " % 1024U) == 0U) { std::cerr << \"[wolvrix-gsim-dpic] site=" + std::to_string(callSite) + " target=" + *target + " miss=\" << " + missesName;
                            for (std::size_t i = 0; i < args.size(); ++i) {
                                stmt += " << \" arg" + std::to_string(i) + "=\" << static_cast<std::uint64_t>(" + args[i] + ")";
                            }
                            appendXsZeroRetireTrace();
                            stmt += " << \"\\n\"; } } ";
                        }
                        stmt += "if (" + condName + ") { ++" + hitsName + "; if (" + hitsName + " <= 16U) { std::cerr << \"[wolvrix-gsim-dpic] site=" + std::to_string(callSite) + " target=" + *target + " hit=\" << " + hitsName;
                        for (std::size_t i = 0; i < args.size(); ++i) {
                            stmt += " << \" arg" + std::to_string(i) + "=\" << static_cast<std::uint64_t>(" + args[i] + ")";
                        }
                        appendXsZeroRetireTrace();
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
                    if (state.enableDpicTrace && state.shouldTraceDpicTarget(*target)) {
                        stmt += " }";
                    }
                    recordDpicCall(state, *target);
                    if (isGlobalPreEdgeDpicTarget(*target)) {
                        // Difftest monitor modules are observational always-@edge
                        // side effects.  Run them in a global pre-register edge
                        // phase so all no-diff monitors sample one consistent
                        // pre-edge snapshot before any clock domain writes state.
                        state.sequentialGlobalPreStmts[domainKey].push_back(
                            guardNoDiffDpicStatement(std::move(stmt), *target));
                    } else {
                        state.sequentialStmts[domainKey].push_back(
                            guardNoDiffDpicStatement(std::move(stmt), *target));
                        state.sequentialStmtDirtyOnCommit[domainKey].push_back(false);
                        state.sequentialStmtActivitySources[domainKey].push_back({});
                    }
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
            std::optional<GsimScheduleBatchMetadata> scheduleBatch;
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

            const std::vector<std::string_view> batchSuffixes = {
                ".schedule.batch.kind",
                ".schedule.batch.version",
                ".schedule.batch.contract",
                ".schedule.batch.count",
                ".schedule.batch.names",
                ".schedule.batch.class_ids",
                ".schedule.batch.class_names",
                ".schedule.batch.flags",
                ".schedule.batch.topo_batch_by_pos",
                ".schedule.batch.first_topo_pos",
                ".schedule.batch.last_topo_pos",
                ".schedule.batch.op_counts",
                ".schedule.batch.succ_offsets",
                ".schedule.batch.succ_targets",
                ".schedule.batch.entry_batches",
                ".schedule.batch.estimated_lines",
            };
            bool hasBatchMetadata = false;
            for (const auto suffix : batchSuffixes)
            {
                if (design.hasScratchpad(scratchPrefix + std::string(suffix)))
                {
                    hasBatchMetadata = true;
                    break;
                }
            }
            if (hasBatchMetadata)
            {
                const auto *batchKind = design.getScratchpad<std::string>(scratchPrefix + ".schedule.batch.kind");
                const auto *batchVersion = design.getScratchpad<int64_t>(scratchPrefix + ".schedule.batch.version");
                const auto *batchContract = design.getScratchpad<std::string>(scratchPrefix + ".schedule.batch.contract");
                const auto *batchCount = design.getScratchpad<int64_t>(scratchPrefix + ".schedule.batch.count");
                const auto *batchNames = design.getScratchpad<std::vector<std::string>>(scratchPrefix + ".schedule.batch.names");
                const auto *batchClassIds = design.getScratchpad<std::vector<int64_t>>(scratchPrefix + ".schedule.batch.class_ids");
                const auto *batchClassNames = design.getScratchpad<std::vector<std::string>>(scratchPrefix + ".schedule.batch.class_names");
                const auto *batchFlags = design.getScratchpad<std::vector<int64_t>>(scratchPrefix + ".schedule.batch.flags");
                const auto *batchTopoByPos = design.getScratchpad<std::vector<int64_t>>(scratchPrefix + ".schedule.batch.topo_batch_by_pos");
                const auto *batchFirstTopoPos = design.getScratchpad<std::vector<int64_t>>(scratchPrefix + ".schedule.batch.first_topo_pos");
                const auto *batchLastTopoPos = design.getScratchpad<std::vector<int64_t>>(scratchPrefix + ".schedule.batch.last_topo_pos");
                const auto *batchOpCounts = design.getScratchpad<std::vector<int64_t>>(scratchPrefix + ".schedule.batch.op_counts");
                const auto *batchSuccOffsets = design.getScratchpad<std::vector<int64_t>>(scratchPrefix + ".schedule.batch.succ_offsets");
                const auto *batchSuccTargets = design.getScratchpad<std::vector<int64_t>>(scratchPrefix + ".schedule.batch.succ_targets");
                const auto *batchEntryBatches = design.getScratchpad<std::vector<int64_t>>(scratchPrefix + ".schedule.batch.entry_batches");
                const auto *batchEstimatedLines = design.getScratchpad<std::vector<int64_t>>(scratchPrefix + ".schedule.batch.estimated_lines");

                bool batchOk = true;
                batchOk = require(batchKind, ".schedule.batch.kind", "string") && batchOk;
                batchOk = require(batchVersion, ".schedule.batch.version", "int64_t") && batchOk;
                batchOk = require(batchContract, ".schedule.batch.contract", "string") && batchOk;
                batchOk = require(batchCount, ".schedule.batch.count", "int64_t") && batchOk;
                batchOk = require(batchNames, ".schedule.batch.names", "vector<string>") && batchOk;
                batchOk = require(batchClassIds, ".schedule.batch.class_ids", "vector<int64_t>") && batchOk;
                batchOk = require(batchClassNames, ".schedule.batch.class_names", "vector<string>") && batchOk;
                batchOk = require(batchFlags, ".schedule.batch.flags", "vector<int64_t>") && batchOk;
                batchOk = require(batchTopoByPos, ".schedule.batch.topo_batch_by_pos", "vector<int64_t>") && batchOk;
                batchOk = require(batchFirstTopoPos, ".schedule.batch.first_topo_pos", "vector<int64_t>") && batchOk;
                batchOk = require(batchLastTopoPos, ".schedule.batch.last_topo_pos", "vector<int64_t>") && batchOk;
                batchOk = require(batchOpCounts, ".schedule.batch.op_counts", "vector<int64_t>") && batchOk;
                batchOk = require(batchSuccOffsets, ".schedule.batch.succ_offsets", "vector<int64_t>") && batchOk;
                batchOk = require(batchSuccTargets, ".schedule.batch.succ_targets", "vector<int64_t>") && batchOk;
                batchOk = require(batchEntryBatches, ".schedule.batch.entry_batches", "vector<int64_t>") && batchOk;
                batchOk = require(batchEstimatedLines, ".schedule.batch.estimated_lines", "vector<int64_t>") && batchOk;
                if (!batchOk)
                {
                    return std::nullopt;
                }

                GsimScheduleBatchMetadata batch{*batchKind,
                                                 *batchVersion,
                                                 *batchContract,
                                                 *batchCount,
                                                 *batchNames,
                                                 *batchClassIds,
                                                 *batchClassNames,
                                                 *batchFlags,
                                                 *batchTopoByPos,
                                                 *batchFirstTopoPos,
                                                 *batchLastTopoPos,
                                                 *batchOpCounts,
                                                 *batchSuccOffsets,
                                                 *batchSuccTargets,
                                                 *batchEntryBatches,
                                                 *batchEstimatedLines};
                if (batch.kind != "activity-batch-v1" || batch.contract != "gsim.activity.schedule_batch.v1" || batch.version <= 0)
                {
                    reportError("gsim scratchpad metadata is malformed",
                                scratchPrefix + " schedule batch metadata contract mismatch");
                    return std::nullopt;
                }
                if (batch.count < 0)
                {
                    reportError("gsim scratchpad metadata is malformed",
                                scratchPrefix + ".schedule.batch.count must be non-negative");
                    return std::nullopt;
                }
                const auto batchSize = static_cast<std::size_t>(batch.count);
                auto requireBatchSize = [&](const auto &items, std::string_view suffix) -> bool
                {
                    if (items.size() == batchSize)
                    {
                        return true;
                    }
                    reportError("gsim scratchpad metadata is malformed",
                                scratchPrefix + std::string(suffix) + " size must match schedule.batch.count");
                    return false;
                };
                if (!requireBatchSize(batch.names, ".schedule.batch.names") ||
                    !requireBatchSize(batch.classIds, ".schedule.batch.class_ids") ||
                    !requireBatchSize(batch.flags, ".schedule.batch.flags") ||
                    !requireBatchSize(batch.firstTopoPos, ".schedule.batch.first_topo_pos") ||
                    !requireBatchSize(batch.lastTopoPos, ".schedule.batch.last_topo_pos") ||
                    !requireBatchSize(batch.opCounts, ".schedule.batch.op_counts") ||
                    !requireBatchSize(batch.estimatedLines, ".schedule.batch.estimated_lines"))
                {
                    return std::nullopt;
                }
                if (batch.classNames.empty())
                {
                    reportError("gsim scratchpad metadata is malformed",
                                scratchPrefix + ".schedule.batch.class_names must not be empty");
                    return std::nullopt;
                }
                if (batch.topoBatchByPos.size() != metadata.topoOrder.size() ||
                    batch.topoBatchByPos.size() != static_cast<std::size_t>(metadata.opCount))
                {
                    reportError("gsim scratchpad metadata is malformed",
                                scratchPrefix + ".schedule.batch.topo_batch_by_pos must cover topology.order");
                    return std::nullopt;
                }
                if (batch.succOffsets.size() != batchSize + 1U || batch.succOffsets.empty() || batch.succOffsets.front() != 0)
                {
                    reportError("gsim scratchpad metadata is malformed",
                                scratchPrefix + ".schedule.batch.succ_offsets must have count+1 entries starting at zero");
                    return std::nullopt;
                }
                std::vector<int64_t> observedCounts(batchSize, 0);
                for (std::size_t pos = 0; pos < batch.topoBatchByPos.size(); ++pos)
                {
                    const auto batchOrdinal = batch.topoBatchByPos[pos];
                    if (batchOrdinal < 0 || static_cast<std::size_t>(batchOrdinal) >= batchSize)
                    {
                        reportError("gsim scratchpad metadata is malformed",
                                    scratchPrefix + ".schedule.batch.topo_batch_by_pos references invalid batch ordinal");
                        return std::nullopt;
                    }
                    ++observedCounts[static_cast<std::size_t>(batchOrdinal)];
                    const auto first = batch.firstTopoPos[static_cast<std::size_t>(batchOrdinal)];
                    const auto last = batch.lastTopoPos[static_cast<std::size_t>(batchOrdinal)];
                    if (first < 0 || last < first || static_cast<std::size_t>(first) > pos || pos > static_cast<std::size_t>(last))
                    {
                        reportError("gsim scratchpad metadata is malformed",
                                    scratchPrefix + ".schedule.batch first/last topo positions do not cover topo_batch_by_pos");
                        return std::nullopt;
                    }
                }
                for (std::size_t batchOrdinal = 0; batchOrdinal < batchSize; ++batchOrdinal)
                {
                    if (batch.opCounts[batchOrdinal] != observedCounts[batchOrdinal])
                    {
                        reportError("gsim scratchpad metadata is malformed",
                                    scratchPrefix + ".schedule.batch.op_counts must match topo_batch_by_pos");
                        return std::nullopt;
                    }
                    if (batch.classIds[batchOrdinal] < 0 ||
                        static_cast<std::size_t>(batch.classIds[batchOrdinal]) >= batch.classNames.size())
                    {
                        reportError("gsim scratchpad metadata is malformed",
                                    scratchPrefix + ".schedule.batch.class_ids references invalid class name");
                        return std::nullopt;
                    }
                    if (batch.estimatedLines[batchOrdinal] < 0)
                    {
                        reportError("gsim scratchpad metadata is malformed",
                                    scratchPrefix + ".schedule.batch.estimated_lines must be non-negative");
                        return std::nullopt;
                    }
                }
                for (std::size_t i = 1; i < batch.succOffsets.size(); ++i)
                {
                    if (batch.succOffsets[i] < batch.succOffsets[i - 1])
                    {
                        reportError("gsim scratchpad metadata is malformed",
                                    scratchPrefix + ".schedule.batch.succ_offsets must be monotonic");
                        return std::nullopt;
                    }
                }
                if (batch.succOffsets.back() != static_cast<int64_t>(batch.succTargets.size()))
                {
                    reportError("gsim scratchpad metadata is malformed",
                                scratchPrefix + ".schedule.batch.succ_offsets must cover succ_targets");
                    return std::nullopt;
                }
                for (std::size_t batchOrdinal = 0; batchOrdinal < batchSize; ++batchOrdinal)
                {
                    std::set<int64_t> seenTargets;
                    const auto begin = batch.succOffsets[batchOrdinal];
                    const auto end = batch.succOffsets[batchOrdinal + 1U];
                    for (int64_t index = begin; index < end; ++index)
                    {
                        const auto target = batch.succTargets[static_cast<std::size_t>(index)];
                        if (target < 0 || static_cast<std::size_t>(target) >= batchSize)
                        {
                            reportError("gsim scratchpad metadata is malformed",
                                        scratchPrefix + ".schedule.batch.succ_targets references invalid batch ordinal");
                            return std::nullopt;
                        }
                        if (target == static_cast<int64_t>(batchOrdinal))
                        {
                            reportError("gsim scratchpad metadata is malformed",
                                        scratchPrefix + ".schedule.batch.succ_targets must not contain self edges");
                            return std::nullopt;
                        }
                        if (!seenTargets.insert(target).second)
                        {
                            reportError("gsim scratchpad metadata is malformed",
                                        scratchPrefix + ".schedule.batch.succ_targets must not contain duplicate targets");
                            return std::nullopt;
                        }
                    }
                }
                if (!std::is_sorted(batch.entryBatches.begin(), batch.entryBatches.end()) ||
                    std::adjacent_find(batch.entryBatches.begin(), batch.entryBatches.end()) != batch.entryBatches.end())
                {
                    reportError("gsim scratchpad metadata is malformed",
                                scratchPrefix + ".schedule.batch.entry_batches must be sorted and unique");
                    return std::nullopt;
                }
                for (const auto entry : batch.entryBatches)
                {
                    if (entry < 0 || static_cast<std::size_t>(entry) >= batchSize)
                    {
                        reportError("gsim scratchpad metadata is malformed",
                                    scratchPrefix + ".schedule.batch.entry_batches references invalid batch ordinal");
                        return std::nullopt;
                    }
                }
                metadata.scheduleBatch = std::move(batch);
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

        std::string parseActivityBatchMode(const EmitOptions &options)
        {
            const auto value = attrValue(options, "activity_batch_mode");
            if (!value)
            {
                return "legacy";
            }
            std::string lowered = *value;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            if (lowered == "legacy" || lowered == "stats" || lowered == "dispatch" || lowered == "strict_dispatch")
            {
                return lowered;
            }
            return "invalid";
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

        int parseNonNegativeIntAttr(const EmitOptions &options, std::string_view key, int defaultValue)
        {
            const auto value = attrValue(options, key);
            if (!value)
            {
                return defaultValue;
            }
            try
            {
                const int parsed = std::stoi(*value);
                return parsed >= 0 ? parsed : defaultValue;
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
            for (const auto& [domainKey, _] : state.sequentialGlobalPreStmts) {
                domains.insert(domainKey);
            }
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
                if (auto globalPreIt = state.sequentialGlobalPreStmts.find(domainKey);
                    globalPreIt != state.sequentialGlobalPreStmts.end()) {
                    SequentialChunkPlan current;
                    current.domainKey = domainKey;
                    current.preReg = true;
                    current.globalPreReg = true;
                    std::size_t currentBytes = 0;
                    auto flushCurrent = [&]() {
                        if (current.stmts.empty()) {
                            return;
                        }
                        current.methodName = "commit_chunk_global_pre_" + sanitizeIdentifier(domainKey) + "_" +
                                             std::to_string(chunkIndex++);
                        plans.push_back(current);
                        current = SequentialChunkPlan{};
                        current.domainKey = domainKey;
                        current.preReg = true;
                        current.globalPreReg = true;
                        currentBytes = 0;
                    };
                    for (const auto& stmt : globalPreIt->second) {
                        const std::size_t estimatedBytes = stmt.size() + 1;
                        if (!current.stmts.empty() && currentBytes + estimatedBytes > maxChunkBytes) {
                            flushCurrent();
                        }
                        current.stmts.push_back(stmt);
                        currentBytes += estimatedBytes;
                    }
                    flushCurrent();
                }
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
            os << "#include <cstring>\n";
            os << "#include <initializer_list>\n";
            os << "#include <map>\n";
            os << "#include <memory>\n";
            os << "#include <stdexcept>\n";
            os << "#include <string>\n";
            os << "#include <type_traits>\n";
            os << "#include <utility>\n";
            os << "#include <vector>\n\n";
            os << "#if defined(__GNUC__) || defined(__clang__)\n";
            os << "#define WOLVRIX_GSIM_ALWAYS_INLINE inline __attribute__((always_inline))\n";
            os << "#else\n";
            os << "#define WOLVRIX_GSIM_ALWAYS_INLINE inline\n";
            os << "#endif\n\n";
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
            os << "inline void wolvrix_gsim_clear_bits(std::vector<std::uint64_t>& out, std::size_t wordCount) {\n";
            os << "    if (out.size() != wordCount) {\n";
            os << "        out.assign(wordCount, 0ULL);\n";
            os << "        return;\n";
            os << "    }\n";
            os << "    std::fill(out.begin(), out.end(), 0ULL);\n";
            os << "}\n";
            os << "inline void wolvrix_gsim_assign_bits(\n";
            os << "    std::vector<std::uint64_t>& out,\n";
            os << "    const std::vector<std::uint64_t>& value) {\n";
            os << "    if (&out == &value) {\n";
            os << "        return;\n";
            os << "    }\n";
            os << "    const auto wordCount = value.size();\n";
            os << "    const auto outWordCount = out.size();\n";
            os << "    if (outWordCount < wordCount) {\n";
            os << "        out.resize(wordCount);\n";
            os << "    }\n";
            os << "    if (wordCount != 0U) {\n";
            os << "        std::copy(value.data(), value.data() + wordCount, out.data());\n";
            os << "    }\n";
            os << "    if (outWordCount > wordCount) {\n";
            os << "        std::fill(out.begin() + static_cast<std::ptrdiff_t>(wordCount), out.end(), 0ULL);\n";
            os << "    }\n";
            os << "}\n";
            os << "inline void wolvrix_gsim_assign_bits(\n";
            os << "    std::vector<std::uint64_t>& out,\n";
            os << "    std::initializer_list<std::uint64_t> words) {\n";
            os << "    const auto wordCount = words.size();\n";
            os << "    if (out.size() != wordCount) {\n";
            os << "        out.resize(wordCount);\n";
            os << "    }\n";
            os << "    if (wordCount != 0U) {\n";
            os << "        std::copy(words.begin(), words.end(), out.data());\n";
            os << "    }\n";
            os << "}\n";
            for (std::size_t wordCount = 1; wordCount <= 4; ++wordCount) {
                os << "inline void wolvrix_gsim_assign_bits_" << wordCount << "(\n";
                os << "    std::vector<std::uint64_t>& out,\n";
                os << "    const std::vector<std::uint64_t>& value) {\n";
                os << "    if (&out == &value) { return; }\n";
                os << "    if (out.size() != " << wordCount << "U) { out.assign(" << wordCount << "U, 0ULL); }\n";
                for (std::size_t word = 0; word < wordCount; ++word) {
                    os << "    out[" << word << "U] = value.size() > " << word << "U ? value[" << word << "U] : 0ULL;\n";
                }
                os << "}\n";
            }
            os << "template <typename L, typename R>\n";
            os << "WOLVRIX_GSIM_ALWAYS_INLINE bool wolvrix_gsim_assign_if_changed(L& lhs, R&& rhs) {\n";
            os << "    const L next = static_cast<L>(std::forward<R>(rhs));\n";
            os << "    if (lhs != next) {\n";
            os << "        lhs = next;\n";
            os << "        return true;\n";
            os << "    }\n";
            os << "    return false;\n";
            os << "}\n";
            os << "template <typename T>\n";
            os << "inline bool wolvrix_gsim_assign_scalar_span_if_changed(\n";
            os << "    T* lhs,\n";
            os << "    const T* rhs,\n";
            os << "    std::size_t count) {\n";
            os << "    const std::size_t byteCount = count * sizeof(T);\n";
            os << "    if (count == 0U || std::memcmp(lhs, rhs, byteCount) == 0) {\n";
            os << "        return false;\n";
            os << "    }\n";
            os << "    std::memcpy(lhs, rhs, byteCount);\n";
            os << "    return true;\n";
            os << "}\n";
            os << "template <typename T>\n";
            os << "inline bool wolvrix_gsim_assign_mux_span_if_changed(\n";
            os << "    T* lhs,\n";
            os << "    bool condition,\n";
            os << "    const T* trueValue,\n";
            os << "    const T* falseValue,\n";
            os << "    std::size_t count) {\n";
            os << "    return wolvrix_gsim_assign_scalar_span_if_changed(lhs, condition ? trueValue : falseValue, count);\n";
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
            os << "inline std::uint64_t wolvrix_gsim_slice_dynamic_bit_to_u64(\n";
            os << "    const std::vector<std::uint64_t>& value,\n";
            os << "    std::uint64_t bitIndex,\n";
            os << "    std::uint32_t operandWidth) {\n";
            os << "    return bitIndex < operandWidth ? (wolvrix_gsim_load_shifted_word(value, bitIndex) & 1ULL) : 0ULL;\n";
            os << "}\n";
            os << "inline std::uint64_t wolvrix_gsim_slice_dynamic_bit_to_u64_u128(\n";
            os << "    const std::vector<std::uint64_t>& value,\n";
            os << "    std::uint64_t bitIndex,\n";
            os << "    std::uint32_t operandWidth) {\n";
            os << "    if (bitIndex >= operandWidth) {\n";
            os << "        return 0ULL;\n";
            os << "    }\n";
            os << "    const auto wordIndex = static_cast<std::size_t>(bitIndex >> 6U);\n";
            os << "    const auto bitOffset = static_cast<std::uint32_t>(bitIndex & 63ULL);\n";
            os << "    const std::uint64_t word = wordIndex == 0U\n";
            os << "        ? (value.empty() ? 0ULL : value[0U])\n";
            os << "        : (value.size() > 1U ? value[1U] : 0ULL);\n";
            os << "    return (word >> bitOffset) & 1ULL;\n";
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
            os << "inline std::uint64_t wolvrix_gsim_slice_dynamic_to_u64_u128(\n";
            os << "    const std::vector<std::uint64_t>& value,\n";
            os << "    std::uint64_t bitIndex,\n";
            os << "    std::uint32_t sliceWidth,\n";
            os << "    std::uint32_t operandWidth) {\n";
            os << "    if (sliceWidth == 0 || sliceWidth > 64 || bitIndex >= operandWidth) {\n";
            os << "        return 0ULL;\n";
            os << "    }\n";
            os << "    const auto availableBits = static_cast<std::uint32_t>(std::min<std::uint64_t>(sliceWidth, operandWidth - bitIndex));\n";
            os << "    const auto wordIndex = static_cast<std::size_t>(bitIndex >> 6U);\n";
            os << "    const auto bitOffset = static_cast<std::uint32_t>(bitIndex & 63ULL);\n";
            os << "    const std::uint64_t low = wordIndex == 0U\n";
            os << "        ? (value.empty() ? 0ULL : value[0U])\n";
            os << "        : (value.size() > 1U ? value[1U] : 0ULL);\n";
            os << "    std::uint64_t result = low >> bitOffset;\n";
            os << "    if (bitOffset != 0U && wordIndex == 0U) {\n";
            os << "        const std::uint64_t high = value.size() > 1U ? value[1U] : 0ULL;\n";
            os << "        result |= high << (64U - bitOffset);\n";
            os << "    }\n";
            os << "    return result & wolvrix_gsim_low_mask(availableBits);\n";
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
            os << "template <std::size_t WordCount, typename L, typename R, typename Op>\n";
            os << "inline void wolvrix_gsim_bitwise_fixed(\n";
            os << "    std::vector<std::uint64_t>& out,\n";
            os << "    const L& lhs,\n";
            os << "    const R& rhs,\n";
            os << "    std::uint64_t lastMask,\n";
            os << "    Op op) {\n";
            os << "    static_assert(WordCount >= 1U && WordCount <= 4U);\n";
            os << "    if (out.size() != WordCount) { out.assign(WordCount, 0ULL); }\n";
            os << "    for (std::size_t i = 0; i + 1U < WordCount; ++i) {\n";
            os << "        out[i] = op(wolvrix_gsim_word_at(lhs, i), wolvrix_gsim_word_at(rhs, i));\n";
            os << "    }\n";
            os << "    constexpr std::size_t last = WordCount - 1U;\n";
            os << "    out[last] = op(wolvrix_gsim_word_at(lhs, last), wolvrix_gsim_word_at(rhs, last)) & lastMask;\n";
            os << "}\n";
            os << "template <std::size_t WordCount, typename T, typename Op>\n";
            os << "inline void wolvrix_gsim_bitwise_fixed_unary(\n";
            os << "    std::vector<std::uint64_t>& out,\n";
            os << "    const T& value,\n";
            os << "    std::uint64_t lastMask,\n";
            os << "    Op op) {\n";
            os << "    static_assert(WordCount >= 1U && WordCount <= 4U);\n";
            os << "    if (out.size() != WordCount) { out.assign(WordCount, 0ULL); }\n";
            os << "    for (std::size_t i = 0; i + 1U < WordCount; ++i) {\n";
            os << "        out[i] = op(wolvrix_gsim_word_at(value, i));\n";
            os << "    }\n";
            os << "    constexpr std::size_t last = WordCount - 1U;\n";
            os << "    out[last] = op(wolvrix_gsim_word_at(value, last)) & lastMask;\n";
            os << "}\n";
            for (std::size_t wordCount = 1; wordCount <= 4; ++wordCount) {
                os << "template <typename L, typename R>\n";
                os << "inline void wolvrix_gsim_bitwise_and_" << wordCount
                   << "(std::vector<std::uint64_t>& out, const L& lhs, const R& rhs, std::uint64_t lastMask) {\n";
                os << "    wolvrix_gsim_bitwise_fixed<" << wordCount
                   << ">(out, lhs, rhs, lastMask, [](std::uint64_t a, std::uint64_t b) { return a & b; });\n";
                os << "}\n";
                os << "inline void wolvrix_gsim_bitwise_and_" << wordCount
                   << "_fast(std::vector<std::uint64_t>& out, const std::vector<std::uint64_t>& lhs, const std::vector<std::uint64_t>& rhs, std::uint64_t lastMask) {\n";
                for (std::size_t i = 0; i < wordCount; ++i) {
                    os << "    out[" << i << "] = (lhs[" << i << "] & rhs[" << i << "])";
                    if (i + 1 == wordCount) {
                        os << " & lastMask";
                    }
                    os << ";\n";
                }
                os << "}\n";
                os << "template <typename L, typename R>\n";
                os << "inline void wolvrix_gsim_bitwise_or_" << wordCount
                   << "(std::vector<std::uint64_t>& out, const L& lhs, const R& rhs, std::uint64_t lastMask) {\n";
                os << "    wolvrix_gsim_bitwise_fixed<" << wordCount
                   << ">(out, lhs, rhs, lastMask, [](std::uint64_t a, std::uint64_t b) { return a | b; });\n";
                os << "}\n";
                os << "inline void wolvrix_gsim_bitwise_or_" << wordCount
                   << "_fast(std::vector<std::uint64_t>& out, const std::vector<std::uint64_t>& lhs, const std::vector<std::uint64_t>& rhs, std::uint64_t lastMask) {\n";
                for (std::size_t i = 0; i < wordCount; ++i) {
                    os << "    out[" << i << "] = (lhs[" << i << "] | rhs[" << i << "])";
                    if (i + 1 == wordCount) {
                        os << " & lastMask";
                    }
                    os << ";\n";
                }
                os << "}\n";
                os << "template <typename L, typename R>\n";
                os << "inline void wolvrix_gsim_bitwise_xor_" << wordCount
                   << "(std::vector<std::uint64_t>& out, const L& lhs, const R& rhs, std::uint64_t lastMask) {\n";
                os << "    wolvrix_gsim_bitwise_fixed<" << wordCount
                   << ">(out, lhs, rhs, lastMask, [](std::uint64_t a, std::uint64_t b) { return a ^ b; });\n";
                os << "}\n";
                os << "inline void wolvrix_gsim_bitwise_xor_" << wordCount
                   << "_fast(std::vector<std::uint64_t>& out, const std::vector<std::uint64_t>& lhs, const std::vector<std::uint64_t>& rhs, std::uint64_t lastMask) {\n";
                for (std::size_t i = 0; i < wordCount; ++i) {
                    os << "    out[" << i << "] = (lhs[" << i << "] ^ rhs[" << i << "])";
                    if (i + 1 == wordCount) {
                        os << " & lastMask";
                    }
                    os << ";\n";
                }
                os << "}\n";
                os << "template <typename T>\n";
                os << "inline void wolvrix_gsim_bitwise_not_" << wordCount
                   << "(std::vector<std::uint64_t>& out, const T& value, std::uint64_t lastMask) {\n";
                os << "    wolvrix_gsim_bitwise_fixed_unary<" << wordCount
                   << ">(out, value, lastMask, [](std::uint64_t a) { return ~a; });\n";
                os << "}\n";
                os << "inline void wolvrix_gsim_bitwise_not_" << wordCount
                   << "_fast(std::vector<std::uint64_t>& out, const std::vector<std::uint64_t>& value, std::uint64_t lastMask) {\n";
                for (std::size_t i = 0; i < wordCount; ++i) {
                    os << "    out[" << i << "] = ~value[" << i << "]";
                    if (i + 1 == wordCount) {
                        os << " & lastMask";
                    }
                    os << ";\n";
                }
                os << "}\n";
            }
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
            os << "inline void wolvrix_gsim_replicate_bit_into(\n";
            os << "    std::vector<std::uint64_t>& result,\n";
            os << "    const T& value,\n";
            os << "    std::uint32_t width) {\n";
            os << "    const auto requiredWords = static_cast<std::size_t>((width + 63U) / 64U);\n";
            os << "    if (result.size() != requiredWords) { result.resize(requiredWords); }\n";
            os << "    if (requiredWords == 0U) { return; }\n";
            os << "    const std::uint64_t fillWord = (wolvrix_gsim_to_u64(value) & 1ULL) != 0ULL ? ~UINT64_C(0) : UINT64_C(0);\n";
            os << "    std::fill(result.begin(), result.end(), fillWord);\n";
            os << "    if ((width % 64U) != 0U) { result.back() &= wolvrix_gsim_low_mask(width % 64U); }\n";
            os << "}\n";
            os << "template <typename T>\n";
            os << "inline std::vector<std::uint64_t> wolvrix_gsim_replicate_bit(\n";
            os << "    const T& value,\n";
            os << "    std::uint32_t width) {\n";
            os << "    std::vector<std::uint64_t> result((width + 63U) / 64U, 0ULL);\n";
            os << "    wolvrix_gsim_replicate_bit_into(result, value, width);\n";
            os << "    return result;\n";
            os << "}\n";
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
            os << "#include <cstddef>\n";
            os << "#include <cstdint>\n";
            os << "#include <map>\n";
            os << "#include <memory>\n";
            os << "#include <string>\n";
            os << "#include <utility>\n";
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
            for (const auto &domain : state.sequentialGlobalPreStmts) {
                headerSequentialDomains.insert(domain.first);
            }
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
                if (state.activityBatchDispatchEnabled) {
                    if (const auto headsIt = state.activitySourceHeadBatches.find(sourceKey);
                        headsIt != state.activitySourceHeadBatches.end() && !headsIt->second.empty()) {
                        emitBatchWordMaskActivation(os, shardWordMasksFor(headsIt->second), " ", "");
                    } else {
                        os << " activate_all_batches();";
                    }
                    return;
                }
                if (!state.enableSharding || !state.enableActivityWatermark || state.shardCount() <= 0) {
                    return;
                }
                if (const auto headsIt = state.activitySourceHeadShards.find(sourceKey);
                    headsIt != state.activitySourceHeadShards.end() && !headsIt->second.empty()) {
                    emitShardWordMaskActivation(os, shardWordMasksFor(headsIt->second), " ", "");
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
                const bool isSequentialClockInput = sequentialClockInputs.count(sanitizedName) > 0;
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
                    if (!state.activityBatchStrictDispatch) {
                        os << "    void activate_all_shards();\n";
                        os << "    void activate_shards(const std::uint32_t* indices, std::size_t count);\n";
                        os << "    void activate_shard(std::uint32_t shard);\n";
                        os << "    void activate_shard_mask(std::uint32_t word, std::uint64_t mask);\n";
                        if (!state.shardActivationRanges.empty()) {
                            os << "    void activate_shard_mask_range(std::uint32_t rangeId);\n";
                        }
                        os << "    void activate_shard_range(std::uint32_t firstShard);\n";
                        if (!state.changedFanoutRanges.empty()) {
                            os << "    void activate_changed_fanout(std::uint32_t fanout);\n";
                        }
                    }
                    if (state.activityBatchDispatchEnabled) {
                        os << "    void activate_all_batches();\n";
                        os << "    void activate_batch(std::uint32_t batch);\n";
                        os << "    void activate_batch_mask(std::uint32_t word, std::uint64_t mask);\n";
                        const int activeBatchWordCount = static_cast<int>((state.activityBatchCount + 63) / 64);
                        for (int word = 0; word < activeBatchWordCount; ++word) {
                            os << "    void run_active_batch_word_" << word << "(std::uint64_t& active_bits_);\n";
                        }
                    }
                    if (!state.activityBatchStrictDispatch) {
                        const int activeWordCount = (state.shardCount() + 63) / 64;
                        for (int word = 0; word < activeWordCount; ++word) {
                            os << "    void run_active_shard_word_" << word << "(std::uint64_t& active_bits_);\n";
                        }
                    }
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
            auto emitScalarTouchedMembers = [&](std::string_view suffix, std::string_view type) {
                os << "    void prepare_domain_next_state" << suffix << "(std::size_t capacity);\n";
                os << "    bool stage_domain_next_state" << suffix << "(bool cond, std::size_t index, " << type
                   << " nextValue, " << type << " mask);\n";
                os << "    bool apply_domain_next_state" << suffix << "();\n";
                os << "    std::vector<" << type << "> domain_next_state" << suffix << "_shadow_;\n";
                os << "    std::vector<std::uint8_t> domain_next_state" << suffix << "_touched_;\n";
                os << "    std::vector<std::uint8_t> domain_next_state" << suffix << "_pending_flags_;\n";
                os << "    std::vector<std::size_t> domain_next_state" << suffix << "_pending_indices_;\n";
            };
            if (state.stateU8Count > 0) {
                emitScalarTouchedMembers("U8", "std::uint8_t");
            }
            if (state.stateU16Count > 0) {
                emitScalarTouchedMembers("U16", "std::uint16_t");
            }
            if (state.stateU32Count > 0) {
                emitScalarTouchedMembers("U32", "std::uint32_t");
            }
            if (state.stateU64Count > 0) {
                emitScalarTouchedMembers("U64", "std::uint64_t");
            }
            if (state.enablePendingWriteStats) {
                os << "    void init_pending_write_stats();\n";
                os << "    void report_pending_write_stats();\n";
                os << "    bool scalar_pending_stats_enabled_ = false;\n";
                os << "    std::uint64_t scalar_pending_stats_interval_ = 50;\n";
                os << "    std::uint64_t scalar_pending_steps_ = 0;\n";
                os << "    std::uint64_t scalar_pending_stage_calls_total_ = 0;\n";
                os << "    std::uint64_t scalar_pending_stage_calls_u8_ = 0;\n";
                os << "    std::uint64_t scalar_pending_stage_calls_u16_ = 0;\n";
                os << "    std::uint64_t scalar_pending_stage_calls_u32_ = 0;\n";
                os << "    std::uint64_t scalar_pending_stage_calls_u64_ = 0;\n";
                os << "    std::uint64_t scalar_pending_accepted_total_ = 0;\n";
                os << "    std::uint64_t scalar_pending_noop_elided_total_ = 0;\n";
                os << "    std::uint64_t scalar_pending_duplicate_writes_total_ = 0;\n";
                os << "    std::uint64_t scalar_pending_apply_calls_total_ = 0;\n";
                os << "    std::uint64_t scalar_pending_apply_slots_total_ = 0;\n";
                os << "    std::uint64_t scalar_pending_unique_touched_last_ = 0;\n";
                os << "    std::uint64_t scalar_pending_unique_touched_max_ = 0;\n";
                os << "    std::uint64_t scalar_pending_unique_touched_step_ = 0;\n";
                os << "    std::uint64_t vector_pending_prepare_calls_ = 0;\n";
                os << "    std::uint64_t vector_pending_apply_calls_ = 0;\n";
                os << "    std::uint64_t vector_pending_apply_entries_ = 0;\n";
            }
            if (state.enableActivityBatchStats) {
                os << "    void init_activity_batch_stats();\n";
                os << "    void report_activity_batch_stats();\n";
                os << "    bool activity_batch_stats_enabled_ = false;\n";
                os << "    std::uint64_t activity_batch_stats_interval_ = 50;\n";
                os << "    std::uint64_t activity_batch_steps_ = 0;\n";
                os << "    std::uint64_t activity_batch_activated_batches_step_ = 0;\n";
                os << "    std::uint64_t activity_batch_executed_batches_step_ = 0;\n";
                os << "    std::uint64_t activity_batch_executed_bodies_step_ = 0;\n";
                os << "    std::uint64_t activity_batch_activated_words_step_ = 0;\n";
                os << "    std::uint64_t activity_batch_queue_max_step_ = 0;\n";
                os << "    std::uint64_t activity_batch_successor_edges_step_ = 0;\n";
                if (!state.activityBatchStrictDispatch) {
                    os << "    std::uint64_t activity_batch_suffix_fallbacks_step_ = 0;\n";
                }
                os << "    std::uint64_t activity_batch_full_replays_step_ = 0;\n";
                os << "    std::uint64_t activity_batch_class_comb_step_ = 0;\n";
                os << "    std::uint64_t activity_batch_class_stateful_step_ = 0;\n";
                os << "    std::uint64_t activity_batch_class_sidefx_step_ = 0;\n";
            }
            if (!state.stateVecWidths.empty()) {
                os << "    void prepare_domain_next_stateVec(std::size_t capacity);\n";
                os << "    void apply_domain_next_stateVec();\n";
                os << "    std::vector<std::pair<std::size_t, std::vector<std::uint64_t>>> domain_next_stateVec_scratch_;\n";
            }
            if (state.enableSharding && state.shardCount() > 0) {
                os << "    bool clock_inputs_dirty_ = true;\n";
                os << "    bool committed_state_dirty_ = true;\n";
            }
            os << "    bool non_clock_inputs_dirty_ = true;\n";
            if (state.enableSharding && state.enableActivityWatermark && state.shardCount() > 0 &&
                !state.activityBatchStrictDispatch) {
                os << "    std::vector<std::uint64_t> active_shard_words_;\n";
                os << "    std::vector<std::uint8_t> active_word_queued_;\n";
                os << "    std::vector<std::uint32_t> active_word_queue_;\n";
            }
            if (state.activityBatchDispatchEnabled) {
                os << "    std::vector<std::uint64_t> active_batch_words_;\n";
                os << "    std::vector<std::uint8_t> active_batch_word_queued_;\n";
                os << "    std::vector<std::uint32_t> active_batch_word_queue_;\n";
            }
            for (const auto &chunk : sequentialChunks) {
                os << "    void " << chunk.methodName
                   << "(bool& committed_, bool& dirty_on_commit_, "
                   << "std::vector<std::pair<std::size_t, std::uint8_t>>* next_stateU8_, "
                   << "std::vector<std::pair<std::size_t, std::uint16_t>>* next_stateU16_, "
                   << "std::vector<std::pair<std::size_t, std::uint32_t>>* next_stateU32_, "
                   << "std::vector<std::pair<std::size_t, std::uint64_t>>* next_stateU64_, "
                   << "std::vector<std::pair<std::size_t, std::vector<std::uint64_t>>>* next_stateVec_);\n";
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
            os << "    std::string activity_batch_mode;\n";
            os << "    bool activity_batch_metadata_present = false;\n";
            os << "    bool activity_batch_metadata_valid = false;\n";
            os << "    std::string activity_batch_fallback_reason;\n";
            os << "    std::int64_t activity_batch_count = 0;\n";
            os << "    std::int64_t activity_batch_avg_ops = 0;\n";
            os << "    std::int64_t activity_batch_max_ops = 0;\n";
            os << "    std::int64_t activity_batch_avg_estimated_lines = 0;\n";
            os << "    std::int64_t activity_batch_max_estimated_lines = 0;\n";
            os << "    std::int64_t activity_batch_max_successor_fanout = 0;\n";
            os << "    std::int64_t activity_batch_successor_edges = 0;\n";
            os << "    std::int64_t activity_batch_entry_count = 0;\n";
            os << "    std::int64_t activity_supernode_active_words = 0;\n";
            os << "    std::int64_t activity_supernode_body_count = 0;\n";
            os << "    std::int64_t activity_supernode_shard_count = 0;\n";
            os << "    std::int64_t activity_batch_to_shard_min_span = 0;\n";
            os << "    std::int64_t activity_batch_to_shard_max_span = 0;\n";
            os << "    bool activity_batch_dispatch_enabled = false;\n";
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
                os << "    std::vector<std::string> schedule_batch_names;\n";
                os << "    std::vector<std::int64_t> schedule_batch_class_ids;\n";
                os << "    std::vector<std::string> schedule_batch_class_names;\n";
                os << "    std::vector<std::int64_t> schedule_batch_flags;\n";
                os << "    std::vector<std::int64_t> schedule_batch_topo_batch_by_pos;\n";
                os << "    std::vector<std::int64_t> schedule_batch_first_topo_pos;\n";
                os << "    std::vector<std::int64_t> schedule_batch_last_topo_pos;\n";
                os << "    std::vector<std::int64_t> schedule_batch_op_counts;\n";
                os << "    std::vector<std::int64_t> schedule_batch_succ_offsets;\n";
                os << "    std::vector<std::int64_t> schedule_batch_succ_targets;\n";
                os << "    std::vector<std::int64_t> schedule_batch_entry_batches;\n";
                os << "    std::vector<std::int64_t> schedule_batch_estimated_lines;\n";
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
            std::map<std::string, std::vector<std::string>> sequentialGlobalPreChunkMethods;
            std::map<std::string, std::vector<std::string>> sequentialStmtChunkMethods;
            std::map<std::string, bool> sequentialChunkHasRegs;
            std::map<std::string, std::size_t> sequentialRegChunkCounts;
            std::map<std::string, SequentialStoragePools> sequentialRegDomainPools;
            auto isDerivedSequentialDomain = [&](const std::string &domainKey) {
                const auto parsedDomain = parseSequentialDomain(domainKey);
                if (!parsedDomain) {
                    return false;
                }
                const std::string resolvedClock =
                    resolveSequentialClockStateName(parsedDomain->second, state.inputPorts);
                const auto exprIt = state.sequentialClockExprs.find(domainKey);
                if (exprIt == state.sequentialClockExprs.end()) {
                    return false;
                }
                return exprIt->second != "input_" + resolvedClock + "_";
            };
            auto sequentialDomainLess = [&](const std::string &lhs, const std::string &rhs) {
                const bool lhsDerived = isDerivedSequentialDomain(lhs);
                const bool rhsDerived = isDerivedSequentialDomain(rhs);
                if (lhsDerived != rhsDerived) {
                    return !lhsDerived && rhsDerived;
                }
                return lhs < rhs;
            };
            std::set<std::string, decltype(sequentialDomainLess)> sequentialDomains(sequentialDomainLess);
            for (const auto &domain : state.sequentialGlobalPreStmts) {
                sequentialDomains.insert(domain.first);
            }
            for (const auto &domain : state.sequentialPreStmts) {
                sequentialDomains.insert(domain.first);
            }
            for (const auto &domain : state.sequentialStmts) {
                sequentialDomains.insert(domain.first);
            }
            for (const auto &chunk : sequentialChunks) {
                sequentialChunkHasRegs[chunk.methodName] = !chunk.regNames.empty();
                if (!chunk.regNames.empty()) {
                    ++sequentialRegChunkCounts[chunk.domainKey];
                    auto &pools = sequentialRegDomainPools[chunk.domainKey];
                    for (const auto &regName : chunk.regNames) {
                        const std::string expr = state.persistentStorageExpr(regName);
                        if (expr.rfind("state_->stateU8[", 0) == 0) {
                            ++pools.stateU8;
                        } else if (expr.rfind("state_->stateU16[", 0) == 0) {
                            ++pools.stateU16;
                        } else if (expr.rfind("state_->stateU32[", 0) == 0) {
                            ++pools.stateU32;
                        } else if (expr.rfind("state_->stateU64[", 0) == 0) {
                            ++pools.stateU64;
                        } else if (expr.rfind("state_->stateVec[", 0) == 0) {
                            ++pools.stateVec;
                        }
                    }
                }
                if (chunk.globalPreReg) {
                    sequentialGlobalPreChunkMethods[chunk.domainKey].push_back(chunk.methodName);
                } else if (chunk.preReg) {
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
            if (state.enablePendingWriteStats || state.enableActivityBatchStats) {
                os << "#include <cstdio>\n";
                os << "#include <cstdlib>\n";
            }
            if (state.emitsDpicCalls) {
                os << "#include <cstring>\n";
                if (state.enableDpicTrace) {
                    os << "#include <iostream>\n";
                }
                os << "#include \"difftest-dpic.h\"\n";
            }
            os << "#include <set>\n\n";
            if (state.enablePendingWriteStats || state.enableActivityBatchStats) {
                os << "namespace {\n";
                os << "bool wolvrix_gsim_env_truthy(const char* value) {\n";
                os << "    if (value == nullptr || *value == '\\0') { return false; }\n";
                os << "    if ((value[0] == '0' || value[0] == 'n' || value[0] == 'N') && value[1] == '\\0') { return false; }\n";
                os << "    return true;\n";
                os << "}\n";
                os << "std::uint64_t wolvrix_gsim_env_u64(const char* value, std::uint64_t fallback) {\n";
                os << "    if (value == nullptr || *value == '\\0') { return fallback; }\n";
                os << "    char* end = nullptr;\n";
                os << "    const auto parsed = std::strtoull(value, &end, 10);\n";
                os << "    return end != value && parsed > 0ULL ? static_cast<std::uint64_t>(parsed) : fallback;\n";
                os << "}\n";
                os << "} // namespace\n\n";
            }
            if (state.enableSharding && state.enableActivityWatermark && !state.activityBatchStrictDispatch &&
                (!state.changedFanoutRanges.empty() || !state.shardActivationRanges.empty())) {
                os << "namespace {\n";
                os << "struct WolvrixGsimShardActivationMask { std::uint32_t word; std::uint64_t mask; };\n";
                os << "struct WolvrixGsimShardActivationRange { std::uint32_t offset; std::uint32_t count; };\n";
                if (!state.changedFanoutRanges.empty()) {
                    os << "constexpr WolvrixGsimShardActivationMask kChangedFanoutMasks[] = {\n";
                    for (const auto& [word, mask] : state.changedFanoutMasks) {
                        os << "    {" << word << "U, UINT64_C(" << mask << ")},\n";
                    }
                    os << "};\n";
                    os << "constexpr WolvrixGsimShardActivationRange kChangedFanoutRanges[] = {\n";
                    for (const auto& [offset, count] : state.changedFanoutRanges) {
                        os << "    {" << offset << "U, " << count << "U},\n";
                    }
                    os << "};\n";
                }
                if (!state.shardActivationRanges.empty()) {
                    os << "constexpr WolvrixGsimShardActivationMask kShardActivationMasks[] = {\n";
                    for (const auto& [word, mask] : state.shardActivationMasks) {
                        os << "    {" << word << "U, UINT64_C(" << mask << ")},\n";
                    }
                    os << "};\n";
                    os << "constexpr WolvrixGsimShardActivationRange kShardActivationRanges[] = {\n";
                    for (const auto& [offset, count] : state.shardActivationRanges) {
                        os << "    {" << offset << "U, " << count << "U},\n";
                    }
                    os << "};\n";
                }
                os << "} // namespace\n\n";
            }
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

            os << "SSimTop::SSimTop() : state_(new SSimTopState()), evalTemps_(new SSimTopEvalTemps())";
            auto appendCtorInit = [&](const std::string &text) {
                os << ", " << text;
            };
            if (state.stateU8Count > 0) {
                appendCtorInit("domain_next_stateU8_shadow_(" + std::to_string(state.stateU8Count) + "U, 0)");
                appendCtorInit("domain_next_stateU8_touched_(" + std::to_string(state.stateU8Count) + "U, 0)");
                appendCtorInit("domain_next_stateU8_pending_flags_(" + std::to_string(state.stateU8Count) + "U, 0)");
            }
            if (state.stateU16Count > 0) {
                appendCtorInit("domain_next_stateU16_shadow_(" + std::to_string(state.stateU16Count) + "U, 0)");
                appendCtorInit("domain_next_stateU16_touched_(" + std::to_string(state.stateU16Count) + "U, 0)");
                appendCtorInit("domain_next_stateU16_pending_flags_(" + std::to_string(state.stateU16Count) + "U, 0)");
            }
            if (state.stateU32Count > 0) {
                appendCtorInit("domain_next_stateU32_shadow_(" + std::to_string(state.stateU32Count) + "U, 0)");
                appendCtorInit("domain_next_stateU32_touched_(" + std::to_string(state.stateU32Count) + "U, 0)");
                appendCtorInit("domain_next_stateU32_pending_flags_(" + std::to_string(state.stateU32Count) + "U, 0)");
            }
            if (state.stateU64Count > 0) {
                appendCtorInit("domain_next_stateU64_shadow_(" + std::to_string(state.stateU64Count) + "U, 0)");
                appendCtorInit("domain_next_stateU64_touched_(" + std::to_string(state.stateU64Count) + "U, 0)");
                appendCtorInit("domain_next_stateU64_pending_flags_(" + std::to_string(state.stateU64Count) + "U, 0)");
            }
            if (state.enableSharding && state.enableActivityWatermark && state.shardCount() > 0 &&
                !state.activityBatchStrictDispatch) {
                appendCtorInit("active_shard_words_((" + std::to_string(state.shardCount()) + "U + 63U) / 64U, UINT64_C(0))");
                appendCtorInit("active_word_queued_((" + std::to_string(state.shardCount()) + "U + 63U) / 64U, 0)");
                appendCtorInit("active_word_queue_()");
            }
            if (state.activityBatchDispatchEnabled) {
                appendCtorInit("active_batch_words_((" + std::to_string(state.activityBatchCount) + "U + 63U) / 64U, UINT64_C(0))");
                appendCtorInit("active_batch_word_queued_((" + std::to_string(state.activityBatchCount) + "U + 63U) / 64U, 0)");
                appendCtorInit("active_batch_word_queue_()");
            }
            if (state.enablePendingWriteStats || state.enableActivityBatchStats) {
                os << " {";
                if (state.enablePendingWriteStats) {
                    os << " init_pending_write_stats();";
                }
                if (state.enableActivityBatchStats) {
                    os << " init_activity_batch_stats();";
                }
                os << " reset(); }\n";
            } else {
                os << " { reset(); }\n";
            }

            os << "SSimTop::~SSimTop() { delete evalTemps_; delete state_; }\n\n";
            if (state.enablePendingWriteStats) {
                os << "void SSimTop::init_pending_write_stats() {\n";
                os << "    scalar_pending_stats_enabled_ = wolvrix_gsim_env_truthy(std::getenv(\"WOLVRIX_GSIM_PENDING_WRITE_STATS\")) || wolvrix_gsim_env_truthy(std::getenv(\"WOLVRIX_XS_GSIM_PENDING_WRITE_STATS\"));\n";
                os << "    scalar_pending_stats_interval_ = wolvrix_gsim_env_u64(std::getenv(\"WOLVRIX_GSIM_PENDING_WRITE_STATS_INTERVAL\"), 50ULL);\n";
                os << "}\n\n";
                os << "void SSimTop::report_pending_write_stats() {\n";
                os << "    if (!scalar_pending_stats_enabled_) { return; }\n";
                os << "    scalar_pending_unique_touched_last_ = scalar_pending_unique_touched_step_;\n";
                os << "    if (scalar_pending_unique_touched_last_ > scalar_pending_unique_touched_max_) { scalar_pending_unique_touched_max_ = scalar_pending_unique_touched_last_; }\n";
                os << "    if (scalar_pending_stats_interval_ != 0ULL && (scalar_pending_steps_ % scalar_pending_stats_interval_) != 0ULL) { return; }\n";
                os << "    std::fprintf(stderr, \"[gsim] scalar_pending_writes step=%llu stage_calls=%llu stage_u8=%llu stage_u16=%llu stage_u32=%llu stage_u64=%llu accepted=%llu unique_last=%llu unique_max=%llu apply_calls=%llu apply_slots=%llu duplicates=%llu noops=%llu vector_prepare=%llu vector_apply=%llu vector_entries=%llu\\n\",\n";
                os << "                 static_cast<unsigned long long>(scalar_pending_steps_),\n";
                os << "                 static_cast<unsigned long long>(scalar_pending_stage_calls_total_),\n";
                os << "                 static_cast<unsigned long long>(scalar_pending_stage_calls_u8_),\n";
                os << "                 static_cast<unsigned long long>(scalar_pending_stage_calls_u16_),\n";
                os << "                 static_cast<unsigned long long>(scalar_pending_stage_calls_u32_),\n";
                os << "                 static_cast<unsigned long long>(scalar_pending_stage_calls_u64_),\n";
                os << "                 static_cast<unsigned long long>(scalar_pending_accepted_total_),\n";
                os << "                 static_cast<unsigned long long>(scalar_pending_unique_touched_last_),\n";
                os << "                 static_cast<unsigned long long>(scalar_pending_unique_touched_max_),\n";
                os << "                 static_cast<unsigned long long>(scalar_pending_apply_calls_total_),\n";
                os << "                 static_cast<unsigned long long>(scalar_pending_apply_slots_total_),\n";
                os << "                 static_cast<unsigned long long>(scalar_pending_duplicate_writes_total_),\n";
                os << "                 static_cast<unsigned long long>(scalar_pending_noop_elided_total_),\n";
                os << "                 static_cast<unsigned long long>(vector_pending_prepare_calls_),\n";
                os << "                 static_cast<unsigned long long>(vector_pending_apply_calls_),\n";
                os << "                 static_cast<unsigned long long>(vector_pending_apply_entries_));\n";
                os << "}\n\n";
            }
            if (state.enableActivityBatchStats) {
                os << "void SSimTop::init_activity_batch_stats() {\n";
                os << "    activity_batch_stats_enabled_ = wolvrix_gsim_env_truthy(std::getenv(\"WOLVRIX_GSIM_ACTIVITY_BATCH_STATS\"));\n";
                os << "    activity_batch_stats_interval_ = wolvrix_gsim_env_u64(std::getenv(\"WOLVRIX_GSIM_ACTIVITY_BATCH_STATS_INTERVAL\"), 50ULL);\n";
                os << "    if (activity_batch_stats_enabled_) {\n";
                os << "        std::fprintf(stderr, \"[gsim] activity_batch_plan batches=%llu active_words=%llu bodies=%llu avg_ops=%llu max_ops=%llu max_fanout=%llu fallback=%s\\n\",\n";
                os << "                     static_cast<unsigned long long>(" << state.activityBatchCount << "ULL),\n";
                os << "                     static_cast<unsigned long long>(" << state.activitySupernodeActiveWords << "ULL),\n";
                os << "                     static_cast<unsigned long long>(" << state.activitySupernodeBodyCount << "ULL),\n";
                os << "                     static_cast<unsigned long long>(" << state.activityBatchAvgOps << "ULL),\n";
                os << "                     static_cast<unsigned long long>(" << state.activityBatchMaxOps << "ULL),\n";
                os << "                     static_cast<unsigned long long>(" << state.activityBatchMaxSuccessorFanout << "ULL),\n";
                os << "                     \"" << state.activityBatchFallbackReason << "\");\n";
                os << "    }\n";
                os << "}\n\n";
                os << "void SSimTop::report_activity_batch_stats() {\n";
                os << "    if (!activity_batch_stats_enabled_) { return; }\n";
                os << "    ++activity_batch_steps_;\n";
                os << "    if (activity_batch_stats_interval_ != 0ULL && (activity_batch_steps_ % activity_batch_stats_interval_) != 0ULL) { return; }\n";
                os << "    std::fprintf(stderr, \"[gsim] activity_batch_stats step=%llu interval=%llu activated_batches=%llu executed_batches=%llu executed_bodies=%llu activated_words=%llu queue_max=%llu successor_edges=%llu ";
                if (!state.activityBatchStrictDispatch) {
                    os << "suffix_fallbacks=%llu ";
                }
                os << "full_replays=%llu class_comb=%llu class_stateful=%llu class_sidefx=%llu\\n\",\n";
                os << "                 static_cast<unsigned long long>(activity_batch_steps_),\n";
                os << "                 static_cast<unsigned long long>(activity_batch_stats_interval_),\n";
                os << "                 static_cast<unsigned long long>(activity_batch_activated_batches_step_),\n";
                os << "                 static_cast<unsigned long long>(activity_batch_executed_batches_step_),\n";
                os << "                 static_cast<unsigned long long>(activity_batch_executed_bodies_step_),\n";
                os << "                 static_cast<unsigned long long>(activity_batch_activated_words_step_),\n";
                os << "                 static_cast<unsigned long long>(activity_batch_queue_max_step_),\n";
                os << "                 static_cast<unsigned long long>(activity_batch_successor_edges_step_),\n";
                if (!state.activityBatchStrictDispatch) {
                    os << "                 static_cast<unsigned long long>(activity_batch_suffix_fallbacks_step_),\n";
                }
                os << "                 static_cast<unsigned long long>(activity_batch_full_replays_step_),\n";
                os << "                 static_cast<unsigned long long>(activity_batch_class_comb_step_),\n";
                os << "                 static_cast<unsigned long long>(activity_batch_class_stateful_step_),\n";
                os << "                 static_cast<unsigned long long>(activity_batch_class_sidefx_step_));\n";
                os << "    activity_batch_activated_batches_step_ = 0;\n";
                os << "    activity_batch_executed_batches_step_ = 0;\n";
                os << "    activity_batch_executed_bodies_step_ = 0;\n";
                os << "    activity_batch_activated_words_step_ = 0;\n";
                os << "    activity_batch_queue_max_step_ = 0;\n";
                os << "    activity_batch_successor_edges_step_ = 0;\n";
                if (!state.activityBatchStrictDispatch) {
                    os << "    activity_batch_suffix_fallbacks_step_ = 0;\n";
                }
                os << "    activity_batch_full_replays_step_ = 0;\n";
                os << "    activity_batch_class_comb_step_ = 0;\n";
                os << "    activity_batch_class_stateful_step_ = 0;\n";
                os << "    activity_batch_class_sidefx_step_ = 0;\n";
                os << "}\n\n";
            }
            auto emitDomainNextScalarHelpers = [&](std::string_view suffix,
                                                       std::string_view statePool,
                                                       std::string_view type,
                                                       std::string_view maxConstant) {
                const std::string suffixLower(suffix == "U8" ? "u8" : suffix == "U16" ? "u16" : suffix == "U32" ? "u32" : "u64");
                os << "void SSimTop::prepare_domain_next_state" << suffix << "(std::size_t capacity) {\n";
                os << "    for (const auto idx_ : domain_next_state" << suffix << "_pending_indices_) {\n";
                os << "        domain_next_state" << suffix << "_touched_[idx_] = 0;\n";
                os << "        domain_next_state" << suffix << "_pending_flags_[idx_] = 0;\n";
                os << "    }\n";
                os << "    domain_next_state" << suffix << "_pending_indices_.clear();\n";
                os << "    if (domain_next_state" << suffix << "_pending_indices_.capacity() < capacity) {\n";
                os << "        domain_next_state" << suffix << "_pending_indices_.reserve(capacity);\n";
                os << "    }\n";
                os << "}\n\n";
                os << "bool SSimTop::stage_domain_next_state" << suffix << "(bool cond, std::size_t index, "
                   << type << " nextValue, " << type << " mask) {\n";
                if (state.enablePendingWriteStats) {
                    os << "    ++scalar_pending_stage_calls_total_;\n";
                    os << "    ++scalar_pending_stage_calls_" << suffixLower << "_;\n";
                }
                os << "    if (!cond || mask == static_cast<" << type << ">(0)) {\n";
                if (state.enablePendingWriteStats) {
                    os << "        ++scalar_pending_noop_elided_total_;\n";
                }
                os << "        return false;\n";
                os << "    }\n";
                os << "    const auto base_ = domain_next_state" << suffix << "_touched_[index] ? domain_next_state"
                   << suffix << "_shadow_[index] : state_->" << statePool << "[index];\n";
                os << "    const auto merged_ = (mask == " << maxConstant << ") ? nextValue : static_cast<" << type
                   << ">((base_ & static_cast<" << type << ">(~mask)) | (nextValue & mask));\n";
                os << "    if (merged_ == base_) {\n";
                if (state.enablePendingWriteStats) {
                    os << "        ++scalar_pending_noop_elided_total_;\n";
                }
                os << "        return false;\n";
                os << "    }\n";
                if (state.enablePendingWriteStats) {
                    os << "    if (domain_next_state" << suffix << "_pending_flags_[index]) { ++scalar_pending_duplicate_writes_total_; }\n";
                }
                os << "    domain_next_state" << suffix << "_shadow_[index] = merged_;\n";
                os << "    domain_next_state" << suffix << "_touched_[index] = 1;\n";
                os << "    if (!domain_next_state" << suffix << "_pending_flags_[index]) {\n";
                os << "        domain_next_state" << suffix << "_pending_flags_[index] = 1;\n";
                os << "        domain_next_state" << suffix << "_pending_indices_.push_back(index);\n";
                os << "    }\n";
                if (state.enablePendingWriteStats) {
                    os << "    ++scalar_pending_accepted_total_;\n";
                }
                os << "    return true;\n";
                os << "}\n\n";
                os << "bool SSimTop::apply_domain_next_state" << suffix << "() {\n";
                os << "    const bool applied_ = !domain_next_state" << suffix << "_pending_indices_.empty();\n";
                if (state.enablePendingWriteStats) {
                    os << "    ++scalar_pending_apply_calls_total_;\n";
                    os << "    scalar_pending_apply_slots_total_ += static_cast<std::uint64_t>(domain_next_state" << suffix << "_pending_indices_.size());\n";
                    os << "    scalar_pending_unique_touched_step_ += static_cast<std::uint64_t>(domain_next_state" << suffix << "_pending_indices_.size());\n";
                }
                os << "    for (const auto idx_ : domain_next_state" << suffix << "_pending_indices_) {\n";
                os << "        state_->" << statePool << "[idx_] = domain_next_state" << suffix << "_shadow_[idx_];\n";
                os << "    }\n";
                os << "    return applied_;\n";
                os << "}\n\n";
            };
            if (state.stateU8Count > 0) {
                emitDomainNextScalarHelpers("U8", "stateU8", "std::uint8_t", "UINT8_MAX");
            }
            if (state.stateU16Count > 0) {
                emitDomainNextScalarHelpers("U16", "stateU16", "std::uint16_t", "UINT16_MAX");
            }
            if (state.stateU32Count > 0) {
                emitDomainNextScalarHelpers("U32", "stateU32", "std::uint32_t", "UINT32_MAX");
            }
            if (state.stateU64Count > 0) {
                emitDomainNextScalarHelpers("U64", "stateU64", "std::uint64_t", "UINT64_MAX");
            }
            if (!state.stateVecWidths.empty()) {
                os << "void SSimTop::prepare_domain_next_stateVec(std::size_t capacity) {\n";
                if (state.enablePendingWriteStats) {
                    os << "    ++vector_pending_prepare_calls_;\n";
                }
                os << "    domain_next_stateVec_scratch_.clear();\n";
                os << "    if (domain_next_stateVec_scratch_.capacity() < capacity) {\n";
                os << "        domain_next_stateVec_scratch_.reserve(capacity);\n";
                os << "    }\n";
                os << "}\n\n";
                os << "void SSimTop::apply_domain_next_stateVec() {\n";
                if (state.enablePendingWriteStats) {
                    os << "    ++vector_pending_apply_calls_;\n";
                    os << "    vector_pending_apply_entries_ += static_cast<std::uint64_t>(domain_next_stateVec_scratch_.size());\n";
                }
                os << "    for (auto& write_ : domain_next_stateVec_scratch_) {\n";
                os << "        state_->stateVec[write_.first] = std::move(write_.second);\n";
                os << "    }\n";
                os << "}\n\n";
            }
            if (hasInputPortNamed(state.inputPorts, "reset")) {
                os << "void SSimTop::set_reset(unsigned reset) { const auto value = static_cast<std::uint8_t>(reset); ";
                os << "if (!(input_reset_ == value)) { input_reset_ = value; non_clock_inputs_dirty_ = true;";
                if (state.activityBatchDispatchEnabled) {
                    if (const auto headsIt = state.activitySourceHeadBatches.find("input_reset");
                        headsIt != state.activitySourceHeadBatches.end() && !headsIt->second.empty()) {
                        emitBatchWordMaskActivation(os, shardWordMasksFor(headsIt->second), " ", "");
                    } else {
                        os << " activate_all_batches();";
                    }
                } else if (state.enableSharding && state.enableActivityWatermark && state.shardCount() > 0) {
                    if (const auto headsIt = state.activitySourceHeadShards.find("input_reset");
                        headsIt != state.activitySourceHeadShards.end() && !headsIt->second.empty()) {
                        emitShardWordMaskActivation(os, shardWordMasksFor(headsIt->second), " ", "");
                    } else if (const auto shardIt = state.activitySourceFirstShard.find("input_reset");
                               shardIt != state.activitySourceFirstShard.end() && shardIt->second >= 0) {
                        os << " activate_shard_range(" << shardIt->second << "U);";
                    }
                }
                os << " } }\n\n";
            } else {
                os << "void SSimTop::set_reset(unsigned reset) { reset_ = reset; }\n\n";
            }
            auto emitActiveShardWordDispatch = [&](int activeWord) {
                os << "void SSimTop::run_active_shard_word_" << activeWord
                   << "(std::uint64_t& active_bits_) {\n";
                os << "    while (active_bits_ != UINT64_C(0)) {\n";
                os << "        const std::uint32_t active_bit_ = static_cast<std::uint32_t>(__builtin_ctzll(active_bits_));\n";
                os << "        active_bits_ &= ~(UINT64_C(1) << active_bit_);\n";
                os << "        const std::uint32_t active_shard_ = " << (activeWord * 64) << "U + active_bit_;\n";
                os << "        switch (active_shard_) {\n";
                const int firstShard = activeWord * 64;
                const int lastShard = std::min(state.shardCount(), firstShard + 64);
                for (int i = firstShard; i < lastShard; ++i) {
                    os << "        case " << i << "U: { sched_" << i << "();";
                    const auto& succ = (i < static_cast<int>(state.shardSuccessors.size())) ? state.shardSuccessors[static_cast<std::size_t>(i)] : std::set<int>{};
                    if (!succ.empty()) {
                        std::uint64_t localSuccessorMask = 0;
                        std::map<int, std::uint64_t> crossWordMasks;
                        for (int successor : succ) {
                            if (successor < 0) {
                                continue;
                            }
                            const int word = successor / 64;
                            const int bit = successor % 64;
                            if (word == activeWord) {
                                localSuccessorMask |= (std::uint64_t{1} << bit);
                            } else {
                                crossWordMasks[word] |= (std::uint64_t{1} << bit);
                            }
                        }
                        if (localSuccessorMask != 0U) {
                            os << " active_bits_ |= UINT64_C(" << localSuccessorMask << ");";
                        }
                        os << buildShardActivation(state, crossWordMasks);
                    }
                    os << " break; }\n";
                }
                os << "        default: break;\n";
                os << "        }\n";
                os << "        const std::uint64_t local_bits_ = active_shard_words_[" << activeWord << "U];\n";
                os << "        if (local_bits_ != UINT64_C(0)) {\n";
                os << "            active_bits_ |= local_bits_;\n";
                os << "            active_shard_words_[" << activeWord << "U] = UINT64_C(0);\n";
                os << "        }\n";
                os << "    }\n";
                os << "}\n\n";
            };
            if (state.enableSharding && state.enableActivityWatermark && state.shardCount() > 0 &&
                !state.activityBatchStrictDispatch) {
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
                if (!state.shardActivationRanges.empty()) {
                    os << "void SSimTop::activate_shard_mask_range(std::uint32_t rangeId) {\n";
                    os << "    if (rangeId >= (sizeof(kShardActivationRanges) / sizeof(kShardActivationRanges[0]))) { return; }\n";
                    os << "    const auto range = kShardActivationRanges[rangeId];\n";
                    os << "    for (std::uint32_t i = 0; i < range.count; ++i) {\n";
                    os << "        const auto entry = kShardActivationMasks[range.offset + i];\n";
                    os << "        activate_shard_mask(entry.word, entry.mask);\n";
                    os << "    }\n";
                    os << "}\n\n";
                }
                os << "void SSimTop::activate_shard_range(std::uint32_t firstShard) {\n";
                os << "    if (firstShard >= " << state.shardCount() << "U) { return; }\n";
                os << "    const std::uint32_t first_word_ = firstShard / 64U;\n";
                os << "    const std::uint64_t first_mask_ = ~UINT64_C(0) << (firstShard % 64U);\n";
                os << "    for (std::uint32_t word = first_word_; word < active_shard_words_.size(); ++word) {\n";
                os << "        std::uint64_t range_mask_ = ~UINT64_C(0);\n";
                os << "        if (word == first_word_) { range_mask_ &= first_mask_; }\n";
                if ((state.shardCount() % 64U) != 0U) {
                    const std::uint64_t lastActiveShardWordMask =
                        (UINT64_C(1) << (state.shardCount() % 64U)) - UINT64_C(1);
                    os << "        if (word + 1U == active_shard_words_.size()) { range_mask_ &= UINT64_C("
                       << lastActiveShardWordMask << "); }\n";
                }
                os << "        activate_shard_mask(word, range_mask_);\n";
                os << "    }\n";
                os << "}\n\n";
                if (!state.changedFanoutRanges.empty()) {
                    os << "void SSimTop::activate_changed_fanout(std::uint32_t fanout) {\n";
                    os << "    if (fanout >= (sizeof(kChangedFanoutRanges) / sizeof(kChangedFanoutRanges[0]))) { return; }\n";
                    os << "    const auto range = kChangedFanoutRanges[fanout];\n";
                    os << "    for (std::uint32_t i = 0; i < range.count; ++i) {\n";
                    os << "        const auto entry = kChangedFanoutMasks[range.offset + i];\n";
                    os << "        activate_shard_mask(entry.word, entry.mask);\n";
                    os << "    }\n";
                    os << "}\n\n";
                }
                const int activeWordCount = (state.shardCount() + 63) / 64;
                for (int activeWord = 0; activeWord < activeWordCount; ++activeWord) {
                    emitActiveShardWordDispatch(activeWord);
                }
            }
            if (state.activityBatchDispatchEnabled && metadata.scheduleBatch) {
                const auto& batch = *metadata.scheduleBatch;
                os << "void SSimTop::activate_all_batches() {\n";
                os << "    active_batch_word_queue_.clear();\n";
                if ((state.activityBatchCount % 64U) == 0U) {
                    os << "    std::fill(active_batch_words_.begin(), active_batch_words_.end(), ~UINT64_C(0));\n";
                } else {
                    const std::uint64_t lastActiveBatchWordMask =
                        (UINT64_C(1) << (state.activityBatchCount % 64U)) - UINT64_C(1);
                    os << "    std::fill(active_batch_words_.begin(), active_batch_words_.end(), ~UINT64_C(0));\n";
                    os << "    if (!active_batch_words_.empty()) { active_batch_words_.back() = UINT64_C("
                       << lastActiveBatchWordMask << "); }\n";
                }
                os << "    std::fill(active_batch_word_queued_.begin(), active_batch_word_queued_.end(), 1);\n";
                os << "    active_batch_word_queue_.reserve(active_batch_words_.size());\n";
                os << "    for (std::uint32_t word = 0; word < active_batch_words_.size(); ++word) { active_batch_word_queue_.push_back(word); }\n";
                os << "    if (activity_batch_stats_enabled_) {\n";
                os << "        ++activity_batch_full_replays_step_;\n";
                os << "        activity_batch_activated_batches_step_ += " << state.activityBatchCount << "ULL;\n";
                os << "        activity_batch_activated_words_step_ += static_cast<std::uint64_t>(active_batch_word_queue_.size());\n";
                os << "        if (active_batch_word_queue_.size() > activity_batch_queue_max_step_) { activity_batch_queue_max_step_ = static_cast<std::uint64_t>(active_batch_word_queue_.size()); }\n";
                os << "    }\n";
                os << "}\n\n";
                os << "void SSimTop::activate_batch(std::uint32_t batch) {\n";
                os << "    if (batch >= " << state.activityBatchCount << "U) { return; }\n";
                os << "    const std::uint32_t word = batch / 64U;\n";
                os << "    const std::uint64_t mask = (UINT64_C(1) << (batch % 64U));\n";
                os << "    activate_batch_mask(word, mask);\n";
                os << "}\n\n";
                os << "void SSimTop::activate_batch_mask(std::uint32_t word, std::uint64_t mask) {\n";
                os << "    if (word >= active_batch_words_.size() || mask == UINT64_C(0)) { return; }\n";
                os << "    const std::uint64_t old_bits_ = active_batch_words_[word];\n";
                os << "    const std::uint64_t new_bits_ = old_bits_ | mask;\n";
                os << "    if (new_bits_ == old_bits_) { return; }\n";
                os << "    active_batch_words_[word] = new_bits_;\n";
                os << "    if (activity_batch_stats_enabled_) {\n";
                os << "        activity_batch_activated_batches_step_ += static_cast<std::uint64_t>(__builtin_popcountll(new_bits_ & ~old_bits_));\n";
                os << "        if (old_bits_ == UINT64_C(0)) { ++activity_batch_activated_words_step_; }\n";
                os << "    }\n";
                os << "    if (active_batch_word_queued_[word] == 0) { active_batch_word_queued_[word] = 1; active_batch_word_queue_.push_back(word); }\n";
                os << "    if (activity_batch_stats_enabled_ && active_batch_word_queue_.size() > activity_batch_queue_max_step_) { activity_batch_queue_max_step_ = static_cast<std::uint64_t>(active_batch_word_queue_.size()); }\n";
                os << "}\n\n";
                const int activeBatchWordCount = static_cast<int>((state.activityBatchCount + 63) / 64);
                for (int activeWord = 0; activeWord < activeBatchWordCount; ++activeWord) {
                    os << "void SSimTop::run_active_batch_word_" << activeWord
                       << "(std::uint64_t& active_bits_) {\n";
                    os << "    while (active_bits_ != UINT64_C(0)) {\n";
                    os << "        const std::uint32_t active_bit_ = static_cast<std::uint32_t>(__builtin_ctzll(active_bits_));\n";
                    os << "        active_bits_ &= ~(UINT64_C(1) << active_bit_);\n";
                    os << "        const std::uint32_t active_batch_ = " << (activeWord * 64) << "U + active_bit_;\n";
                    os << "        switch (active_batch_) {\n";
                    const int firstBatch = activeWord * 64;
                    const int lastBatch = std::min(static_cast<int>(state.activityBatchCount), firstBatch + 64);
                    for (int batchIndex = firstBatch; batchIndex < lastBatch; ++batchIndex) {
                        os << "        case " << batchIndex << "U: {\n";
                        os << "            if (activity_batch_stats_enabled_) {\n";
                        os << "                ++activity_batch_executed_batches_step_;\n";
                        os << "                ++activity_batch_executed_bodies_step_;\n";
                        const auto flags = (batchIndex < static_cast<int>(batch.flags.size())) ? batch.flags[static_cast<std::size_t>(batchIndex)] : 0;
                        if ((flags & 1) != 0) {
                            os << "                ++activity_batch_class_comb_step_;\n";
                        }
                        if ((flags & 2) != 0) {
                            os << "                ++activity_batch_class_stateful_step_;\n";
                        }
                        if ((flags & 4) != 0) {
                            os << "                ++activity_batch_class_sidefx_step_;\n";
                        }
                        os << "            }\n";
                        if (batchIndex < static_cast<int>(state.activityBatchShardSpans.size())) {
                            const auto [firstShard, lastShard] = state.activityBatchShardSpans[static_cast<std::size_t>(batchIndex)];
                            if (firstShard >= 0 && lastShard >= firstShard) {
                                for (int64_t shard = firstShard; shard <= lastShard; ++shard) {
                                    os << "            sched_" << shard << "();\n";
                                }
                            }
                        }
                        std::uint64_t localSuccessorMask = 0;
                        std::map<int, std::uint64_t> crossWordMasks;
                        if (batchIndex + 1 < static_cast<int>(batch.succOffsets.size())) {
                            const auto start = batch.succOffsets[static_cast<std::size_t>(batchIndex)];
                            const auto end = batch.succOffsets[static_cast<std::size_t>(batchIndex + 1)];
                            for (int64_t cursor = start; cursor < end && cursor >= 0 &&
                                                   static_cast<std::size_t>(cursor) < batch.succTargets.size(); ++cursor) {
                                const auto successor = batch.succTargets[static_cast<std::size_t>(cursor)];
                                if (successor < 0) {
                                    continue;
                                }
                                const int word = static_cast<int>(successor / 64);
                                const int bit = static_cast<int>(successor % 64);
                                if (word == activeWord) {
                                    localSuccessorMask |= (std::uint64_t{1} << bit);
                                } else {
                                    crossWordMasks[word] |= (std::uint64_t{1} << bit);
                                }
                            }
                            os << "            if (activity_batch_stats_enabled_) { activity_batch_successor_edges_step_ += "
                               << std::max<int64_t>(0, end - start) << "ULL; }\n";
                        }
                        if (localSuccessorMask != 0U) {
                            os << "            active_bits_ |= UINT64_C(" << localSuccessorMask << ");\n";
                        }
                        for (const auto& [word, mask] : crossWordMasks) {
                            os << "            activate_batch_mask(" << word << "U, UINT64_C(" << mask << "));\n";
                        }
                        os << "            break;\n";
                        os << "        }\n";
                    }
                    os << "        default: break;\n";
                    os << "        }\n";
                    os << "        const std::uint64_t local_bits_ = active_batch_words_[" << activeWord << "U];\n";
                    os << "        if (local_bits_ != UINT64_C(0)) {\n";
                    os << "            active_bits_ |= local_bits_;\n";
                    os << "            active_batch_words_[" << activeWord << "U] = UINT64_C(0);\n";
                    os << "        }\n";
                    os << "    }\n";
                    os << "}\n\n";
                }
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
                if (state.activityBatchDispatchEnabled) {
                    os << "    activate_all_batches();\n";
                } else {
                    os << "    activate_all_shards();\n";
                }
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
                if (state.activityBatchDispatchEnabled) {
                    os << "    std::size_t active_batch_cursor_ = 0;\n";
                    os << "    while (active_batch_cursor_ < active_batch_word_queue_.size()) {\n";
                    os << "        const std::uint32_t active_word_ = active_batch_word_queue_[active_batch_cursor_++];\n";
                    os << "        if (active_word_ >= active_batch_words_.size()) { continue; }\n";
                    os << "        std::uint64_t active_bits_ = active_batch_words_[active_word_];\n";
                    os << "        active_batch_words_[active_word_] = UINT64_C(0);\n";
                    os << "        switch (active_word_) {\n";
                    const int activeBatchWordCount = static_cast<int>((state.activityBatchCount + 63) / 64);
                    for (int word = 0; word < activeBatchWordCount; ++word) {
                        os << "        case " << word << "U: run_active_batch_word_" << word << "(active_bits_); break;\n";
                    }
                    os << "        default: break;\n";
                    os << "        }\n";
                    os << "        active_batch_word_queued_[active_word_] = 0;\n";
                    os << "    }\n";
                    os << "    active_batch_word_queue_.clear();\n";
                    os << "    std::fill(active_batch_words_.begin(), active_batch_words_.end(), UINT64_C(0));\n";
                    os << "    std::fill(active_batch_word_queued_.begin(), active_batch_word_queued_.end(), 0);\n";
                    if (!state.activityBatchStrictDispatch) {
                        os << "    if (!active_word_queue_.empty()) {\n";
                        os << "        if (activity_batch_stats_enabled_) { ++activity_batch_suffix_fallbacks_step_; }\n";
                        os << "        std::size_t active_cursor_ = 0;\n";
                        os << "        while (active_cursor_ < active_word_queue_.size()) {\n";
                        os << "            const std::uint32_t active_word_ = active_word_queue_[active_cursor_++];\n";
                        os << "            if (active_word_ >= active_shard_words_.size()) { continue; }\n";
                        os << "            std::uint64_t active_bits_ = active_shard_words_[active_word_];\n";
                        os << "            active_shard_words_[active_word_] = UINT64_C(0);\n";
                        os << "            switch (active_word_) {\n";
                        const int activeWordCount = (state.shardCount() + 63) / 64;
                        for (int word = 0; word < activeWordCount; ++word) {
                            os << "            case " << word << "U: run_active_shard_word_" << word << "(active_bits_); break;\n";
                        }
                        os << "            default: break;\n";
                        os << "            }\n";
                        os << "            active_word_queued_[active_word_] = 0;\n";
                        os << "        }\n";
                        os << "        active_word_queue_.clear();\n";
                        os << "        std::fill(active_shard_words_.begin(), active_shard_words_.end(), UINT64_C(0));\n";
                        os << "        std::fill(active_word_queued_.begin(), active_word_queued_.end(), 0);\n";
                        os << "    }\n";
                    }
                } else if (state.enableActivityWatermark) {
                    os << "    std::size_t active_cursor_ = 0;\n";
                    os << "    while (active_cursor_ < active_word_queue_.size()) {\n";
                    os << "        const std::uint32_t active_word_ = active_word_queue_[active_cursor_++];\n";
                    os << "        if (active_word_ >= active_shard_words_.size()) { continue; }\n";
                    os << "        std::uint64_t active_bits_ = active_shard_words_[active_word_];\n";
                    os << "        active_shard_words_[active_word_] = UINT64_C(0);\n";
                    os << "        switch (active_word_) {\n";
                    const int activeWordCount = (state.shardCount() + 63) / 64;
                    for (int word = 0; word < activeWordCount; ++word) {
                        os << "        case " << word << "U: run_active_shard_word_" << word << "(active_bits_); break;\n";
                    }
                    os << "        default: break;\n";
                    os << "        }\n";
                    os << "        active_word_queued_[active_word_] = 0;\n";
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
                emitCoalescedLatchStatements(os, state.latchStmts, "        ");
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

            auto emitReplayShardBody = [&](int shard, std::uint8_t enclosingMask) {
                os << "        sched_" << shard << "();";
                if (state.enableActivityWatermark && shard < static_cast<int>(state.shardDirtyReplaySuccessors.size())) {
                    for (std::uint8_t mask = 1; mask < 4; ++mask) {
                        const auto& succ = state.shardDirtyReplaySuccessors[static_cast<std::size_t>(shard)][mask];
                        if (succ.empty()) {
                            continue;
                        }
                        const bool coveredByEnclosingGuard = mask == enclosingMask;
                        if (!coveredByEnclosingGuard) {
                            os << " if ((replay_mask_ & UINT8_C(" << static_cast<unsigned>(mask)
                               << ")) != UINT8_C(0)) {";
                        }
                        emitShardWordMaskActivation(os, shardWordMasksFor(succ), " ", "");
                        if (!coveredByEnclosingGuard) {
                            os << " }";
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
                        emitReplayShardBody(replayShard, shardMask);
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
            if (state.enablePendingWriteStats) {
                os << "    ++scalar_pending_steps_;\n";
                os << "    scalar_pending_unique_touched_step_ = 0;\n";
            }
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
            auto commitStepEdgeExpr = [&](const std::string &domainKey) {
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
                return std::pair<std::string, bool>{edgeExpr, clockNeedsPreEdgeReplay};
            };
            auto emitReplayOnlyEdgeBatch = [&](const std::vector<std::string> &edgeExprs) {
                if (edgeExprs.empty()) {
                    return;
                }
                os << "    if ((non_clock_inputs_dirty_ || committed_state_dirty_) && !dirty_replayed_) {\n";
                os << "        bool edge_replay_pending_ = false;\n";
                for (const auto &edgeExpr : edgeExprs) {
                    os << "        edge_replay_pending_ = edge_replay_pending_ || (" << edgeExpr << ");\n";
                }
                os << "        if (edge_replay_pending_) {\n";
                if (!state.latchStmts.empty()) {
                    os << "            settle();\n";
                }
                emitPendingReplay("            ", false, true, true);
                os << "        }\n";
                os << "    }\n";
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
                    if (state.enableSharding && state.shardCount() > 0) {
                        os << "    // Direct clock domains commit before derived-clock domains so a same-step\n";
                        os << "    // register update can replay and expose a derived clock edge deterministically.\n";
                    }
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
                            if (state.activityBatchDispatchEnabled) {
                                os << "        activate_all_batches();\n";
                            } else {
                                os << "        activate_all_shards();\n";
                            }
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
                        const auto globalPreIt = sequentialGlobalPreChunkMethods.find(domainKey);
                        if (globalPreIt == sequentialGlobalPreChunkMethods.end()) {
                            continue;
                        }
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
                        if (state.enableSharding && state.shardCount() > 0 && !clockNeedsPreEdgeReplay) {
                            os << "        if ((non_clock_inputs_dirty_ || committed_state_dirty_) && !dirty_replayed_) {\n";
                            if (!state.latchStmts.empty()) {
                                os << "            settle();\n";
                                emitPendingReplay("            ", false, true, true);
                            } else {
                                emitPendingReplay("            ", false, true, true);
                            }
                            os << "        }\n";
                        }
                        os << "        bool domain_pre_committed_ = false;\n";
                        os << "        bool domain_pre_dirty_ = false;\n";
                        for (const auto &methodName : globalPreIt->second) {
                            os << "        " << methodName << "(domain_pre_committed_, domain_pre_dirty_, nullptr, nullptr, nullptr, nullptr, nullptr);\n";
                        }
                        os << "        if (domain_pre_committed_) {\n";
                        os << "            committed_ = true;\n";
                        os << "        }\n";
                        os << "    }\n";
                    }
                    std::vector<std::string> replayOnlyRegEdgeBatch;
                    auto flushReplayOnlyRegEdgeBatch = [&]() {
                        emitReplayOnlyEdgeBatch(replayOnlyRegEdgeBatch);
                        replayOnlyRegEdgeBatch.clear();
                    };
                    const bool hasDerivedRegReplayBarrier =
                        state.enableSharding && state.shardCount() > 0 &&
                        std::any_of(sequentialDomains.begin(), sequentialDomains.end(),
                                    [&](const std::string &domainKey) {
                                        return isDerivedSequentialDomain(domainKey);
                                    });
                    if (hasDerivedRegReplayBarrier) {
                        os << "    auto replay_derived_reg_if_dirty_ = [&]() {\n";
                        os << "        if ((non_clock_inputs_dirty_ || committed_state_dirty_) && !dirty_replayed_) {\n";
                        if (!state.latchStmts.empty()) {
                            os << "            settle();\n";
                            emitPendingReplay("            ", false, true, true);
                        } else {
                            emitPendingReplay("            ", false, true, true);
                        }
                        os << "        }\n";
                        os << "    };\n";
                    }
                    for (const auto &domainKey : sequentialDomains) {
                        const auto [edgeExpr, clockNeedsPreEdgeReplay] = commitStepEdgeExpr(domainKey);
                        const bool domainIsDerived = isDerivedSequentialDomain(domainKey);
                        const auto regChunkIt = sequentialRegChunkMethods.find(domainKey);
                        const bool hasRegBody = regChunkIt != sequentialRegChunkMethods.end();
                        const bool canBatchReplayOnlyEdge =
                            state.enableSharding && state.shardCount() > 0 && !hasRegBody &&
                            !clockNeedsPreEdgeReplay;
                        if (canBatchReplayOnlyEdge) {
                            replayOnlyRegEdgeBatch.push_back(edgeExpr);
                            continue;
                        }
                        flushReplayOnlyRegEdgeBatch();
                        if (state.enableSharding && state.shardCount() > 0 && domainIsDerived) {
                            os << "    replay_derived_reg_if_dirty_();\n";
                        } else if (state.enableSharding && state.shardCount() > 0 &&
                                   clockNeedsPreEdgeReplay && !domainIsDerived) {
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
                        if (state.enableSharding && state.shardCount() > 0 && !clockNeedsPreEdgeReplay) {
                            os << "        if ((non_clock_inputs_dirty_ || committed_state_dirty_) && !dirty_replayed_) {\n";
                            if (!state.latchStmts.empty()) {
                                os << "            settle();\n";
                                emitPendingReplay("            ", false, true, true);
                            } else {
                                emitPendingReplay("            ", false, true, true);
                            }
                            os << "        }\n";
                        }
                        if (regChunkIt != sequentialRegChunkMethods.end()) {
                            os << "        bool domain_reg_committed_ = false;\n";
                            os << "        bool domain_reg_dirty_ = false;\n";
                            const bool domainNeedsNextState = sequentialRegChunkCounts[domainKey] > 1;
                            std::string domainNextArgs = "nullptr, nullptr, nullptr, nullptr, nullptr";
                            if (domainNeedsNextState) {
                                const auto poolsIt = sequentialRegDomainPools.find(domainKey);
                                const SequentialStoragePools pools =
                                    poolsIt != sequentialRegDomainPools.end() ? poolsIt->second
                                                                              : SequentialStoragePools{};
                                std::vector<std::string> args;
                                std::string commitLine = "        if (domain_reg_committed_) {";
                                if (pools.stateU8) {
                                    os << "        prepare_domain_next_stateU8(" << pools.stateU8 << "U);\n";
                                    commitLine += " apply_domain_next_stateU8();";
                                    args.push_back("nullptr");
                                } else {
                                    args.push_back("nullptr");
                                }
                                if (pools.stateU16) {
                                    os << "        prepare_domain_next_stateU16(" << pools.stateU16 << "U);\n";
                                    commitLine += " apply_domain_next_stateU16();";
                                    args.push_back("nullptr");
                                } else {
                                    args.push_back("nullptr");
                                }
                                if (pools.stateU32) {
                                    os << "        prepare_domain_next_stateU32(" << pools.stateU32 << "U);\n";
                                    commitLine += " apply_domain_next_stateU32();";
                                    args.push_back("nullptr");
                                } else {
                                    args.push_back("nullptr");
                                }
                                if (pools.stateU64) {
                                    os << "        prepare_domain_next_stateU64(" << pools.stateU64 << "U);\n";
                                    commitLine += " apply_domain_next_stateU64();";
                                    args.push_back("nullptr");
                                } else {
                                    args.push_back("nullptr");
                                }
                                if (pools.stateVec) {
                                    os << "        prepare_domain_next_stateVec(" << pools.stateVec << "U);\n";
                                    commitLine += " apply_domain_next_stateVec();";
                                    args.push_back("&domain_next_stateVec_scratch_");
                                } else {
                                    args.push_back("nullptr");
                                }
                                domainNextArgs = args[0] + ", " + args[1] + ", " + args[2] + ", " + args[3] + ", " + args[4];
                                commitLine += " }";
                                for (const auto &methodName : regChunkIt->second) {
                                    const bool methodHasRegs = sequentialChunkHasRegs[methodName];
                                    if (methodHasRegs) {
                                        os << "        " << methodName << "(domain_reg_committed_, domain_reg_dirty_, " << domainNextArgs << ");\n";
                                    } else {
                                        os << "        " << methodName << "(domain_reg_committed_, domain_reg_dirty_, nullptr, nullptr, nullptr, nullptr, nullptr);\n";
                                    }
                                }
                                os << commitLine << "\n";
                            } else {
                                for (const auto &methodName : regChunkIt->second) {
                                    os << "        " << methodName << "(domain_reg_committed_, domain_reg_dirty_, nullptr, nullptr, nullptr, nullptr, nullptr);\n";
                                }
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
                    flushReplayOnlyRegEdgeBatch();
                    os << "    if (any_domain_reg_committed_) {\n";
                    if (state.emitsNoDiffGuardedDpicCalls && !state.emitsRuntimeDpicCalls) {
                        os << "#ifndef CONFIG_NO_DIFFTEST\n";
                    }
                    if (state.enableSharding && state.enableActivityWatermark && state.emitsDpicCalls &&
                        state.dpicGlobalWarmupSteps > 0) {
                        os << "        if (gsim_pre_dpic_steps_ < " << state.dpicGlobalWarmupSteps << "U) {\n";
                        if (state.activityBatchDispatchEnabled) {
                            os << "            activate_all_batches();\n";
                        } else {
                            os << "            activate_all_shards();\n";
                        }
                        os << "        } else {\n";
                        if (!state.dpicPreSettleShards.empty()) {
                            if (state.activityBatchStrictDispatch) {
                                os << "            activate_all_batches();\n";
                            } else {
                                os << "            static constexpr std::uint32_t kDpicPreSettleShards[] = {";
                                bool firstShard = true;
                                for (int shard : state.dpicPreSettleShards) {
                                    os << (firstShard ? "" : ", ") << shard << "U";
                                    firstShard = false;
                                }
                                os << "};\n";
                                os << "            activate_shards(kDpicPreSettleShards, " << state.dpicPreSettleShards.size() << "U);\n";
                            }
                        }
                        os << "        }\n";
                    } else if (state.enableSharding && state.enableActivityWatermark && state.emitsDpicCalls &&
                               !state.dpicPreSettleShards.empty()) {
                        if (state.activityBatchStrictDispatch) {
                            os << "        activate_all_batches();\n";
                        } else {
                            os << "        static constexpr std::uint32_t kDpicPreSettleShards[] = {";
                            bool firstShard = true;
                            for (int shard : state.dpicPreSettleShards) {
                                os << (firstShard ? "" : ", ") << shard << "U";
                                firstShard = false;
                            }
                            os << "};\n";
                            os << "        activate_shards(kDpicPreSettleShards, " << state.dpicPreSettleShards.size() << "U);\n";
                        }
                    }
                    os << "        settle();\n";
                    os << "        post_commit_settled_ = true;\n";
                    if (state.emitsNoDiffGuardedDpicCalls && !state.emitsRuntimeDpicCalls) {
                        os << "#endif\n";
                    }
                    os << "    }\n";
                    std::vector<std::string> replayOnlyStmtEdgeBatch;
                    auto flushReplayOnlyStmtEdgeBatch = [&]() {
                        emitReplayOnlyEdgeBatch(replayOnlyStmtEdgeBatch);
                        replayOnlyStmtEdgeBatch.clear();
                    };
                    const bool hasDerivedStmtReplayBarrier =
                        state.enableSharding && state.shardCount() > 0 &&
                        std::any_of(sequentialDomains.begin(), sequentialDomains.end(),
                                    [&](const std::string &domainKey) {
                                        return isDerivedSequentialDomain(domainKey);
                                    });
                    if (hasDerivedStmtReplayBarrier) {
                        os << "    auto replay_derived_stmt_if_dirty_ = [&]() {\n";
                        os << "        if ((non_clock_inputs_dirty_ || committed_state_dirty_) && !dirty_replayed_) {\n";
                        if (!state.latchStmts.empty()) {
                            os << "            settle();\n";
                            emitPendingReplay("            ", false, true, true);
                        } else {
                            emitPendingReplay("            ", false, true, true);
                        }
                        os << "        }\n";
                        os << "    };\n";
                    }
                    for (const auto &domainKey : sequentialDomains) {
                        const auto [edgeExpr, clockNeedsPreEdgeReplay] = commitStepEdgeExpr(domainKey);
                        (void)clockNeedsPreEdgeReplay;
                        const bool domainIsDerived = isDerivedSequentialDomain(domainKey);
                        const auto stmtChunkIt = sequentialStmtChunkMethods.find(domainKey);
                        const auto stmtIt = state.sequentialStmts.find(domainKey);
                        const bool hasStmtBody =
                            stmtChunkIt != sequentialStmtChunkMethods.end() || stmtIt != state.sequentialStmts.end();
                        if (state.enableSharding && state.shardCount() > 0 && !hasStmtBody) {
                            replayOnlyStmtEdgeBatch.push_back(edgeExpr);
                            continue;
                        }
                        flushReplayOnlyStmtEdgeBatch();
                        if (state.enableSharding && state.shardCount() > 0 && domainIsDerived) {
                            os << "    replay_derived_stmt_if_dirty_();\n";
                        }
                        os << "    if (" << edgeExpr << ") {\n";
                        if (state.enableSharding && state.shardCount() > 0 && !domainIsDerived) {
                            os << "        if ((non_clock_inputs_dirty_ || committed_state_dirty_) && !dirty_replayed_) {\n";
                            if (!state.latchStmts.empty()) {
                                os << "            settle();\n";
                                emitPendingReplay("            ", false, true, true);
                            } else {
                                emitPendingReplay("            ", false, true, true);
                            }
                            os << "        }\n";
                        }
                        if (stmtChunkIt != sequentialStmtChunkMethods.end()) {
                            os << "        bool domain_stmt_committed_ = false;\n";
                            os << "        bool domain_stmt_dirty_ = false;\n";
                            for (const auto &methodName : stmtChunkIt->second) {
                                os << "        " << methodName << "(domain_stmt_committed_, domain_stmt_dirty_, nullptr, nullptr, nullptr, nullptr, nullptr);\n";
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
                        } else if (stmtIt != state.sequentialStmts.end()) {
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
                                    if (state.activityBatchDispatchEnabled) {
                                        os << "            activate_all_batches();\n";
                                    } else {
                                        os << "            activate_all_shards();\n";
                                    }
                                }
                                os << "        }\n";
                            }
                        }
                        os << "    }\n";
                    }
                    flushReplayOnlyStmtEdgeBatch();
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
                        if (state.activityBatchDispatchEnabled) {
                            os << "        activate_all_batches();\n";
                        } else {
                            os << "        activate_all_shards();\n";
                        }
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
            if (state.enablePendingWriteStats) {
                os << "    report_pending_write_stats();\n";
            }
            if (state.enableActivityBatchStats) {
                os << "    report_activity_batch_stats();\n";
            }
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
            os << "    metadata.activity_batch_mode = \"" << state.activityBatchMode << "\";\n";
            os << "    metadata.activity_batch_metadata_present = " << (state.activityBatchMetadataPresent ? "true" : "false") << ";\n";
            os << "    metadata.activity_batch_metadata_valid = " << (state.activityBatchMetadataValid ? "true" : "false") << ";\n";
            os << "    metadata.activity_batch_fallback_reason = \"" << state.activityBatchFallbackReason << "\";\n";
            os << "    metadata.activity_batch_count = " << state.activityBatchCount << ";\n";
            os << "    metadata.activity_batch_avg_ops = " << state.activityBatchAvgOps << ";\n";
            os << "    metadata.activity_batch_max_ops = " << state.activityBatchMaxOps << ";\n";
            os << "    metadata.activity_batch_avg_estimated_lines = " << state.activityBatchAvgEstimatedLines << ";\n";
            os << "    metadata.activity_batch_max_estimated_lines = " << state.activityBatchMaxEstimatedLines << ";\n";
            os << "    metadata.activity_batch_max_successor_fanout = " << state.activityBatchMaxSuccessorFanout << ";\n";
            os << "    metadata.activity_batch_successor_edges = " << state.activityBatchSuccessorEdges << ";\n";
            os << "    metadata.activity_batch_entry_count = " << state.activityBatchEntryCount << ";\n";
            os << "    metadata.activity_supernode_active_words = " << state.activitySupernodeActiveWords << ";\n";
            os << "    metadata.activity_supernode_body_count = " << state.activitySupernodeBodyCount << ";\n";
            os << "    metadata.activity_supernode_shard_count = " << state.activitySupernodeShardCount << ";\n";
            os << "    metadata.activity_batch_to_shard_min_span = " << state.activityBatchToShardMinSpan << ";\n";
            os << "    metadata.activity_batch_to_shard_max_span = " << state.activityBatchToShardMaxSpan << ";\n";
            os << "    metadata.activity_batch_dispatch_enabled = " << (state.activityBatchDispatchEnabled ? "true" : "false") << ";\n";
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
            if (metadata.scheduleBatch)
            {
                const auto &batch = *metadata.scheduleBatch;
                os << "    metadata.schedule_batch_names = {";
                for (std::size_t i = 0; i < batch.names.size(); ++i)
                {
                    if (i != 0)
                    {
                        os << ", ";
                    }
                    os << '"' << batch.names[i] << '"';
                }
                os << "};\n";
                os << "    metadata.schedule_batch_class_ids = {" << joinInts(batch.classIds, ", ") << "};\n";
                os << "    metadata.schedule_batch_class_names = {";
                for (std::size_t i = 0; i < batch.classNames.size(); ++i)
                {
                    if (i != 0)
                    {
                        os << ", ";
                    }
                    os << '"' << batch.classNames[i] << '"';
                }
                os << "};\n";
                os << "    metadata.schedule_batch_flags = {" << joinInts(batch.flags, ", ") << "};\n";
                os << "    metadata.schedule_batch_topo_batch_by_pos = {" << joinInts(batch.topoBatchByPos, ", ") << "};\n";
                os << "    metadata.schedule_batch_first_topo_pos = {" << joinInts(batch.firstTopoPos, ", ") << "};\n";
                os << "    metadata.schedule_batch_last_topo_pos = {" << joinInts(batch.lastTopoPos, ", ") << "};\n";
                os << "    metadata.schedule_batch_op_counts = {" << joinInts(batch.opCounts, ", ") << "};\n";
                os << "    metadata.schedule_batch_succ_offsets = {" << joinInts(batch.succOffsets, ", ") << "};\n";
                os << "    metadata.schedule_batch_succ_targets = {" << joinInts(batch.succTargets, ", ") << "};\n";
                os << "    metadata.schedule_batch_entry_batches = {" << joinInts(batch.entryBatches, ", ") << "};\n";
                os << "    metadata.schedule_batch_estimated_lines = {" << joinInts(batch.estimatedLines, ", ") << "};\n";
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
                                        std::string_view internalHeaderFilename,
                                        bool writeRegsToDomainNextState)
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
            os << "void SSimTop::" << chunk.methodName
               << "(bool& committed_, bool& dirty_on_commit_, "
               << "std::vector<std::pair<std::size_t, std::uint8_t>>* next_stateU8_, "
               << "std::vector<std::pair<std::size_t, std::uint16_t>>* next_stateU16_, "
               << "std::vector<std::pair<std::size_t, std::uint32_t>>* next_stateU32_, "
               << "std::vector<std::pair<std::size_t, std::uint64_t>>* next_stateU64_, "
               << "std::vector<std::pair<std::size_t, std::vector<std::uint64_t>>>* next_stateVec_) {\n";
            if (!writeRegsToDomainNextState) {
                os << "    (void)next_stateU8_;\n";
                os << "    (void)next_stateU16_;\n";
                os << "    (void)next_stateU32_;\n";
                os << "    (void)next_stateU64_;\n";
                os << "    (void)next_stateVec_;\n";
            }
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
            struct DomainNextTarget {
                std::string expr;
                std::string pendingWrites;
                std::string index;
                std::string scalarStage;
                std::string scalarMask;
            };
            auto targetStorage = [&](const std::string& regName) {
                std::string expr = state.persistentStorageExpr(regName);
                DomainNextTarget target{expr, "", ""};
                if (writeRegsToDomainNextState) {
                    if (expr.rfind("state_->stateU8[", 0) == 0) {
                        target.scalarStage = "stage_domain_next_stateU8";
                        target.scalarMask = "UINT8_MAX";
                        target.index = expr.substr(std::string("state_->stateU8[").size());
                    } else if (expr.rfind("state_->stateU16[", 0) == 0) {
                        target.scalarStage = "stage_domain_next_stateU16";
                        target.scalarMask = "UINT16_MAX";
                        target.index = expr.substr(std::string("state_->stateU16[").size());
                    } else if (expr.rfind("state_->stateU32[", 0) == 0) {
                        target.scalarStage = "stage_domain_next_stateU32";
                        target.scalarMask = "UINT32_MAX";
                        target.index = expr.substr(std::string("state_->stateU32[").size());
                    } else if (expr.rfind("state_->stateU64[", 0) == 0) {
                        target.scalarStage = "stage_domain_next_stateU64";
                        target.scalarMask = "UINT64_MAX";
                        target.index = expr.substr(std::string("state_->stateU64[").size());
                    } else if (expr.rfind("state_->stateVec[", 0) == 0) {
                        target.pendingWrites = "next_stateVec_";
                        target.index = expr.substr(std::string("state_->stateVec[").size());
                    }
                    if (!target.index.empty() && target.index.back() == ']') {
                        target.index.pop_back();
                    }
                }
                return target;
            };
            auto domainNextWriteStmt = [&](const std::string& regName, const std::string& valueExpr, bool moveValue) {
                const DomainNextTarget target = targetStorage(regName);
                if (!target.scalarStage.empty()) {
                    return target.scalarStage + "(true, static_cast<std::size_t>(" + target.index + "), " +
                           (moveValue ? "std::move(" + valueExpr + ")" : valueExpr) + ", " + target.scalarMask + ")";
                }
                if (target.pendingWrites.empty()) {
                    return target.expr + " = " + (moveValue ? "std::move(" + valueExpr + ")" : valueExpr);
                }
                return target.pendingWrites + "->emplace_back(static_cast<std::size_t>(" + target.index + "), " +
                       (moveValue ? "std::move(" + valueExpr + ")" : valueExpr) + ")";
            };
            auto isWideReg = [&](const std::string& regName, const std::vector<std::string>& regStmts) {
                const auto regWidthIt = state.storageWidths.find(regName);
                return (regWidthIt != state.storageWidths.end() && regWidthIt->second > 64) ||
                       std::any_of(regStmts.begin(), regStmts.end(), [&](const std::string& stmt) {
                           return stmt.find("wolvrix_gsim_mask_merge") != std::string::npos;
                       });
            };
            auto emitActivitySourceTouches = [&](const std::vector<std::string>& sources, std::string_view indent) {
                if (!state.enableSharding || !state.enableActivityWatermark || state.shardCount() <= 0) {
                    return;
                }
                std::set<int> firstShards;
                std::set<int> firstBatches;
                for (const auto& source : sources) {
                    if (state.activityBatchDispatchEnabled) {
                        if (const auto headsIt = state.activitySourceHeadBatches.find(source);
                            headsIt != state.activitySourceHeadBatches.end()) {
                            firstBatches.insert(headsIt->second.begin(), headsIt->second.end());
                        }
                        continue;
                    }
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
                if (state.activityBatchDispatchEnabled) {
                    if (!firstBatches.empty()) {
                        os << indent;
                        emitBatchWordMaskActivation(os, shardWordMasksFor(firstBatches), "", " ");
                    } else {
                        os << indent << "activate_all_batches(); ";
                    }
                    return;
                }
                if (!firstShards.empty()) {
                    os << indent << buildShardActivation(state, shardWordMasksFor(firstShards)) << " ";
                } else {
                    os << indent << "activate_all_shards(); ";
                }
            };
            auto emitStatement = [&](std::string s, bool dirtyOnCommit = false, const std::string* touchSource = nullptr,
                                     bool localOnlyUpdate = false) {
                (void)touchSource;
                if (s.rfind("        ", 0) == 0) s.erase(0, 8);
                if (localOnlyUpdate) {
                    const std::string needle = "committed_ = true;";
                    std::size_t pos = 0;
                    while ((pos = s.find(needle, pos)) != std::string::npos) {
                        s.erase(pos, needle.size());
                    }
                } else {
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
                        std::string replacement = "chunk_updated_ = true; ";
                        replacement += "committed_ = true;";
                        std::size_t pos = 0;
                        while ((pos = s.find(needle, pos)) != std::string::npos) {
                            s.replace(pos, needle.size(), replacement);
                            pos += replacement.size();
                        }
                    }
                }
                const auto assignPos = s.find(" = ");
                if (assignPos != std::string::npos && s.ends_with(";")) {
                    const std::string lhs = s.substr(0, assignPos);
                    const std::string rhs = s.substr(assignPos + 3, s.size() - assignPos - 4);
                    const bool tempVecLhs = lhs.rfind("evalTemps_->tempVec[", 0) == 0;
                    const bool simpleWideRhs = rhs.rfind("evalTemps_->tempVec[", 0) == 0 ||
                                               rhs.rfind("state_->stateVec[", 0) == 0;
                    if (tempVecLhs && simpleWideRhs && rhs.find('(') == std::string::npos) {
                        s = "wolvrix_gsim_assign_bits(" + lhs + ", " + rhs + ");";
                    } else if (tempVecLhs && isWideStorageTernaryExpr(rhs)) {
                        s = "wolvrix_gsim_assign_bits(" + lhs + ", " + rhs + ");";
                    } else if (tempVecLhs && rhs.rfind("std::vector<std::uint64_t>{", 0) == 0) {
                        const auto bracePos = rhs.find('{');
                        s = "wolvrix_gsim_assign_bits(" + lhs + ", " + rhs.substr(bracePos) + ");";
                    }
                }
                os << "    " << s << "\n";
            };
            struct ParsedNextAssignment {
                std::string condition;
                std::string rhs;
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
                const std::string updatedMarker = "; next_" + regName + "_updated_ = true; committed_ = true;";
                const auto updatedPos = s.find(updatedMarker, rhsBegin);
                const auto committedPos = s.find("; committed_ = true;", rhsBegin);
                const auto rhsEnd = updatedPos != std::string::npos
                                        ? updatedPos
                                        : (committedPos != std::string::npos ? committedPos : s.find(";", rhsBegin));
                if (rhsEnd == std::string::npos) {
                    return std::nullopt;
                }
                ParsedNextAssignment parsed;
                parsed.condition = s.substr(4, markerPos - 4);
                parsed.rhs = s.substr(rhsBegin, rhsEnd - rhsBegin);
                return parsed;
            };
            auto canDirectSequentialWrite = [&](const std::string& regName, const std::vector<std::string>& regStmts) {
                if (regStmts.size() != 1) {
                    return false;
                }
                const auto parsed = parseSingleNextAssignment(regName, regStmts.front());
                return parsed.has_value() && parsed->condition.find("state_->") == std::string::npos &&
                       parsed->condition.find("next_" + regName) == std::string::npos &&
                       parsed->rhs.find("state_->") == std::string::npos &&
                       parsed->rhs.find("next_" + regName) == std::string::npos;
            };
            auto canDirectScalarWrite = [&](const std::string& regName, const std::vector<std::string>& regStmts) {
                return canDirectSequentialWrite(regName, regStmts);
            };
            auto wideNeedsPersistentBase = [](const std::vector<std::string>& regStmts) {
                return std::any_of(regStmts.begin(), regStmts.end(), [](const std::string& stmt) {
                    return stmt.find("wolvrix_gsim_mask_merge(") != std::string::npos;
                });
            };
            auto canDirectWideWrite = [&](const std::string& regName, const std::vector<std::string>& regStmts) {
                if (regStmts.size() != 1 || wideNeedsPersistentBase(regStmts)) {
                    return false;
                }
                return canDirectSequentialWrite(regName, regStmts);
            };
            constexpr bool kAllowDelayedDirectSequentialStateWrite = true;
            std::vector<std::pair<std::string, std::string>> delayedDirectWrites;

            for (const auto &regName : chunk.regNames) {
                const auto regStmtIt = chunk.regStmts.find(regName);
                const std::vector<std::string>& regStmts = regStmtIt != chunk.regStmts.end() ? regStmtIt->second : chunk.stmts;
                const bool wideReg = isWideReg(regName, regStmts);
                const bool directWideWrite = kAllowDelayedDirectSequentialStateWrite && writeRegsToDomainNextState && wideReg &&
                                             canDirectWideWrite(regName, regStmts);
                const bool directScalarWrite = kAllowDelayedDirectSequentialStateWrite && writeRegsToDomainNextState && !wideReg &&
                                               canDirectScalarWrite(regName, regStmts);
                if (directWideWrite || directScalarWrite) {
                    continue;
                }
                if (wideReg) {
                    os << "    std::vector<std::uint64_t> next_" << regName;
                    if (wideNeedsPersistentBase(regStmts)) {
                        os << " = " << state.persistentStorageExpr(regName);
                    }
                    os << ";\n";
                    os << "    bool next_" << regName << "_updated_ = false;\n";
                } else {
                    os << "    auto next_" << regName << " = " << state.persistentStorageExpr(regName) << ";\n";
                    os << "    bool next_" << regName << "_updated_ = false;\n";
                }
            }
            for (const auto &regName : chunk.regNames) {
                const auto regStmtIt = chunk.regStmts.find(regName);
                if (regStmtIt == chunk.regStmts.end()) {
                    continue;
                }
                const bool wideReg = isWideReg(regName, regStmtIt->second);
                const auto parsed = regStmtIt->second.size() == 1 ? parseSingleNextAssignment(regName, regStmtIt->second.front()) : std::optional<ParsedNextAssignment>{};
                const bool directWideWrite = kAllowDelayedDirectSequentialStateWrite && writeRegsToDomainNextState && wideReg &&
                                             canDirectWideWrite(regName, regStmtIt->second);
                const bool directScalarWrite = kAllowDelayedDirectSequentialStateWrite && writeRegsToDomainNextState && !wideReg &&
                                                canDirectScalarWrite(regName, regStmtIt->second);
                if (directWideWrite) {
                    const DomainNextTarget target = targetStorage(regName);
                    if (!target.pendingWrites.empty()) {
                        if (isSimpleWideLvalue(parsed->rhs)) {
                            delayedDirectWrites.emplace_back(
                                regName,
                                "if (" + parsed->condition + ") { if (" + target.expr + " != " + parsed->rhs +
                                ") { " + domainNextWriteStmt(regName, parsed->rhs, false) +
                                "; committed_ = true; } }");
                        } else {
                            const std::string tempName = "delayed_direct_" + regName;
                            delayedDirectWrites.emplace_back(
                                regName,
                                "if (" + parsed->condition + ") { auto " + tempName + " = " + parsed->rhs +
                                "; if (" + target.expr + " != " + tempName + ") { " +
                                domainNextWriteStmt(regName, tempName, true) +
                                "; committed_ = true; } }");
                        }
                    } else if (isSimpleWideLvalue(parsed->rhs)) {
                        delayedDirectWrites.emplace_back(
                            regName,
                            "if (" + parsed->condition + ") { if (" + target.expr + " != " + parsed->rhs +
                            ") { wolvrix_gsim_assign_bits(" + target.expr + ", " + parsed->rhs +
                            "); committed_ = true; } }");
                    } else {
                        const std::string tempName = "delayed_direct_" + regName;
                        delayedDirectWrites.emplace_back(
                            regName,
                            "if (" + parsed->condition + ") { auto " + tempName + " = " + parsed->rhs +
                            "; if (" + target.expr + " != " + tempName + ") { " + target.expr +
                            " = std::move(" + tempName +
                            "); committed_ = true; } }");
                    }
                    continue;
                }
                if (directScalarWrite) {
                    const DomainNextTarget target = targetStorage(regName);
                    const std::string tempName = "delayed_direct_" + regName;
                    delayedDirectWrites.emplace_back(
                        regName,
                        "if (" + parsed->condition + ") { const auto " + tempName + " = " + parsed->rhs +
                        "; if (" + domainNextWriteStmt(regName, tempName, false) +
                        ") { committed_ = true; } }");
                    continue;
                }
                const DomainNextTarget localTarget = targetStorage(regName);
                const bool scalarDomainNextLocal = writeRegsToDomainNextState && !wideReg && !localTarget.scalarStage.empty();
                for (auto stmt : regStmtIt->second) {
                    emitStatement(stmt, false, &regName, scalarDomainNextLocal);
                }
            }
            for (std::size_t stmtIndex = 0; stmtIndex < chunk.stmts.size(); ++stmtIndex) {
                emitStatement(chunk.stmts[stmtIndex],
                              tracksStatementDirty && chunk.stmtDirtyOnCommit[stmtIndex]);
            }
            for (const auto& [regName, stmt] : delayedDirectWrites) {
                emitStatement(stmt, false, &regName);
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
                os << "    {\n";
                for (const auto &regName : chunk.regNames) {
                    const auto regStmtIt = chunk.regStmts.find(regName);
                    const std::vector<std::string>& regStmts = regStmtIt != chunk.regStmts.end() ? regStmtIt->second : chunk.stmts;
                    const bool wideReg = isWideReg(regName, regStmts);
                    const bool directWideWrite = kAllowDelayedDirectSequentialStateWrite && writeRegsToDomainNextState && wideReg &&
                                                 canDirectWideWrite(regName, regStmts);
                    const bool directScalarWrite = kAllowDelayedDirectSequentialStateWrite && writeRegsToDomainNextState && !wideReg &&
                                                   canDirectScalarWrite(regName, regStmts);
                    if (directWideWrite || directScalarWrite) {
                        continue;
                    }
                    const DomainNextTarget target = targetStorage(regName);
                    if (wideReg) {
                        os << "        if (next_" << regName << "_updated_) { "
                           << domainNextWriteStmt(regName, "next_" + regName, true) << "; }\n";
                    } else if (!target.scalarStage.empty()) {
                        os << "        if (next_" << regName << "_updated_) { if ("
                           << domainNextWriteStmt(regName, "next_" + regName, false)
                           << ") { chunk_updated_ = true; committed_ = true; } }\n";
                    } else {
                        os << "        if (next_" << regName << "_updated_) { "
                           << domainNextWriteStmt(regName, "next_" + regName, false) << "; }\n";
                    }
                }
                os << "    }\n";
                if (state.enableSharding && state.enableActivityWatermark && state.shardCount() > 0) {
                    os << "    if (chunk_updated_) { ";
                    emitActivitySourceTouches(chunk.regNames, "");
                    os << "}\n";
                }
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
        state.activityBoundaryShardSoftBytes =
            parseNonNegativeIntAttr(options, "activity_boundary_shard_soft_bytes",
                                    state.activityBoundaryShardSoftBytes);
        state.changedValueFanoutInlineMaskLimit =
            parseNonNegativeIntAttr(options, "changed_value_fanout_inline_mask_limit",
                                    state.changedValueFanoutInlineMaskLimit);
        state.shardActivationInlineMaskLimit =
            parseNonNegativeIntAttr(options, "shard_activation_inline_mask_limit",
                                    state.shardActivationInlineMaskLimit);
        state.enableDpicTrace = attrEnabled(options, "dpic_trace", false);
        state.enableXsZeroRetireTrace =
            state.enableDpicTrace && attrEnabled(options, "xs_zero_retire_trace", false);
        state.dpicGlobalWarmupSteps = parsePositiveIntAttr(options, "dpic_global_warmup_steps", 0);
        state.enablePendingWriteStats = attrEnabled(options, "pending_write_stats", false);
        state.activityBatchMode = parseActivityBatchMode(options);
        if (state.activityBatchMode == "invalid")
        {
            reportError("invalid gsim activity batch mode",
                        "activity_batch_mode must be one of legacy, stats, dispatch, strict_dispatch");
            result.success = false;
            return result;
        }
        state.enableActivityBatchStats = attrEnabled(options, "activity_batch_stats", false) || state.activityBatchMode == "stats" ||
                                         state.activityBatchMode == "dispatch" || state.activityBatchMode == "strict_dispatch";
        state.activityBatchMetadataPresent = metadata->scheduleBatch.has_value();
        state.activityBatchMetadataValid = metadata->scheduleBatch.has_value();
        state.activityBatchDispatchEnabled = metadata->scheduleBatch.has_value() &&
                                             (state.activityBatchMode == "dispatch" || state.activityBatchMode == "strict_dispatch");
        state.activityBatchStrictDispatch = state.activityBatchMode == "strict_dispatch";
        if (state.activityBatchDispatchEnabled) {
            state.enableActivityWatermark = true;
        }
        if (!metadata->scheduleBatch && (state.activityBatchMode == "dispatch" || state.activityBatchMode == "strict_dispatch"))
        {
            reportError("missing required gsim activity batch metadata",
                        target->namespacePath + ".schedule.batch.* is required by activity_batch_mode=" + state.activityBatchMode);
            result.success = false;
            return result;
        }
        state.activityBatchFallbackReason = metadata->scheduleBatch ? "none" : "missing";
        if (auto targetsAttr = attrValue(options, "dpic_trace_targets"))
        {
            for (auto &targetName : splitCsv(*targetsAttr)) {
                state.dpicTraceTargets.insert(std::move(targetName));
            }
        }
        if (state.enableXsZeroRetireTrace && state.dpicTraceTargets.empty())
        {
            state.dpicTraceTargets.insert("v_difftest_InstrCommit");
        }
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
        state.enableSharding = metadata->opCount > 128 || state.activityBatchDispatchEnabled;
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
        auto recordSequentialClockInput = [&](std::string_view clockSymbol) {
            sequentialClockInputs.insert(resolveSequentialClockStateName(clockSymbol, state.inputPorts));
        };
        for (const auto& opId : target->graph->operations()) {
            const auto op = target->graph->getOperation(opId);
            const auto operands = op.operands();
            switch (op.kind()) {
                case wolvrix::lib::grh::OperationKind::kMemoryWritePort: {
                    if (operands.size() < 5) {
                        break;
                    }
                    const auto eventValue = target->graph->getValue(operands[4]);
                    recordSequentialClockInput(eventValue.symbolText().empty()
                                                   ? std::string_view("clock")
                                                   : eventValue.symbolText());
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kRegisterWritePort: {
                    std::string clockSymbol = "clock";
                    if (operands.size() > 3) {
                        const auto eventValue = target->graph->getValue(operands[3]);
                        if (!eventValue.symbolText().empty()) {
                            clockSymbol = std::string(eventValue.symbolText());
                        }
                    }
                    if (auto clockSymAttr = op.attr("clockSymbol")) {
                        if (auto *sym = std::get_if<std::string>(&*clockSymAttr); sym != nullptr && !sym->empty()) {
                            clockSymbol = *sym;
                        }
                    }
                    recordSequentialClockInput(clockSymbol);
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kDpicCall: {
                    auto eventEdgeAttr = op.attr("eventEdge");
                    const auto *eventEdges =
                        eventEdgeAttr ? std::get_if<std::vector<std::string>>(&*eventEdgeAttr) : nullptr;
                    const std::size_t eventCount =
                        eventEdges != nullptr && !eventEdges->empty() ? eventEdges->size() : 1U;
                    if (operands.size() < 1U + eventCount) {
                        break;
                    }
                    const std::size_t eventStart = operands.size() - eventCount;
                    const auto eventValue = target->graph->getValue(operands[eventStart]);
                    recordSequentialClockInput(eventValue.symbolText().empty()
                                                   ? std::string_view("clock")
                                                   : eventValue.symbolText());
                    break;
                }
                default:
                    break;
            }
        }
        std::set<std::string> sequentialDomains;
        for (const auto &domain : state.sequentialGlobalPreStmts) {
            sequentialDomains.insert(domain.first);
        }
        for (const auto &domain : state.sequentialPreStmts) {
            sequentialDomains.insert(domain.first);
        }
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
            const std::uint8_t replayMask = sequentialClockInputs.count(sanitizeIdentifier(port.name)) > 0
                ? kDirtyReplayClock
                : kDirtyReplayNonClock;
            state.dirtyReplayRootMasks[port.value] = replayMask;
            state.valueDirtyReplayMasks[port.value] = replayMask;
            if (state.enableActivityWatermark || state.activityBatchDispatchEnabled) {
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
        if (state.activityBatchDispatchEnabled && metadata->scheduleBatch) {
            state.opBatchOrdinalByIndex.assign(opIdByIndex.size(), -1);
            const auto& batchTopoByPos = metadata->scheduleBatch->topoBatchByPos;
            for (std::size_t pos = 0; pos < metadata->topoOrder.size() && pos < batchTopoByPos.size(); ++pos) {
                const auto opIndex = metadata->topoOrder[pos];
                const auto batchOrdinal = batchTopoByPos[pos];
                if (opIndex >= 0 && static_cast<std::size_t>(opIndex) < state.opBatchOrdinalByIndex.size() &&
                    batchOrdinal >= 0) {
                    state.opBatchOrdinalByIndex[static_cast<std::size_t>(opIndex)] = static_cast<std::int32_t>(batchOrdinal);
                }
            }
        }
        if (state.enableSharding && state.enableActivityWatermark &&
            state.activityBoundaryShardSoftBytes > 0) {
            state.opActivityOrdinalByIndex.assign(opIdByIndex.size(), -1);
            state.activityClassBitsByOrdinal.assign(metadata->scheduleActivityOrder.size(), 0);
            std::unordered_map<std::string, std::uint8_t> classBits;
            std::uint8_t nextClassBit = 1;
            for (std::size_t ordinal = 0; ordinal < metadata->scheduleActivityOrder.size(); ++ordinal) {
                const auto& activityName = metadata->scheduleActivityOrder[ordinal];
                const auto classIt = metadata->scheduleActivityClasses.find(activityName);
                const std::string className =
                    classIt != metadata->scheduleActivityClasses.end() ? classIt->second : std::string();
                auto bitIt = classBits.find(className);
                if (bitIt == classBits.end()) {
                    const std::uint8_t bit = nextClassBit != 0 ? nextClassBit : std::uint8_t{0x80};
                    bitIt = classBits.emplace(className, bit).first;
                    if (nextClassBit != 0 && nextClassBit < 0x80U) {
                        nextClassBit = static_cast<std::uint8_t>(nextClassBit << 1U);
                    } else {
                        nextClassBit = 0;
                    }
                }
                state.activityClassBitsByOrdinal[ordinal] = bitIt->second;
                const auto membersIt = metadata->scheduleActivityMembers.find(activityName);
                if (membersIt == metadata->scheduleActivityMembers.end()) {
                    continue;
                }
                for (const auto opIndex : membersIt->second) {
                    if (opIndex >= 0 && static_cast<std::size_t>(opIndex) < state.opActivityOrdinalByIndex.size()) {
                        state.opActivityOrdinalByIndex[static_cast<std::size_t>(opIndex)] =
                            static_cast<std::int32_t>(ordinal);
                    }
                }
            }
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
        finalizeChangeTrackedFanout(state);

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
        precomputeShardSuccessorActivationRanges(state);
        precomputeSequentialChunkActivityActivationRanges(state, sequentialChunks);
        if (const auto &batch = metadata->scheduleBatch)
        {
            state.activityBatchCount = batch->count;
            state.activityBatchEntryCount = static_cast<int64_t>(batch->entryBatches.size());
            state.activitySupernodeActiveWords = (batch->count + 63) / 64;
            state.activitySupernodeBodyCount = state.activityBatchDispatchEnabled ? batch->count : 0;
            state.activitySupernodeShardCount = static_cast<int64_t>(state.shardCount());
            state.activityBatchShardSpans.assign(static_cast<std::size_t>(batch->count), {-1, -1});
            for (const auto count : batch->opCounts)
            {
                state.activityBatchAvgOps += count;
                state.activityBatchMaxOps = std::max(state.activityBatchMaxOps, count);
            }
            if (batch->count > 0)
            {
                state.activityBatchAvgOps /= batch->count;
            }
            for (const auto estimatedLines : batch->estimatedLines)
            {
                state.activityBatchAvgEstimatedLines += estimatedLines;
                state.activityBatchMaxEstimatedLines = std::max(state.activityBatchMaxEstimatedLines, estimatedLines);
            }
            if (batch->count > 0)
            {
                state.activityBatchAvgEstimatedLines /= batch->count;
            }
            state.activityBatchSuccessorEdges = static_cast<int64_t>(batch->succTargets.size());
            for (std::size_t batchIndex = 0; batchIndex + 1U < batch->succOffsets.size(); ++batchIndex)
            {
                state.activityBatchMaxSuccessorFanout =
                    std::max(state.activityBatchMaxSuccessorFanout,
                             batch->succOffsets[batchIndex + 1U] - batch->succOffsets[batchIndex]);
            }
            if (state.enableSharding && !batch->topoBatchByPos.empty() && !state.opProducerFirstShardByIndex.empty())
            {
                std::vector<int64_t> firstShard(static_cast<std::size_t>(batch->count), std::numeric_limits<int64_t>::max());
                std::vector<int64_t> lastShard(static_cast<std::size_t>(batch->count), -1);
                for (std::size_t pos = 0; pos < metadata->topoOrder.size() && pos < batch->topoBatchByPos.size(); ++pos)
                {
                    const auto batchOrdinal = batch->topoBatchByPos[pos];
                    const auto opIndex = metadata->topoOrder[pos];
                    if (batchOrdinal < 0 || static_cast<std::size_t>(batchOrdinal) >= firstShard.size() ||
                        opIndex < 0 || static_cast<std::size_t>(opIndex) >= state.opProducerFirstShardByIndex.size())
                    {
                        continue;
                    }
                    const auto opFirst = state.opProducerFirstShardByIndex[static_cast<std::size_t>(opIndex)];
                    const auto opLast = state.opProducerLastShardByIndex[static_cast<std::size_t>(opIndex)];
                    if (opFirst < 0 || opLast < opFirst)
                    {
                        continue;
                    }
                    auto index = static_cast<std::size_t>(batchOrdinal);
                    firstShard[index] = std::min(firstShard[index], static_cast<int64_t>(opFirst));
                    lastShard[index] = std::max(lastShard[index], static_cast<int64_t>(opLast));
                }
                state.activityBatchToShardMinSpan = std::numeric_limits<int64_t>::max();
                state.activityBatchToShardMaxSpan = 0;
                bool sawSpan = false;
                for (std::size_t index = 0; index < firstShard.size(); ++index)
                {
                    if (firstShard[index] == std::numeric_limits<int64_t>::max() || lastShard[index] < firstShard[index])
                    {
                        continue;
                    }
                    const int64_t span = lastShard[index] - firstShard[index] + 1;
                    state.activityBatchShardSpans[index] = {firstShard[index], lastShard[index]};
                    state.activityBatchToShardMinSpan = std::min(state.activityBatchToShardMinSpan, span);
                    state.activityBatchToShardMaxSpan = std::max(state.activityBatchToShardMaxSpan, span);
                    sawSpan = true;
                }
                if (!sawSpan)
                {
                    state.activityBatchToShardMinSpan = 0;
                }
            }
        }

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

        std::map<std::string, std::size_t> sequentialRegChunkCounts;
        for (const auto& chunk : sequentialChunks) {
            if (!chunk.regNames.empty()) {
                ++sequentialRegChunkCounts[chunk.domainKey];
            }
        }
        for (const auto &chunk : sequentialChunks) {
            std::string chunkFileName = baseName + "_" + chunk.methodName + ".cpp";
            std::filesystem::path chunkPath = outputDir / chunkFileName;
            auto chunkFile = openOutputFile(chunkPath);
            if (!chunkFile) {
                result.success = false;
                return result;
            }
            const bool writeRegsToDomainNextState = !chunk.regNames.empty() &&
                sequentialRegChunkCounts[chunk.domainKey] > 1;
            writeSequentialChunkSource(*chunkFile, state, chunk, internalHeaderPath.filename().string(),
                                       writeRegsToDomainNextState);
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
