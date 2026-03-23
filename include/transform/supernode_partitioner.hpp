#ifndef WOLVRIX_TRANSFORM_SUPERNODE_PARTITIONER_HPP
#define WOLVRIX_TRANSFORM_SUPERNODE_PARTITIONER_HPP

#include "core/transform.hpp"
#include "transform/supernode_graph.hpp"
#include <cstddef>
#include <vector>

namespace wolvrix::lib::transform
{

class SuperNodePartitioner {
public:
    explicit SuperNodePartitioner(SuperNodeGraph& sg);

    void partition();
    void setMaxSuperNodeSize(size_t size) { maxSuperNodeSize_ = size; }

private:
    struct DPState {
        int cost = 0;
        int backtrack = -1;
        int cutCost = 0;
        int internalCost = 0;
    };

    std::vector<int> computeOptimalCuts();
    int computeCutCost(int start, int end) const;
    void mergeByIntervals(const std::vector<int>& cuts);

    SuperNodeGraph& sg_;
    size_t maxSuperNodeSize_ = 35;
};

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_SUPERNODE_PARTITIONER_HPP
