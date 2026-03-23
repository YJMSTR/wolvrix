#ifndef WOLVRIX_TRANSFORM_TIMING_DOMAIN_ANALYZER_HPP
#define WOLVRIX_TRANSFORM_TIMING_DOMAIN_ANALYZER_HPP

#include "core/transform.hpp"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wolvrix::lib::transform
{

struct EventKey {
    std::vector<std::string> eventEdge;  // Edge polarity: "posedge", "negedge", etc.
    std::vector<wolvrix::lib::grh::ValueId> eventSignals;

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
    explicit TimingDomainAnalyzer(const wolvrix::lib::grh::Graph& graph);

    // Analyze timing domains in the design
    std::unordered_map<EventKey, std::string, EventKeyHash> analyzeTimingDomains();

    // Assign timing domain to each operation
    std::unordered_map<wolvrix::lib::grh::OperationId, std::string, wolvrix::lib::grh::OperationIdHash> assignTimingDomains();

    // Find cross-domain edges
    std::vector<std::pair<wolvrix::lib::grh::OperationId, wolvrix::lib::grh::OperationId>> findCrossDomainEdges();

private:
    const wolvrix::lib::grh::Graph& graph_;
    std::unordered_map<wolvrix::lib::grh::OperationId, std::string, wolvrix::lib::grh::OperationIdHash> opToDomain_;
    std::unordered_map<EventKey, std::string, EventKeyHash> domainMap_;
    std::unordered_map<wolvrix::lib::grh::OperationId, std::string, wolvrix::lib::grh::OperationIdHash> latchDomains_;
    std::unordered_set<wolvrix::lib::grh::OperationId, wolvrix::lib::grh::OperationIdHash> malformedOps_;

    EventKey extractEventKey(const wolvrix::lib::grh::Operation& op) const;
    std::string generateDomainName(const EventKey& key, int index);
};

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_TIMING_DOMAIN_ANALYZER_HPP
