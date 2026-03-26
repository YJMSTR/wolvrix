#include "transform/gsim.hpp"

#include "core/grh.hpp"

#include <algorithm>
#include <map>
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
            std::map<int64_t, std::string> classifications;
            std::map<int64_t, std::vector<int64_t>> predecessors;
            std::map<int64_t, std::vector<int64_t>> successors;
            std::vector<std::string> opDescriptors;
            int64_t opCount = 0;
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

        std::vector<OpRecord> collectRecords(const wolvrix::lib::grh::Graph &graph)
        {
            std::vector<OpRecord> records;
            std::unordered_map<uint32_t, std::size_t> indexByOp;

            for (const auto opId : graph.operations())
            {
                if (!opId.valid())
                {
                    continue;
                }
                const auto op = graph.getOperation(opId);
                indexByOp.emplace(opId.index, records.size());
                records.push_back(OpRecord{opId,
                                           std::string(op.symbolText()),
                                           std::string(wolvrix::lib::grh::toString(op.kind())),
                                           {},
                                           {}});
            }

            for (auto &record : records)
            {
                const auto op = graph.getOperation(record.id);
                std::unordered_set<uint32_t> seenPreds;
                for (const auto operand : op.operands())
                {
                    const auto def = graph.getValue(operand).definingOp();
                    if (!def.valid() || def == record.id)
                    {
                        continue;
                    }
                    if (seenPreds.insert(def.index).second)
                    {
                        record.preds.push_back(def);
                    }
                }
                std::sort(record.preds.begin(), record.preds.end(), [](const auto &lhs, const auto &rhs) {
                    return lhs.index < rhs.index;
                });
            }

            for (const auto &record : records)
            {
                for (const auto pred : record.preds)
                {
                    auto it = indexByOp.find(pred.index);
                    if (it == indexByOp.end())
                    {
                        continue;
                    }
                    records[it->second].succs.push_back(record.id);
                }
            }

            for (auto &record : records)
            {
                std::sort(record.succs.begin(), record.succs.end(), [](const auto &lhs, const auto &rhs) {
                    return lhs.index < rhs.index;
                });
            }

            std::sort(records.begin(), records.end(), [](const auto &lhs, const auto &rhs) {
                return lhs.id.index < rhs.id.index;
            });
            return records;
        }

        std::vector<wolvrix::lib::grh::OperationId> stableTopologicalOrder(const std::vector<OpRecord> &records)
        {
            std::unordered_map<uint32_t, std::size_t> indegree;
            std::unordered_map<uint32_t, std::vector<wolvrix::lib::grh::OperationId>> succs;
            std::vector<wolvrix::lib::grh::OperationId> ready;

            for (const auto &record : records)
            {
                indegree[record.id.index] = record.preds.size();
                succs.emplace(record.id.index, record.succs);
                if (record.preds.empty())
                {
                    ready.push_back(record.id);
                }
            }
            std::sort(ready.begin(), ready.end(), [](const auto &lhs, const auto &rhs) {
                return lhs.index < rhs.index;
            });

            std::vector<wolvrix::lib::grh::OperationId> order;
            while (!ready.empty())
            {
                const auto next = ready.front();
                ready.erase(ready.begin());
                order.push_back(next);
                auto it = succs.find(next.index);
                if (it == succs.end())
                {
                    continue;
                }
                for (const auto succ : it->second)
                {
                    auto indegreeIt = indegree.find(succ.index);
                    if (indegreeIt == indegree.end() || indegreeIt->second == 0)
                    {
                        continue;
                    }
                    --indegreeIt->second;
                    if (indegreeIt->second == 0)
                    {
                        ready.push_back(succ);
                    }
                }
                std::sort(ready.begin(), ready.end(), [](const auto &lhs, const auto &rhs) {
                    return lhs.index < rhs.index;
                });
            }

            if (order.size() != records.size())
            {
                order.clear();
                for (const auto &record : records)
                {
                    order.push_back(record.id);
                }
            }
            return order;
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
                                 std::vector<int64_t> roots,
                                 std::map<std::string, std::vector<int64_t>> groups,
                                 std::vector<std::string> groupNames,
                                 std::vector<int64_t> topo,
                                 std::map<int64_t, std::string> classifications,
                                 std::map<int64_t, std::vector<int64_t>> predecessors,
                                 std::map<int64_t, std::vector<int64_t>> successors,
                                 std::vector<std::string> opDescriptors,
                                 int64_t opCount)
    {
        const std::string prefix = "gsim." + graph.symbol() + ".";
        design().eraseScratchpadNamespace(prefix);
        setScratchpad(prefix + "roots", std::move(roots));
        setScratchpad(prefix + "event_groups", std::move(groups));
        setScratchpad(prefix + "event_group_names", std::move(groupNames));
        setScratchpad(prefix + "topology.order", std::move(topo));
        setScratchpad(prefix + "topology.predecessors", std::move(predecessors));
        setScratchpad(prefix + "topology.successors", std::move(successors));
        setScratchpad(prefix + "ops.classification", std::move(classifications));
        setScratchpad(prefix + "ops.descriptors", std::move(opDescriptors));
        setScratchpad(prefix + "schedule.kind", std::string("placeholder"));
        setScratchpad(prefix + "schedule.version", int64_t{1});
        setScratchpad(prefix + "schedule.contract", std::string("metadata-first-mvp"));
        setScratchpad(prefix + "hypergraph.kind", std::string("placeholder"));
        setScratchpad(prefix + "hypergraph.version", int64_t{1});
        setScratchpad(prefix + "graph_symbol", graph.symbol());
        setScratchpad(prefix + "op_count", opCount);
    }

    PassResult GsimPass::run()
    {
        std::vector<wolvrix::lib::grh::Graph *> targets;
        if (options_.path.empty())
        {
            for (auto &entry : design().graphs())
            {
                if (entry.second)
                {
                    targets.push_back(entry.second.get());
                }
            }
            std::sort(targets.begin(), targets.end(), [](const auto *lhs, const auto *rhs) {
                return lhs->symbol() < rhs->symbol();
            });
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
        }

        for (auto *graph : targets)
        {
            if (!validateGraph(*graph))
            {
                return PassResult{false, true, {}};
            }

            const auto records = collectRecords(*graph);
            const auto topoOrder = stableTopologicalOrder(records);

            std::vector<int64_t> roots;
            std::vector<int64_t> topo;
            std::map<std::string, std::vector<int64_t>> groups;
            std::map<int64_t, std::string> classifications;
            std::map<int64_t, std::vector<int64_t>> predecessors;
            std::map<int64_t, std::vector<int64_t>> successors;
            std::vector<std::string> opDescriptors;

            for (const auto &record : records)
            {
                const auto op = graph->getOperation(record.id);
                const auto className = classifyOp(op.kind());
                classifications.emplace(static_cast<int64_t>(record.id.index), className);

                if (record.preds.empty() || isEventRootKind(op.kind()))
                {
                    roots.push_back(static_cast<int64_t>(record.id.index));
                }

                groups[eventGroupKey(op)].push_back(static_cast<int64_t>(record.id.index));

                std::vector<int64_t> predIds;
                for (const auto pred : record.preds)
                {
                    predIds.push_back(static_cast<int64_t>(pred.index));
                }
                predecessors.emplace(static_cast<int64_t>(record.id.index), std::move(predIds));

                std::vector<int64_t> succIds;
                for (const auto succ : record.succs)
                {
                    succIds.push_back(static_cast<int64_t>(succ.index));
                }
                successors.emplace(static_cast<int64_t>(record.id.index), std::move(succIds));

                opDescriptors.push_back(std::to_string(record.id.index) + ":" + record.kind + ":" + className + ":" + record.symbol);
            }

            std::sort(roots.begin(), roots.end());
            roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
            for (const auto opId : topoOrder)
            {
                topo.push_back(static_cast<int64_t>(opId.index));
            }

            std::vector<std::string> groupNames;
            for (const auto &[name, ids] : groups)
            {
                (void)ids;
                groupNames.push_back(name);
            }

            writeMetadata(*graph,
                          std::move(roots),
                          std::move(groups),
                          std::move(groupNames),
                          std::move(topo),
                          std::move(classifications),
                          std::move(predecessors),
                          std::move(successors),
                          std::move(opDescriptors),
                          static_cast<int64_t>(records.size()));
        }

        return PassResult{false, false, {}};
    }

} // namespace wolvrix::lib::transform
