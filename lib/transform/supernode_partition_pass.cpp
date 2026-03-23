#include "transform/supernode_partition_pass.hpp"
#include "transform/supernode_graph.hpp"
#include "transform/timing_domain_analyzer.hpp"
#include "transform/supernode_coarsener.hpp"
#include "transform/supernode_partitioner.hpp"

namespace wolvrix::lib::transform
{

SuperNodePartitionPass::SuperNodePartitionPass()
    : Pass("supernode-partition", "SuperNode Partition",
           "Build and annotate supernode partition metadata") {}

PassResult SuperNodePartitionPass::run() {
    PassResult result;

    for (auto& entry : design().graphs()) {
        auto& graph = *entry.second;
        auto graphSymbol = entry.first;

        // Check preconditions: design must be flattened (no kInstance ops)
        for (const auto& opId : graph.operations()) {
            auto op = graph.getOperation(opId);
            if (op.kind() == wolvrix::lib::grh::OperationKind::kInstance) {
                diags().error("supernode-partition",
                    "Design contains kInstance operations - must flatten before partitioning",
                    "Graph: " + graphSymbol.text());
                result.failed = true;
                return result;
            }
            if (op.kind() == wolvrix::lib::grh::OperationKind::kBlackbox) {
                diags().error("supernode-partition",
                    "Design contains kBlackbox operations - not supported",
                    "Graph: " + graphSymbol.text());
                result.failed = true;
                return result;
            }
        }

        // Analyze timing domains
        TimingDomainAnalyzer analyzer(graph);
        auto opToDomain = analyzer.assignTimingDomains();

        // Group operations by timing domain
        std::unordered_map<std::string, std::vector<wolvrix::lib::grh::OperationId>> domainOps;
        for (const auto& [opId, domain] : opToDomain) {
            domainOps[domain].push_back(opId);
        }

        // Process each timing domain separately
        for (const auto& [domain, ops] : domainOps) {
            // Initialize supernode graph for this domain
            SuperNodeGraph sg;

            // Create supernodes for operations in this domain
            for (const auto& opId : ops) {
                auto op = graph.getOperation(opId);
                SuperNodeId snId = sg.createSuperNode();
                sg.addMember(snId, op.id());
                sg.getNode(snId).timingDomain = domain;
            }

            // Build edges within this domain
            for (const auto& opId : ops) {
                auto op = graph.getOperation(opId);
                auto srcSnId = sg.getSuperNodeForOp(op.id());
                if (!srcSnId) continue;

                for (const auto& operand : op.operands()) {
                    auto value = graph.getValue(operand);
                    auto defOpId = value.definingOp();
                    if (defOpId.valid()) {
                        auto dstSnId = sg.getSuperNodeForOp(defOpId);
                        if (dstSnId && *srcSnId != *dstSnId) {
                            sg.getNode(*dstSnId).successors.insert(*srcSnId);
                            sg.getNode(*srcSnId).predecessors.insert(*dstSnId);
                        }
                    }
                }
            }

            // Coarsen
            SuperNodeCoarsener coarsener(sg, graph);
            coarsener.setMaxSuperNodeSize(maxSuperNodeSize_);
            coarsener.coarsen();

            // Partition
            SuperNodePartitioner partitioner(sg);
            partitioner.setMaxSuperNodeSize(maxSuperNodeSize_);
            partitioner.partition();

            // Write scratchpad metadata with graph and domain namespace
            std::string prefix = "supernode." + graphSymbol.text() + "." + domain + ".";

            // Basic statistics
            setScratchpad(prefix + "count", sg.nodeCount());
            setScratchpad(prefix + "edge_count", sg.edgeCount());

            // Calculate size statistics
            size_t totalMembers = 0;
            size_t maxSize = 0;
            for (const auto& snId : sg.validNodeIds()) {
                const auto& node = sg.getNode(snId);
                size_t memberCount = node.members.size();
                totalMembers += memberCount;
                maxSize = std::max(maxSize, memberCount);
            }
            double avgSize = sg.nodeCount() > 0 ? static_cast<double>(totalMembers) / sg.nodeCount() : 0.0;
            setScratchpad(prefix + "avg_size", avgSize);
            setScratchpad(prefix + "max_size", maxSize);

            // Build op_to_sn and sn_to_ops mappings
            std::unordered_map<wolvrix::lib::grh::OperationId, SuperNodeId, wolvrix::lib::grh::OperationIdHash> opToSn;
            std::unordered_map<SuperNodeId, std::vector<wolvrix::lib::grh::OperationId>> snToOps;
            for (const auto& snId : sg.validNodeIds()) {
                const auto& node = sg.getNode(snId);
                snToOps[snId] = node.members;
                for (const auto& opId : node.members) {
                    opToSn[opId] = snId;
                }
            }
            setScratchpad(prefix + "op_to_sn", opToSn);
            setScratchpad(prefix + "sn_to_ops", snToOps);

            // Build predecessors and successors maps
            std::unordered_map<SuperNodeId, std::vector<SuperNodeId>> predecessors;
            std::unordered_map<SuperNodeId, std::vector<SuperNodeId>> successors;
            for (const auto& snId : sg.validNodeIds()) {
                const auto& node = sg.getNode(snId);
                predecessors[snId] = std::vector<SuperNodeId>(node.predecessors.begin(), node.predecessors.end());
                successors[snId] = std::vector<SuperNodeId>(node.successors.begin(), node.successors.end());
            }
            setScratchpad(prefix + "predecessors", predecessors);
            setScratchpad(prefix + "successors", successors);

            // Topological order
            auto topoOrder = sg.topologicalSort();
            setScratchpad(prefix + "topo_order", topoOrder);

            // Timing domain
            setScratchpad(prefix + "timing_domain", domain);

            // Cross-domain edges (empty for per-domain graphs)
            std::vector<std::pair<SuperNodeId, SuperNodeId>> crossDomainEdges;
            setScratchpad(prefix + "cross_domain_edges", crossDomainEdges);

            // Count cut edges
            size_t cutEdges = 0;
            for (const auto& snId : sg.validNodeIds()) {
                const auto& node = sg.getNode(snId);
                cutEdges += node.successors.size();
            }
            setScratchpad(prefix + "cut_edges", cutEdges);

            result.changed = true;
        }
    }

    return result;
}

} // namespace wolvrix::lib::transform
