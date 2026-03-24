#ifndef WOLVRIX_TRANSFORM_SUPERNODE_CONTROL_HPP
#define WOLVRIX_TRANSFORM_SUPERNODE_CONTROL_HPP

#include "core/grh.hpp"
#include "transform/supernode_graph.hpp"

#include <string>
#include <vector>

namespace wolvrix::lib::transform
{

std::string structuralValueSignature(const grh::Graph &graph,
                                     grh::ValueId valueId,
                                     size_t depth = 0);

bool hasResetLikeControl(const grh::Graph &graph, const grh::Operation &op);

std::string sequentialControlSignature(const grh::Graph &graph,
                                       const grh::Operation &op);

std::vector<std::string> nodeControlSignatures(const grh::Graph &graph,
                                               const SuperNode &node);

bool haveCompatibleSequentialControl(const grh::Graph &graph,
                                     const SuperNode &lhs,
                                     const SuperNode &rhs);

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_SUPERNODE_CONTROL_HPP
