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

        // Write to scratchpad
        setScratchpad("supernode.count", sg.nodeCount());
        setScratchpad("supernode.edge_count", sg.edgeCount());
        setScratchpad("supernode.cross_domain_edges", sg.crossDomainEdgeCount());

        result.changed = true;
    }

    return result;
}

} // namespace wolvrix::lib::transform
