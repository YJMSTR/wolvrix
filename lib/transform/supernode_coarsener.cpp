#include "transform/supernode_coarsener.hpp"
#include "transform/supernode_control.hpp"
#include <algorithm>
#include <unordered_map>

namespace wolvrix::lib::transform
{

SuperNodeCoarsener::SuperNodeCoarsener(SuperNodeGraph& sg, const grh::Graph& graph)
    : sg_(sg), graph_(graph) {}

void SuperNodeCoarsener::coarsen() {
    bool changed = true;
    while (changed) {
        changed = false;
        changed |= mergeResetAll();
        changed |= mergeWhenNodes();
        if (changed) {
            resort();
        }
        changed |= mergeOut1();
        changed |= mergeIn1();
        changed |= mergeSublings();
    }
}

bool SuperNodeCoarsener::mergeResetAll() {
    bool changed = false;

    std::unordered_map<std::string, std::vector<SuperNodeId>> resetGroups;

    for (const auto& snId : sg_.validNodeIds()) {
        const auto& node = sg_.getNode(snId);
        std::vector<std::string> signatures;

        for (const auto& opId : node.members) {
            auto op = graph_.getOperation(opId);
            if (op.kind() == wolvrix::lib::grh::OperationKind::kRegisterWritePort ||
                op.kind() == wolvrix::lib::grh::OperationKind::kMemoryWritePort ||
                op.kind() == wolvrix::lib::grh::OperationKind::kLatchWritePort) {
                if (hasResetLikeControl(graph_, op)) {
                    signatures.push_back(sequentialControlSignature(graph_, op));
                }
            }
        }

        if (!signatures.empty()) {
            std::sort(signatures.begin(), signatures.end());
            std::ostringstream key;
            for (size_t i = 0; i < signatures.size(); ++i) {
                if (i != 0) {
                    key << "||";
                }
                key << signatures[i];
            }
            resetGroups[key.str()].push_back(snId);
        }
    }

    for (const auto& [sig, nodes] : resetGroups) {
        if (nodes.size() > 1) {
            SuperNodeId target = nodes[0];
            for (size_t i = 1; i < nodes.size(); i++) {
                if (canMerge(target, nodes[i])) {
                    doMerge(target, nodes[i]);
                    changed = true;
                }
            }
        }
    }
    return changed;
}

bool SuperNodeCoarsener::mergeWhenNodes() {
    bool changed = false;

    // Group nodes by their kMux condition operands
    // Key = condition operand's defining operation ID
    std::unordered_map<std::string, std::vector<SuperNodeId>> condGroups;

    for (const auto& snId : sg_.validNodeIds()) {
        const auto& node = sg_.getNode(snId);

        // Check if this supernode contains kMux operations
        std::string condSig;
        for (const auto& opId : node.members) {
            auto op = graph_.getOperation(opId);
            if (op.kind() == wolvrix::lib::grh::OperationKind::kMux) {
                auto operands = op.operands();
                if (operands.size() >= 3) {
                    condSig = structuralValueSignature(graph_, operands[0]);
                    break;
                }
            }
        }

        if (!condSig.empty()) {
            condGroups[condSig].push_back(snId);
        }
    }

    // Merge nodes with identical kMux conditions
    for (const auto& [sig, nodes] : condGroups) {
        if (nodes.size() > 1) {
            SuperNodeId target = nodes[0];
            for (size_t i = 1; i < nodes.size(); i++) {
                if (canMerge(target, nodes[i]) && haveSamePredecessors(target, nodes[i])) {
                    doMerge(target, nodes[i]);
                    changed = true;
                }
            }
        }
    }
    return changed;
}

bool SuperNodeCoarsener::mergeOut1() {
    bool changed = false;
    std::vector<SuperNodeId> toMerge;
    for (const auto& snId : sg_.validNodeIds()) {
        if (sg_.successors(snId).size() == 1) {
            auto succId = *sg_.successors(snId).begin();
            if (canMerge(snId, succId)) {
                toMerge.push_back(snId);
            }
        }
    }
    for (auto id : toMerge) {
        if (sg_.isValid(id) && sg_.successors(id).size() == 1) {
            auto succId = *sg_.successors(id).begin();
            if (sg_.isValid(succId) && canMerge(id, succId)) {
                doMerge(succId, id);
                changed = true;
            }
        }
    }
    return changed;
}

bool SuperNodeCoarsener::mergeIn1() {
    bool changed = false;
    std::vector<SuperNodeId> toMerge;
    for (const auto& snId : sg_.validNodeIds()) {
        if (sg_.predecessors(snId).size() == 1) {
            auto predId = *sg_.predecessors(snId).begin();
            if (canMerge(predId, snId)) {
                toMerge.push_back(snId);
            }
        }
    }
    for (auto id : toMerge) {
        if (sg_.isValid(id) && sg_.predecessors(id).size() == 1) {
            auto predId = *sg_.predecessors(id).begin();
            if (sg_.isValid(predId) && canMerge(predId, id)) {
                doMerge(predId, id);
                changed = true;
            }
        }
    }
    return changed;
}

bool SuperNodeCoarsener::mergeSublings() {
    bool changed = false;
    std::unordered_map<uint64_t, std::vector<SuperNodeId>> groups;
    for (const auto& snId : sg_.validNodeIds()) {
        uint64_t hash = computeDeterministicHash(snId);
        groups[hash].push_back(snId);
    }
    for (const auto& [hash, nodes] : groups) {
        if (nodes.size() > 1) {
            for (size_t i = 1; i < nodes.size(); i++) {
                if (sg_.isValid(nodes[0]) && sg_.isValid(nodes[i]) &&
                    canMerge(nodes[0], nodes[i]) && haveSamePredecessors(nodes[0], nodes[i])) {
                    doMerge(nodes[0], nodes[i]);
                    changed = true;
                }
            }
        }
    }
    return changed;
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
    if (!haveCompatibleSequentialControl(graph_, node1, node2)) {
        return false;
    }
    if (!sg_.canContract(snId1, snId2) && !sg_.canContract(snId2, snId1)) {
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

uint64_t SuperNodeCoarsener::computeDeterministicHash(SuperNodeId snId) const {
    // Sort predecessors for deterministic hashing
    std::vector<SuperNodeId> sortedPreds(sg_.predecessors(snId).begin(), sg_.predecessors(snId).end());
    std::sort(sortedPreds.begin(), sortedPreds.end());

    uint64_t hash = 0;
    for (auto predId : sortedPreds) {
        hash ^= predId + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    }
    return hash;
}

bool SuperNodeCoarsener::haveSamePredecessors(SuperNodeId snId1, SuperNodeId snId2) const {
    const auto& preds1 = sg_.predecessors(snId1);
    const auto& preds2 = sg_.predecessors(snId2);
    return preds1 == preds2;
}

} // namespace wolvrix::lib::transform
