#include "transform/supernode_coarsener.hpp"
#include <algorithm>
#include <unordered_map>

namespace wolvrix::lib::transform
{

SuperNodeCoarsener::SuperNodeCoarsener(SuperNodeGraph& sg, const grh::Graph& graph)
    : sg_(sg), graph_(graph) {}

void SuperNodeCoarsener::coarsen() {
    mergeResetAll();
    mergeWhenNodes();
    resort();

    bool changed = true;
    while (changed) {
        changed = false;
        mergeOut1();
        mergeIn1();
        mergeSublings();
    }
}

void SuperNodeCoarsener::mergeResetAll() {
    // Simplified implementation - merge reset-related nodes
}

void SuperNodeCoarsener::mergeWhenNodes() {
    // Simplified implementation - merge nodes with shared conditions
}

void SuperNodeCoarsener::mergeOut1() {
    std::vector<SuperNodeId> toMerge;
    for (size_t i = 0; i < sg_.nodeCount(); i++) {
        if (sg_.isValid(i) && sg_.successors(i).size() == 1) {
            auto succId = *sg_.successors(i).begin();
            if (canMerge(i, succId)) {
                toMerge.push_back(i);
            }
        }
    }
    for (auto id : toMerge) {
        auto succId = *sg_.successors(id).begin();
        doMerge(succId, id);
    }
}

void SuperNodeCoarsener::mergeIn1() {
    std::vector<SuperNodeId> toMerge;
    for (size_t i = 0; i < sg_.nodeCount(); i++) {
        if (sg_.isValid(i) && sg_.predecessors(i).size() == 1) {
            auto predId = *sg_.predecessors(i).begin();
            if (canMerge(predId, i)) {
                toMerge.push_back(i);
            }
        }
    }
    for (auto id : toMerge) {
        auto predId = *sg_.predecessors(id).begin();
        doMerge(predId, id);
    }
}

void SuperNodeCoarsener::mergeSublings() {
    std::unordered_map<uint64_t, std::vector<SuperNodeId>> groups;
    for (size_t i = 0; i < sg_.nodeCount(); i++) {
        if (sg_.isValid(i)) {
            uint64_t hash = computeHash(i);
            groups[hash].push_back(i);
        }
    }
    for (const auto& [hash, nodes] : groups) {
        if (nodes.size() > 1) {
            for (size_t i = 1; i < nodes.size(); i++) {
                if (canMerge(nodes[0], nodes[i])) {
                    doMerge(nodes[0], nodes[i]);
                }
            }
        }
    }
}

void SuperNodeCoarsener::resort() {
    auto sorted = sg_.topologicalSort();
    for (size_t i = 0; i < sorted.size(); i++) {
        sg_.getNode(sorted[i]).topologicalOrder = i;
    }
}

bool SuperNodeCoarsener::canMerge(SuperNodeId snId1, SuperNodeId snId2) const {
    if (!sg_.isValid(snId1) || !sg_.isValid(snId2)) {
        return false;
    }
    const auto& node1 = sg_.getNode(snId1);
    const auto& node2 = sg_.getNode(snId2);
    if (node1.memberCount() + node2.memberCount() > maxSuperNodeSize_) {
        return false;
    }
    if (node1.timingDomain != node2.timingDomain) {
        return false;
    }
    return true;
}

void SuperNodeCoarsener::doMerge(SuperNodeId targetId, SuperNodeId sourceId) {
    sg_.merge(targetId, sourceId);
}

uint64_t SuperNodeCoarsener::computeHash(SuperNodeId snId) const {
    uint64_t hash = 0;
    const auto& preds = sg_.predecessors(snId);
    for (auto predId : preds) {
        hash ^= predId + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    }
    return hash;
}

} // namespace wolvrix::lib::transform
