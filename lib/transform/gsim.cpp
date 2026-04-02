#include "transform/gsim.hpp"

#include "core/grh.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <queue>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wolvrix::lib::transform
{

    namespace
    {
        struct OpRecord
        {
            wolvrix::lib::grh::OperationId id;
            std::string symbol;
            std::string kind;
            std::vector<wolvrix::lib::grh::OperationId> preds;
            std::vector<wolvrix::lib::grh::OperationId> succs;
        };

        struct GsimMetadata
        {
            std::vector<int64_t> roots;
            std::vector<int64_t> topo;
            std::map<std::string, std::vector<int64_t>> groups;
            std::vector<std::string> groupNames;
            std::vector<std::string> scheduleActivityOrder;
            std::map<std::string, std::vector<int64_t>> scheduleActivityMembers;
            std::map<std::string, std::string> scheduleActivityClasses;
            std::vector<std::string> hypergraphNodeNames;
            std::map<std::string, std::vector<int64_t>> hypergraphNodeMembers;
            std::vector<std::string> hypergraphEdgeNames;
            std::map<std::string, std::string> hypergraphEdgeSources;
            std::map<std::string, std::string> hypergraphEdgeTargets;
            std::map<std::string, std::vector<int64_t>> hypergraphEdgeSinks;
            std::map<int64_t, std::string> classifications;
            std::map<int64_t, std::vector<int64_t>> predecessors;
            std::map<int64_t, std::vector<int64_t>> successors;
            std::vector<std::string> opDescriptors;
            int64_t opCount = 0;
        };

        struct TopologyStats
        {
            int64_t initialReadySize = 0;
            int64_t maxReadySize = 0;
            int64_t processedCount = 0;
        };

        bool isEventRootKind(wolvrix::lib::grh::OperationKind kind)
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
            case wolvrix::lib::grh::OperationKind::kLatchWritePort:
            case wolvrix::lib::grh::OperationKind::kMemoryWritePort:
            case wolvrix::lib::grh::OperationKind::kMemoryReadPort:
            case wolvrix::lib::grh::OperationKind::kSystemTask:
            case wolvrix::lib::grh::OperationKind::kDpicCall:
            case wolvrix::lib::grh::OperationKind::kDpicImport:
                return true;
            default:
                return false;
            }
        }

        std::string classifyOp(wolvrix::lib::grh::OperationKind kind)
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kSystemTask:
                return "system-task";
            case wolvrix::lib::grh::OperationKind::kSystemFunction:
                return "system-function";
            case wolvrix::lib::grh::OperationKind::kDpicImport:
                return "dpi-import";
            case wolvrix::lib::grh::OperationKind::kDpicCall:
                return "dpi-call";
            case wolvrix::lib::grh::OperationKind::kRegister:
            case wolvrix::lib::grh::OperationKind::kRegisterReadPort:
            case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
            case wolvrix::lib::grh::OperationKind::kLatch:
            case wolvrix::lib::grh::OperationKind::kLatchReadPort:
            case wolvrix::lib::grh::OperationKind::kLatchWritePort:
            case wolvrix::lib::grh::OperationKind::kMemory:
            case wolvrix::lib::grh::OperationKind::kMemoryReadPort:
            case wolvrix::lib::grh::OperationKind::kMemoryWritePort:
                return "stateful";
            default:
                return "logic";
            }
        }

        std::string eventGroupKey(const wolvrix::lib::grh::Operation &op)
        {
            switch (op.kind())
            {
            case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
            {
                auto attr = op.attr("clockSymbol");
                if (const auto *clock = attr ? std::get_if<std::string>(&*attr) : nullptr)
                {
                    return "reg:" + *clock;
                }
                return "reg:unknown";
            }
            case wolvrix::lib::grh::OperationKind::kLatchWritePort:
            {
                auto attr = op.attr("enableSymbol");
                if (const auto *enable = attr ? std::get_if<std::string>(&*attr) : nullptr)
                {
                    return "latch:" + *enable;
                }
                return "latch:unknown";
            }
            case wolvrix::lib::grh::OperationKind::kMemoryReadPort:
            case wolvrix::lib::grh::OperationKind::kMemoryWritePort:
            {
                auto attr = op.attr("clockSymbol");
                if (const auto *clock = attr ? std::get_if<std::string>(&*attr) : nullptr)
                {
                    return "mem:" + *clock;
                }
                return "mem:unknown";
            }
            case wolvrix::lib::grh::OperationKind::kSystemTask:
                return "system-task";
            case wolvrix::lib::grh::OperationKind::kDpicCall:
                return "dpi-call";
            case wolvrix::lib::grh::OperationKind::kDpicImport:
                return "dpi-import";
            default:
                return "combinational";
            }
        }

        void emitRealtimeStageLog(std::string_view graphSymbol, std::string_view message)
        {
            std::cerr << "[gsim] graph=" << graphSymbol << ' ' << message << '\n';
            std::cerr.flush();
        }

        std::vector<OpRecord> collectRecords(const wolvrix::lib::grh::Graph &graph)
        {
            std::cerr << "[collect] pass 1: counting ops...\n";
            std::vector<OpRecord> records;
            records.reserve(100000); // Will grow as needed

            // First pass: collect all valid operations
            size_t opCount = 0;
            for (const auto opId : graph.operations())
            {
                if (!opId.valid()) continue;
                ++opCount;
            }
            std::cerr << "[collect] total ops: " << opCount << "\n";

            records.reserve(opCount);
            for (const auto opId : graph.operations())
            {
                if (!opId.valid()) continue;
                const auto op = graph.getOperation(opId);
                // Store index only, convert to string later when needed
                records.push_back(OpRecord{opId, "", "", {}, {}});
            }

            std::cerr << "[collect] pass 2: building index map...\n";
            // Build index map using vector (since indices are dense after sort)
            std::unordered_map<uint32_t, std::size_t> indexByOp;
            indexByOp.reserve(records.size() * 2);
            for (std::size_t i = 0; i < records.size(); ++i)
            {
                indexByOp.emplace(records[i].id.index, i);
            }

            std::cerr << "[collect] pass 3: collecting preds...\n";
            // Use versioned seen array to avoid O(n) clear per op
            std::vector<uint32_t> seenVersion(records.size(), 0);
            uint32_t currentVersion = 1;

            // Second pass: collect predecessors for each op
            size_t processed = 0;
            for (auto &record : records)
            {
                const auto op = graph.getOperation(record.id);

                for (const auto operand : op.operands())
                {
                    const auto def = graph.getValue(operand).definingOp();
                    if (!def.valid() || def == record.id) continue;
                    auto it = indexByOp.find(def.index);
                    if (it != indexByOp.end())
                    {
                        std::size_t idx = it->second;
                        if (seenVersion[idx] != currentVersion)
                        {
                            seenVersion[idx] = currentVersion;
                            record.preds.push_back(def);
                        }
                    }
                }

                ++currentVersion;
                if (currentVersion == 0)
                {
                    std::fill(seenVersion.begin(), seenVersion.end(), 0);
                    currentVersion = 1;
                }

                ++processed;
                if ((processed & 0x3FFFF) == 0) // Every 256K ops
                {
                    std::cerr << "[collect] processed " << processed << "/" << records.size() << "\n";
                }
            }

            std::cerr << "[collect] pass 4: building succs...\n";
            // Build successors
            for (const auto &record : records)
            {
                for (const auto pred : record.preds)
                {
                    auto it = indexByOp.find(pred.index);
                    if (it == indexByOp.end()) continue;
                    records[it->second].succs.push_back(record.id);
                }
            }

            std::cerr << "[collect] pass 5: filling symbols...\n";
            // Fill symbols only at the end
            for (auto &record : records)
            {
                const auto op = graph.getOperation(record.id);
                record.symbol = std::string(op.symbolText());
                record.kind = std::string(wolvrix::lib::grh::toString(op.kind()));
            }

            std::cerr << "[collect] done\n";
            return records;
        }

        std::vector<wolvrix::lib::grh::OperationId> stableTopologicalOrder(const std::vector<OpRecord> &records,
                                                                            TopologyStats *stats = nullptr)
        {
            if (records.empty())
            {
                return {};
            }

            // Find max index to size vectors
            uint32_t maxIndex = 0;
            for (const auto &record : records)
            {
                maxIndex = std::max(maxIndex, record.id.index);
            }

            std::cerr << "[topo-debug] records=" << records.size() << " maxIndex=" << maxIndex
                      << " vector_mem=" << ((maxIndex + 1) * (sizeof(uint32_t) + sizeof(bool) + sizeof(void*))) / (1024*1024) << "MB\n";

            // Use vectors instead of unordered_map for O(1) indexing
            std::vector<uint32_t> indegree(maxIndex + 1, 0);
            std::vector<std::vector<wolvrix::lib::grh::OperationId>> succs(maxIndex + 1);
            std::vector<bool> hasRecord(maxIndex + 1, false);

            std::cerr << "[topo-debug] vectors allocated\n";

            for (const auto &record : records)
            {
                indegree[record.id.index] = static_cast<uint32_t>(record.preds.size());
                succs[record.id.index] = record.succs;
                hasRecord[record.id.index] = true;
            }

            std::cerr << "[topo-debug] data copied\n";

            // Collect initial ready ops and sort them once
            std::vector<uint32_t> currentLevel;
            currentLevel.reserve(records.size());

            for (const auto &record : records)
            {
                if (record.preds.empty())
                {
                    currentLevel.push_back(record.id.index);
                }
            }

            // Sort once to maintain stable ordering
            std::sort(currentLevel.begin(), currentLevel.end());

            if (stats)
            {
                stats->initialReadySize = static_cast<int64_t>(currentLevel.size());
                stats->maxReadySize = static_cast<int64_t>(currentLevel.size());
                stats->processedCount = 0;
            }

            std::vector<wolvrix::lib::grh::OperationId> order;
            order.reserve(records.size());

            std::vector<uint32_t> nextLevel;
            nextLevel.reserve(1024);

            size_t currentPos = 0;
            uint32_t currentMaxReady = static_cast<uint32_t>(currentLevel.size());

            while (currentPos < currentLevel.size())
            {
                // Get next from current level (already sorted)
                const uint32_t nextIdx = currentLevel[currentPos++];
                order.push_back(wolvrix::lib::grh::OperationId{nextIdx});

                // Process successors
                for (const auto succ : succs[nextIdx])
                {
                    if (!hasRecord[succ.index]) continue;
                    if (indegree[succ.index] == 0) continue;

                    --indegree[succ.index];
                    if (indegree[succ.index] == 0)
                    {
                        nextLevel.push_back(succ.index);
                    }
                }

                // When current level is exhausted, sort nextLevel and swap
                if (currentPos >= currentLevel.size() && !nextLevel.empty())
                {
                    std::sort(nextLevel.begin(), nextLevel.end());
                    currentLevel.swap(nextLevel);
                    nextLevel.clear();
                    currentPos = 0;

                    if (stats)
                    {
                        currentMaxReady = std::max(currentMaxReady, static_cast<uint32_t>(currentLevel.size()));
                    }
                }
            }

            if (stats)
            {
                stats->maxReadySize = static_cast<int64_t>(currentMaxReady);
                stats->processedCount = static_cast<int64_t>(order.size());
            }

            if (order.size() != records.size())
            {
                order.clear();
                for (const auto &record : records)
                {
                    order.push_back(record.id);
                }
                if (stats)
                {
                    stats->processedCount = static_cast<int64_t>(order.size());
                }
            }
            return order;
        }

        GsimMetadata buildMetadata(const wolvrix::lib::grh::Graph &graph,
                                   const std::vector<OpRecord> &records,
                                   const std::vector<wolvrix::lib::grh::OperationId> &topoOrder)
        {
            GsimMetadata metadata;
            metadata.opCount = static_cast<int64_t>(records.size());

            std::unordered_map<uint32_t, std::string> eventGroupByOp;
            std::unordered_map<std::string, std::vector<int64_t>> consumerIdsByGroup;
            std::unordered_map<std::string, std::unordered_set<int64_t>> consumerSetByGroup;

            for (const auto &record : records)
            {
                const auto op = graph.getOperation(record.id);
                eventGroupByOp.emplace(record.id.index, eventGroupKey(op));
            }

            for (const auto &record : records)
            {
                const auto op = graph.getOperation(record.id);
                const auto className = classifyOp(op.kind());
                const int64_t opIndex = static_cast<int64_t>(record.id.index);
                const std::string groupKey = eventGroupByOp.at(record.id.index);
                const std::string activityName = "activity." + groupKey;
                const std::string hyperNodeName = "node." + groupKey;
                metadata.classifications.emplace(opIndex, className);

                if (record.preds.empty() || isEventRootKind(op.kind()))
                {
                    metadata.roots.push_back(opIndex);
                }

                metadata.groups[groupKey].push_back(opIndex);
                metadata.scheduleActivityMembers[activityName].push_back(opIndex);
                metadata.hypergraphNodeMembers[hyperNodeName].push_back(opIndex);

                std::vector<int64_t> predIds;
                for (const auto pred : record.preds)
                {
                    predIds.push_back(static_cast<int64_t>(pred.index));
                }
                metadata.predecessors.emplace(opIndex, std::move(predIds));

                std::vector<int64_t> succIds;
                for (const auto succ : record.succs)
                {
                    succIds.push_back(static_cast<int64_t>(succ.index));
                    const std::string &succGroup = eventGroupByOp.at(succ.index);
                    if (succGroup != groupKey)
                    {
                        auto &seenConsumers = consumerSetByGroup[groupKey];
                        if (seenConsumers.insert(static_cast<int64_t>(succ.index)).second)
                        {
                            consumerIdsByGroup[groupKey].push_back(static_cast<int64_t>(succ.index));
                        }
                    }
                }
                metadata.successors.emplace(opIndex, std::move(succIds));

                metadata.opDescriptors.push_back(std::to_string(record.id.index) + ":" + record.kind + ":" + className + ":" + record.symbol);
            }

            std::sort(metadata.roots.begin(), metadata.roots.end());
            metadata.roots.erase(std::unique(metadata.roots.begin(), metadata.roots.end()), metadata.roots.end());
            for (const auto opId : topoOrder)
            {
                metadata.topo.push_back(static_cast<int64_t>(opId.index));
            }

            for (const auto &[name, ids] : metadata.groups)
            {
                (void)ids;
                metadata.groupNames.push_back(name);
            }

            for (const auto &[groupKey, ids] : metadata.groups)
            {
                (void)ids;
                const std::string activityName = "activity." + groupKey;
                const std::string hyperNodeName = "node." + groupKey;
                metadata.scheduleActivityOrder.push_back(activityName);
                metadata.hypergraphNodeNames.push_back(hyperNodeName);

                std::string activityClass = "combinational";
                if (groupKey.rfind("reg:", 0) == 0 || groupKey.rfind("latch:", 0) == 0 || groupKey.rfind("mem:", 0) == 0)
                {
                    activityClass = "stateful";
                }
                else if (groupKey == "system-task" || groupKey == "dpi-call" || groupKey == "dpi-import")
                {
                    activityClass = "side-effect";
                }
                metadata.scheduleActivityClasses.emplace(activityName, activityClass);

                std::string edgeName = "edge." + groupKey;
                metadata.hypergraphEdgeNames.push_back(edgeName);
                metadata.hypergraphEdgeSources.emplace(edgeName, hyperNodeName);
                metadata.hypergraphEdgeTargets.emplace(edgeName, activityName);
                auto consumersIt = consumerIdsByGroup.find(groupKey);
                if (consumersIt != consumerIdsByGroup.end())
                {
                    auto sinks = std::move(consumersIt->second);
                    std::sort(sinks.begin(), sinks.end());
                    metadata.hypergraphEdgeSinks.emplace(edgeName, std::move(sinks));
                }
                else
                {
                    metadata.hypergraphEdgeSinks.emplace(edgeName, std::vector<int64_t>{});
                }
            }

            return metadata;
        }
    } // namespace

    GsimPass::GsimPass()
        : Pass("gsim", "GSim", "Build metadata-first gsim analysis scratchpad")
    {
    }

    GsimPass::GsimPass(GsimOptions options)
        : Pass("gsim", "GSim", "Build metadata-first gsim analysis scratchpad"),
          options_(std::move(options))
    {
    }

    bool GsimPass::validateGraph(const wolvrix::lib::grh::Graph &graph)
    {
        return validateGraphAnalysisPreconditions(graph, diags(), id());
    }

    void GsimPass::writeMetadata(const wolvrix::lib::grh::Graph &graph,
                                 std::string_view scratchpadNamespace,
                                 std::vector<int64_t> roots,
                                 std::map<std::string, std::vector<int64_t>> groups,
                                 std::vector<std::string> groupNames,
                                 std::vector<std::string> scheduleActivityOrder,
                                 std::map<std::string, std::vector<int64_t>> scheduleActivityMembers,
                                 std::map<std::string, std::string> scheduleActivityClasses,
                                 std::vector<std::string> hypergraphNodeNames,
                                 std::map<std::string, std::vector<int64_t>> hypergraphNodeMembers,
                                 std::vector<std::string> hypergraphEdgeNames,
                                 std::map<std::string, std::string> hypergraphEdgeSources,
                                 std::map<std::string, std::string> hypergraphEdgeTargets,
                                 std::map<std::string, std::vector<int64_t>> hypergraphEdgeSinks,
                                 std::vector<int64_t> topo,
                                 std::map<int64_t, std::string> classifications,
                                 std::map<int64_t, std::vector<int64_t>> predecessors,
                                 std::map<int64_t, std::vector<int64_t>> successors,
                                 std::vector<std::string> opDescriptors,
                                 int64_t opCount,
                                 int64_t graphRevision)
    {
        const std::string prefix(scratchpadNamespace);
        design().eraseScratchpadNamespace(prefix + ".");
        setScratchpad(prefix + ".roots", std::move(roots));
        setScratchpad(prefix + ".event_groups", std::move(groups));
        setScratchpad(prefix + ".event_group_names", std::move(groupNames));
        setScratchpad(prefix + ".schedule.kind", std::string("activity-v1"));
        setScratchpad(prefix + ".schedule.version", int64_t{1});
        setScratchpad(prefix + ".schedule.contract", std::string("gsim.activity.schedule.v1"));
        setScratchpad(prefix + ".schedule.activity_order", std::move(scheduleActivityOrder));
        setScratchpad(prefix + ".schedule.activity_members", std::move(scheduleActivityMembers));
        setScratchpad(prefix + ".schedule.activity_classes", std::move(scheduleActivityClasses));
        setScratchpad(prefix + ".hypergraph.kind", std::string("activity-connectivity-v1"));
        setScratchpad(prefix + ".hypergraph.version", int64_t{1});
        setScratchpad(prefix + ".hypergraph.contract", std::string("gsim.activity.hypergraph.v1"));
        setScratchpad(prefix + ".hypergraph.node_names", std::move(hypergraphNodeNames));
        setScratchpad(prefix + ".hypergraph.node_members", std::move(hypergraphNodeMembers));
        setScratchpad(prefix + ".hypergraph.edge_names", std::move(hypergraphEdgeNames));
        setScratchpad(prefix + ".hypergraph.edge_sources", std::move(hypergraphEdgeSources));
        setScratchpad(prefix + ".hypergraph.edge_targets", std::move(hypergraphEdgeTargets));
        setScratchpad(prefix + ".hypergraph.edge_sinks", std::move(hypergraphEdgeSinks));
        setScratchpad(prefix + ".topology.order", std::move(topo));
        setScratchpad(prefix + ".topology.predecessors", std::move(predecessors));
        setScratchpad(prefix + ".topology.successors", std::move(successors));
        setScratchpad(prefix + ".ops.classification", std::move(classifications));
        setScratchpad(prefix + ".ops.descriptors", std::move(opDescriptors));
        setScratchpad(prefix + ".graph_symbol", graph.symbol());
        setScratchpad(prefix + ".op_count", opCount);
        setScratchpad(prefix + ".graph_revision", graphRevision);
    }

    PassResult GsimPass::run()
    {
        std::vector<wolvrix::lib::grh::Graph *> targets;
        std::vector<std::string> targetNamespaces;
        if (options_.path.empty())
        {
            for (auto &entry : design().graphs())
            {
                if (entry.second)
                {
                    targets.push_back(entry.second.get());
                    targetNamespaces.push_back("gsim." + entry.second->symbol());
                }
            }
            std::vector<std::size_t> order(targets.size());
            for (std::size_t i = 0; i < order.size(); ++i)
            {
                order[i] = i;
            }
            std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
                return targets[lhs]->symbol() < targets[rhs]->symbol();
            });
            std::vector<wolvrix::lib::grh::Graph *> sortedTargets;
            std::vector<std::string> sortedNamespaces;
            sortedTargets.reserve(order.size());
            sortedNamespaces.reserve(order.size());
            for (std::size_t index : order)
            {
                sortedTargets.push_back(targets[index]);
                sortedNamespaces.push_back(targetNamespaces[index]);
            }
            targets = std::move(sortedTargets);
            targetNamespaces = std::move(sortedNamespaces);
        }
        else
        {
            std::string errorText;
            auto resolved = resolveTargetPath(design(), options_.path,
                                              TargetPathRequirement::GraphOnlyOrInstancePath,
                                              errorText);
            if (!resolved)
            {
                error("failed to resolve gsim target path", errorText);
                return PassResult{false, true, {}};
            }
            targets.push_back(resolved->targetGraph);
            targetNamespaces.push_back(resolved->scratchpadNamespace);
        }

        for (std::size_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex)
        {
            auto *graph = targets[targetIndex];
            const std::string &scratchpadNamespace = targetNamespaces[targetIndex];
            emitRealtimeStageLog(graph->symbol(), "stage=validate start");
            info(*graph, "[gsim] stage=validate start");
            if (!validateGraph(*graph))
            {
                return PassResult{false, true, {}};
            }
            emitRealtimeStageLog(graph->symbol(), "stage=validate done");
            info(*graph, "[gsim] stage=validate done");

            emitRealtimeStageLog(graph->symbol(), "stage=collect start");
            info(*graph, "[gsim] stage=collect start");
            const auto collectStart = std::chrono::steady_clock::now();
            const auto records = collectRecords(*graph);
            const auto collectEnd = std::chrono::steady_clock::now();
            const auto collectMs = std::chrono::duration_cast<std::chrono::milliseconds>(collectEnd - collectStart).count();
            {
                std::ostringstream progress;
                progress << "[gsim] stage=collect done ms=" << collectMs
                         << " ops=" << records.size();
                emitRealtimeStageLog(graph->symbol(), progress.str().substr(std::string("[gsim] ").size()));
                info(*graph, progress.str());
            }

            TopologyStats topoStats;
            emitRealtimeStageLog(graph->symbol(), "stage=topo start");
            info(*graph, "[gsim] stage=topo start");
            const auto topoStart = std::chrono::steady_clock::now();
            const auto topoOrder = stableTopologicalOrder(records, &topoStats);
            const auto topoEnd = std::chrono::steady_clock::now();
            const auto topoMs = std::chrono::duration_cast<std::chrono::milliseconds>(topoEnd - topoStart).count();
            {
                std::ostringstream progress;
                progress << "[gsim] stage=topo done ms=" << topoMs
                         << " ready_init=" << topoStats.initialReadySize
                         << " ready_max=" << topoStats.maxReadySize
                         << " processed=" << topoStats.processedCount;
                emitRealtimeStageLog(graph->symbol(), progress.str().substr(std::string("[gsim] ").size()));
                info(*graph, progress.str());
            }

            emitRealtimeStageLog(graph->symbol(), "stage=metadata start");
            info(*graph, "[gsim] stage=metadata start");
            const auto metadataStart = std::chrono::steady_clock::now();
            auto metadata = buildMetadata(*graph, records, topoOrder);
            const auto metadataEnd = std::chrono::steady_clock::now();
            const auto metadataMs = std::chrono::duration_cast<std::chrono::milliseconds>(metadataEnd - metadataStart).count();

            const int64_t rootCount = static_cast<int64_t>(metadata.roots.size());
            const int64_t topoCount = static_cast<int64_t>(metadata.topo.size());
            {
                std::ostringstream progress;
                progress << "[gsim] stage=metadata done ms=" << metadataMs
                         << " roots=" << rootCount
                         << " topo=" << topoCount;
                emitRealtimeStageLog(graph->symbol(), progress.str().substr(std::string("[gsim] ").size()));
                info(*graph, progress.str());
            }

            emitRealtimeStageLog(graph->symbol(), "stage=write start");
            info(*graph, "[gsim] stage=write start");
            const auto writeStart = std::chrono::steady_clock::now();
            writeMetadata(*graph,
                          scratchpadNamespace,
                          std::move(metadata.roots),
                          std::move(metadata.groups),
                          std::move(metadata.groupNames),
                          std::move(metadata.scheduleActivityOrder),
                          std::move(metadata.scheduleActivityMembers),
                          std::move(metadata.scheduleActivityClasses),
                          std::move(metadata.hypergraphNodeNames),
                          std::move(metadata.hypergraphNodeMembers),
                          std::move(metadata.hypergraphEdgeNames),
                          std::move(metadata.hypergraphEdgeSources),
                          std::move(metadata.hypergraphEdgeTargets),
                          std::move(metadata.hypergraphEdgeSinks),
                          std::move(metadata.topo),
                          std::move(metadata.classifications),
                          std::move(metadata.predecessors),
                          std::move(metadata.successors),
                          std::move(metadata.opDescriptors),
                          metadata.opCount,
                          static_cast<int64_t>(graph->revision()));
            const auto writeEnd = std::chrono::steady_clock::now();
            const auto writeMs = std::chrono::duration_cast<std::chrono::milliseconds>(writeEnd - writeStart).count();
            {
                std::ostringstream progress;
                progress << "[gsim] stage=write done ms=" << writeMs;
                emitRealtimeStageLog(graph->symbol(), progress.str().substr(std::string("[gsim] ").size()));
                info(*graph, progress.str());
            }

            std::ostringstream summary;
            summary << "[gsim] graph=" << graph->symbol()
                    << " ops=" << metadata.opCount
                    << " roots=" << rootCount
                    << " topo=" << topoCount
                    << " collect_ms=" << collectMs
                    << " topo_ms=" << topoMs
                    << " metadata_ms=" << metadataMs
                    << " write_ms=" << writeMs
                    << " ready_init=" << topoStats.initialReadySize
                    << " ready_max=" << topoStats.maxReadySize
                    << " processed=" << topoStats.processedCount;
            info(*graph, summary.str());
        }

        return PassResult{false, false, {}};
    }

} // namespace wolvrix::lib::transform
