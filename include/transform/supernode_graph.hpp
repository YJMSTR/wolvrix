#ifndef WOLVRIX_TRANSFORM_SUPERNODE_GRAPH_HPP
#define WOLVRIX_TRANSFORM_SUPERNODE_GRAPH_HPP

#include "core/transform.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <optional>

namespace wolvrix::lib::transform
{

using SuperNodeId = int;

struct SuperNode {
    SuperNodeId id = -1;
    std::vector<grh::OperationId> members;
    std::unordered_set<SuperNodeId> predecessors;
    std::unordered_set<SuperNodeId> successors;
    int topologicalOrder = -1;
    std::string timingDomain;

    size_t memberCount() const { return members.size(); }
    bool valid() const { return id >= 0; }
};

class SuperNodeGraph {
public:
    SuperNodeGraph() = default;

    // Creation and management
    SuperNodeId createSuperNode();
    void addMember(SuperNodeId snId, grh::OperationId opId);
    void merge(SuperNodeId targetId, SuperNodeId sourceId);
    void remove(SuperNodeId snId);

    // Query
    const SuperNode& getNode(SuperNodeId id) const;
    SuperNode& getNode(SuperNodeId id);
    std::optional<SuperNodeId> getSuperNodeForOp(grh::OperationId opId) const;
    const std::unordered_set<SuperNodeId>& predecessors(SuperNodeId id) const;
    const std::unordered_set<SuperNodeId>& successors(SuperNodeId id) const;

    // Topological sort
    std::vector<SuperNodeId> topologicalSort() const;
    bool hasCircularDependency() const;
    bool canContract(SuperNodeId targetId, SuperNodeId sourceId) const;

    // Statistics
    size_t nodeCount() const;
    size_t edgeCount() const;
    size_t crossDomainEdgeCount() const;

    // Validation
    bool isValid(SuperNodeId id) const;

    // Stable traversal
    std::vector<SuperNodeId> validNodeIds() const;

    // Iteration support
    const std::vector<SuperNode>& nodes() const { return nodes_; }

private:
    std::vector<SuperNode> nodes_;
    std::unordered_map<grh::OperationId, SuperNodeId, grh::OperationIdHash> op2super_;
    std::vector<SuperNodeId> freeList_;
    SuperNodeId nextId_ = 0;
};

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_SUPERNODE_GRAPH_HPP
