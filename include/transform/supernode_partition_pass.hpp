#ifndef WOLVRIX_TRANSFORM_SUPERNODE_PARTITION_PASS_HPP
#define WOLVRIX_TRANSFORM_SUPERNODE_PARTITION_PASS_HPP

#include "core/transform.hpp"

namespace wolvrix::lib::transform
{

class SuperNodePartitionPass : public Pass {
public:
    SuperNodePartitionPass();

    PassResult run() override;

private:
    size_t maxSuperNodeSize_ = 35;
};

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_SUPERNODE_PARTITION_PASS_HPP
