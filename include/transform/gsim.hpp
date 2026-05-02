#ifndef WOLVRIX_TRANSFORM_GSIM_HPP
#define WOLVRIX_TRANSFORM_GSIM_HPP

#include "core/transform.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace wolvrix::lib::transform
{

    struct GsimOptions
    {
        std::string path;
    };

    class GsimPass : public Pass
    {
    public:
        GsimPass();
        explicit GsimPass(GsimOptions options);

        PassResult run() override;

    protected:
        bool validateGraph(const wolvrix::lib::grh::Graph &graph);
        void writeMetadata(const wolvrix::lib::grh::Graph &graph,
                           std::string_view scratchpadNamespace,
                           std::vector<int64_t> roots,
                           std::map<std::string, std::vector<int64_t>> groups,
                           std::vector<std::string> groupNames,
                           std::vector<std::string> scheduleActivityOrder,
                           std::map<std::string, std::vector<int64_t>> scheduleActivityMembers,
                           std::map<std::string, std::string> scheduleActivityClasses,
                           int64_t scheduleBatchCount,
                           std::vector<std::string> scheduleBatchNames,
                           std::vector<int64_t> scheduleBatchClassIds,
                           std::vector<std::string> scheduleBatchClassNames,
                           std::vector<int64_t> scheduleBatchFlags,
                           std::vector<int64_t> scheduleBatchTopoByPos,
                           std::vector<int64_t> scheduleBatchFirstTopoPos,
                           std::vector<int64_t> scheduleBatchLastTopoPos,
                           std::vector<int64_t> scheduleBatchOpCounts,
                           std::vector<int64_t> scheduleBatchSuccOffsets,
                           std::vector<int64_t> scheduleBatchSuccTargets,
                           std::vector<int64_t> scheduleBatchEntryBatches,
                           std::vector<int64_t> scheduleBatchEstimatedLines,
                           std::vector<std::string> hypergraphNodeNames,
                           std::map<std::string, std::vector<int64_t>> hypergraphNodeMembers,
                           std::vector<std::string> hypergraphEdgeNames,
                           std::map<std::string, std::string> hypergraphEdgeSources,
                           std::map<std::string, std::string> hypergraphEdgeTargets,
                           std::map<std::string, std::vector<int64_t>> hypergraphEdgeSinks,
                           std::vector<int64_t> topo,
                           std::map<int64_t, std::string> classifications,
                           std::map<int64_t, std::vector<int64_t>> predecessors,
                           std::map<int64_t, std::vector<int64_t>> successors,
                           std::vector<std::string> opDescriptors,
                           int64_t opCount,
                           int64_t graphRevision);

    private:
        GsimOptions options_;
    };

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_GSIM_HPP
