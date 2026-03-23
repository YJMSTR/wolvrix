#ifndef WOLVRIX_TRANSFORM_TIMING_DOMAIN_ANALYZER_HPP
#define WOLVRIX_TRANSFORM_TIMING_DOMAIN_ANALYZER_HPP

#include "core/transform.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace wolvrix::lib::transform
{

struct EventKey {
    std::vector<grh::EdgeType> eventEdge;
    std::vector<grh::ValueId> eventSignals;

    bool operator==(const EventKey& other) const;
    size_t hash() const;
};

struct EventKeyHash {
    size_t operator()(const EventKey& key) const {
        return key.hash();
    }
};

class TimingDomainAnalyzer {
public:
    explicit TimingDomainAnalyzer(const grh::Graph& graph);

    // Analyze timing domains in the design
    std::unordered_map<EventKey, std::string, EventKeyHash> analyzeTimingDomains();

    // Assign timing domain to each operation
    std::unordered_map<grh::OperationId, std::string, grh::OperationIdHash> assignTimingDomains();

    // Find cross-domain edges
    std::vector<std::pair<grh::OperationId, grh::OperationId>> findCrossDomainEdges();

private:
    const grh::Graph& graph_;
    std::unordered_map<grh::OperationId, std::string, grh::OperationIdHash> opToDomain_;
    std::unordered_map<EventKey, std::string, EventKeyHash> domainMap_;

    EventKey extractEventKey(const grh::Operation& op) const;
    std::string generateDomainName(const EventKey& key, int index);
};

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_TIMING_DOMAIN_ANALYZER_HPP
