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

        // Analyze timing domains
        TimingDomainAnalyzer analyzer(graph);
        auto opToDomain = analyzer.assignTimingDomains();

        // Initialize supernode graph
        SuperNodeGraph sg;
        for (const auto& opId : graph.operations()) {
            auto op = graph.getOperation(opId);
            SuperNodeId snId = sg.createSuperNode();
            sg.addMember(snId, op.id());
            auto it = opToDomain.find(op.id());
            if (it != opToDomain.end()) {
                sg.getNode(snId).timingDomain = it->second;
            }
        }

        // Build edges
        for (const auto& opId : graph.operations()) {
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

        // Write comprehensive scratchpad metadata
        // Basic statistics
        setScratchpad("supernode.count", sg.nodeCount());
        setScratchpad("supernode.edge_count", sg.edgeCount());

        // Calculate size statistics
        size_t totalMembers = 0;
        size_t maxSize = 0;
        for (const auto& [snId, node] : sg.nodes()) {
            size_t memberCount = node.members.size();
            totalMembers += memberCount;
            maxSize = std::max(maxSize, memberCount);
        }
        double avgSize = sg.nodeCount() > 0 ? static_cast<double>(totalMembers) / sg.nodeCount() : 0.0;
        setScratchpad("supernode.avg_size", avgSize);
        setScratchpad("supernode.max_size", maxSize);

        // Count cross-domain edges
        size_t crossDomainEdges = 0;
        for (const auto& [snId, node] : sg.nodes()) {
            for (const auto& succId : node.successors) {
                const auto& succNode = sg.getNode(succId);
                if (node.timingDomain != succNode.timingDomain) {
                    crossDomainEdges++;
                }
            }
        }
        setScratchpad("supernode.cross_domain_edges", crossDomainEdges);

        // Count cut edges (edges between different supernodes)
        size_t cutEdges = 0;
        for (const auto& [snId, node] : sg.nodes()) {
            cutEdges += node.successors.size();
        }
        setScratchpad("supernode.cut_edges", cutEdges);

        result.changed = true;
    }

    return result;
}

} // namespace wolvrix::lib::transform
