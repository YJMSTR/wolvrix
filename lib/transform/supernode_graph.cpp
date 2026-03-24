#include "transform/supernode_graph.hpp"
#include <algorithm>
#include <functional>
#include <limits>
#include <queue>
#include <stdexcept>

namespace wolvrix::lib::transform
{

namespace
{

bool reachesExcludingDirectEdge(const std::vector<SuperNode> &nodes,
                                SuperNodeId startId,
                                SuperNodeId goalId,
                                SuperNodeId blockedFrom,
                                SuperNodeId blockedTo)
{
    std::unordered_set<SuperNodeId> visited;
    std::queue<SuperNodeId> queue;
    queue.push(startId);
    visited.insert(startId);

    while (!queue.empty())
    {
        const SuperNodeId current = queue.front();
        queue.pop();

        std::vector<SuperNodeId> successors(nodes[current].successors.begin(),
                                            nodes[current].successors.end());
        std::sort(successors.begin(), successors.end());

        for (const SuperNodeId succId : successors)
        {
            if (current == blockedFrom && succId == blockedTo)
            {
                continue;
            }
            if (succId == goalId)
            {
                return true;
            }
            if (visited.insert(succId).second)
            {
                queue.push(succId);
            }
        }
    }

    return false;
}

} // namespace

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

    if (!canContract(targetId, sourceId)) {
        throw std::invalid_argument("Merge would create a cycle");
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

    // Remove self-loops and any remaining references to sourceId
    target.predecessors.erase(targetId);
    target.successors.erase(targetId);
    target.predecessors.erase(sourceId);
    target.successors.erase(sourceId);

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
    std::priority_queue<SuperNodeId, std::vector<SuperNodeId>, std::greater<SuperNodeId>> ready;

    // Calculate in-degrees
    size_t validCount = 0;
    for (const auto& node : nodes_) {
        if (node.valid()) {
            validCount++;
            inDegree[node.id] = node.predecessors.size();
            if (inDegree[node.id] == 0) {
                ready.push(node.id);
            }
        }
    }

    // Kahn's algorithm
    while (!ready.empty()) {
        const SuperNodeId current = ready.top();
        ready.pop();
        result.push_back(current);

        std::vector<SuperNodeId> successors(nodes_[current].successors.begin(),
                                            nodes_[current].successors.end());
        std::sort(successors.begin(), successors.end());
        for (const auto succId : successors) {
            inDegree[succId]--;
            if (inDegree[succId] == 0) {
                ready.push(succId);
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
    try {
        auto sorted = topologicalSort();
        size_t validNodeCount = 0;
        for (const auto& node : nodes_) {
            if (node.valid()) {
                validNodeCount++;
            }
        }
        return sorted.size() != validNodeCount;
    } catch (const std::runtime_error&) {
        // topologicalSort() throws when there's a cycle
        return true;
    }
}

bool SuperNodeGraph::canContract(SuperNodeId targetId, SuperNodeId sourceId) const {
    if (!isValid(targetId) || !isValid(sourceId)) {
        return false;
    }
    if (targetId == sourceId) {
        return true;
    }

    const bool hasDirectEdgeTargetToSource = nodes_[targetId].successors.count(sourceId) > 0;
    const bool hasDirectEdgeSourceToTarget = nodes_[sourceId].successors.count(targetId) > 0;

    if (reachesExcludingDirectEdge(nodes_, targetId, sourceId, targetId,
                                   hasDirectEdgeTargetToSource ? sourceId : std::numeric_limits<SuperNodeId>::min())) {
        return false;
    }
    if (reachesExcludingDirectEdge(nodes_, sourceId, targetId, sourceId,
                                   hasDirectEdgeSourceToTarget ? targetId : std::numeric_limits<SuperNodeId>::min())) {
        return false;
    }
    return true;
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
