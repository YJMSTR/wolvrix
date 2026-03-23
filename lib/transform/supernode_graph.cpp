#include "transform/supernode_graph.hpp"
#include <algorithm>
#include <queue>
#include <stdexcept>

namespace wolvrix::lib::transform
{

SuperNodeId SuperNodeGraph::createSuperNode() {
    SuperNodeId id;
    if (!freeList_.empty()) {
        id = freeList_.back();
        freeList_.pop_back();
        nodes_[id] = SuperNode{};
        nodes_[id].id = id;
    } else {
        id = nextId_++;
        nodes_.push_back(SuperNode{});
        nodes_.back().id = id;
    }
    return id;
}

void SuperNodeGraph::addMember(SuperNodeId snId, grh::OperationId opId) {
    if (!isValid(snId)) {
        throw std::invalid_argument("Invalid SuperNode ID");
    }

    auto it = op2super_.find(opId);
    if (it != op2super_.end()) {
        throw std::invalid_argument("Operation already belongs to another SuperNode");
    }

    nodes_[snId].members.push_back(opId);
    op2super_[opId] = snId;
}

void SuperNodeGraph::merge(SuperNodeId targetId, SuperNodeId sourceId) {
    if (!isValid(targetId) || !isValid(sourceId)) {
        throw std::invalid_argument("Invalid SuperNode ID");
    }

    if (targetId == sourceId) {
        return;
    }

    // Check for cycle: use BFS to detect if target is reachable from source
    // If target is reachable from source, merging would create a cycle
    std::unordered_set<SuperNodeId> visited;
    std::queue<SuperNodeId> queue;
    queue.push(sourceId);
    visited.insert(sourceId);

    while (!queue.empty()) {
        SuperNodeId current = queue.front();
        queue.pop();

        if (current == targetId) {
            throw std::invalid_argument("Merge would create a circular dependency");
        }

        for (auto succId : nodes_[current].successors) {
            if (visited.find(succId) == visited.end()) {
                visited.insert(succId);
                queue.push(succId);
            }
        }
    }

    auto& target = nodes_[targetId];
    auto& source = nodes_[sourceId];

    // Move members
    for (auto opId : source.members) {
        target.members.push_back(opId);
        op2super_[opId] = targetId;
    }

    // Update predecessors
    for (auto predId : source.predecessors) {
        if (predId != targetId) {
            target.predecessors.insert(predId);
            nodes_[predId].successors.erase(sourceId);
            nodes_[predId].successors.insert(targetId);
        }
    }

    // Update successors
    for (auto succId : source.successors) {
        if (succId != targetId) {
            target.successors.insert(succId);
            nodes_[succId].predecessors.erase(sourceId);
            nodes_[succId].predecessors.insert(targetId);
        }
    }

    // Remove self-loops
    target.predecessors.erase(targetId);
    target.successors.erase(targetId);

    // Clear source and add to free list
    source.members.clear();
    source.predecessors.clear();
    source.successors.clear();
    source.id = -1;
    freeList_.push_back(sourceId);
}

void SuperNodeGraph::remove(SuperNodeId snId) {
    if (!isValid(snId)) {
        return;
    }

    auto& node = nodes_[snId];

    // Remove from op2super mapping
    for (auto opId : node.members) {
        op2super_.erase(opId);
    }

    // Update predecessors
    for (auto predId : node.predecessors) {
        nodes_[predId].successors.erase(snId);
    }

    // Update successors
    for (auto succId : node.successors) {
        nodes_[succId].predecessors.erase(snId);
    }

    // Clear and add to free list
    node.members.clear();
    node.predecessors.clear();
    node.successors.clear();
    node.id = -1;
    freeList_.push_back(snId);
}

const SuperNode& SuperNodeGraph::getNode(SuperNodeId id) const {
    if (!isValid(id)) {
        throw std::out_of_range("Invalid SuperNode ID");
    }
    return nodes_[id];
}

SuperNode& SuperNodeGraph::getNode(SuperNodeId id) {
    if (!isValid(id)) {
        throw std::out_of_range("Invalid SuperNode ID");
    }
    return nodes_[id];
}

std::optional<SuperNodeId> SuperNodeGraph::getSuperNodeForOp(grh::OperationId opId) const {
    auto it = op2super_.find(opId);
    if (it == op2super_.end()) {
        return std::nullopt;
    }
    return it->second;
}

const std::unordered_set<SuperNodeId>& SuperNodeGraph::predecessors(SuperNodeId id) const {
    return getNode(id).predecessors;
}

const std::unordered_set<SuperNodeId>& SuperNodeGraph::successors(SuperNodeId id) const {
    return getNode(id).successors;
}

std::vector<SuperNodeId> SuperNodeGraph::topologicalSort() const {
    std::vector<SuperNodeId> result;
    std::unordered_map<SuperNodeId, int> inDegree;
    std::queue<SuperNodeId> queue;

    // Calculate in-degrees
    size_t validCount = 0;
    for (const auto& node : nodes_) {
        if (node.valid()) {
            validCount++;
            inDegree[node.id] = node.predecessors.size();
            if (inDegree[node.id] == 0) {
                queue.push(node.id);
            }
        }
    }

    // Kahn's algorithm
    while (!queue.empty()) {
        SuperNodeId current = queue.front();
        queue.pop();
        result.push_back(current);

        for (auto succId : nodes_[current].successors) {
            inDegree[succId]--;
            if (inDegree[succId] == 0) {
                queue.push(succId);
            }
        }
    }

    // Fail loudly on cyclic input
    if (result.size() != validCount) {
        throw std::runtime_error("Topological sort failed: graph contains cycles");
    }

    return result;
}

bool SuperNodeGraph::hasCircularDependency() const {
    auto sorted = topologicalSort();
    size_t validNodeCount = 0;
    for (const auto& node : nodes_) {
        if (node.valid()) {
            validNodeCount++;
        }
    }
    return sorted.size() != validNodeCount;
}

size_t SuperNodeGraph::nodeCount() const {
    size_t count = 0;
    for (const auto& node : nodes_) {
        if (node.valid()) {
            count++;
        }
    }
    return count;
}

size_t SuperNodeGraph::edgeCount() const {
    size_t count = 0;
    for (const auto& node : nodes_) {
        if (node.valid()) {
            count += node.successors.size();
        }
    }
    return count;
}

size_t SuperNodeGraph::crossDomainEdgeCount() const {
    size_t count = 0;
    for (const auto& node : nodes_) {
        if (node.valid()) {
            for (auto succId : node.successors) {
                if (node.timingDomain != nodes_[succId].timingDomain) {
                    count++;
                }
            }
        }
    }
    return count;
}

bool SuperNodeGraph::isValid(SuperNodeId id) const {
    return id >= 0 && id < static_cast<SuperNodeId>(nodes_.size()) && nodes_[id].valid();
}

std::vector<SuperNodeId> SuperNodeGraph::validNodeIds() const {
    std::vector<SuperNodeId> result;
    for (const auto& node : nodes_) {
        if (node.valid()) {
            result.push_back(node.id);
        }
    }
    return result;
}

} // namespace wolvrix::lib::transform

