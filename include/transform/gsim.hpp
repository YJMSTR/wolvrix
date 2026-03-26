#ifndef WOLVRIX_TRANSFORM_GSIM_HPP
#define WOLVRIX_TRANSFORM_GSIM_HPP

#include "core/transform.hpp"

#include <string>

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
                           std::vector<int64_t> roots,
                           std::map<std::string, std::vector<int64_t>> groups,
                           std::vector<std::string> groupNames,
                           std::vector<int64_t> topo,
                           std::map<int64_t, std::string> classifications,
                           std::map<int64_t, std::vector<int64_t>> predecessors,
                           std::map<int64_t, std::vector<int64_t>> successors,
                           std::vector<std::string> opDescriptors,
                           int64_t opCount);

    private:
        GsimOptions options_;
    };

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_GSIM_HPP
