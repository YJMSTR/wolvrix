#ifndef WOLVRIX_TRANSFORM_SUPERNODE_COARSENER_HPP
#define WOLVRIX_TRANSFORM_SUPERNODE_COARSENER_HPP

#include "core/transform.hpp"
#include "transform/supernode_graph.hpp"
#include <cstddef>

namespace wolvrix::lib::transform
{

class SuperNodeCoarsener {
public:
    SuperNodeCoarsener(SuperNodeGraph& sg, const grh::Graph& graph);

    void coarsen();
    void setMaxSuperNodeSize(size_t size) { maxSuperNodeSize_ = size; }

private:
    void mergeResetAll();
    void mergeWhenNodes();
    void mergeOut1();
    void mergeIn1();
    void mergeSublings();
    void resort();

    bool canMerge(SuperNodeId snId1, SuperNodeId snId2) const;
    void doMerge(SuperNodeId targetId, SuperNodeId sourceId);
    uint64_t computeHash(SuperNodeId snId) const;

    SuperNodeGraph& sg_;
    const grh::Graph& graph_;
    size_t maxSuperNodeSize_ = 35;
};

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_SUPERNODE_COARSENER_HPP
