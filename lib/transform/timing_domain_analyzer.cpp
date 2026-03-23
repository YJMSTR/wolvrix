#include "transform/timing_domain_analyzer.hpp"
#include <sstream>

namespace wolvrix::lib::transform
{

bool EventKey::operator==(const EventKey& other) const {
    return eventEdge == other.eventEdge && eventSignals == other.eventSignals;
}

size_t EventKey::hash() const {
    size_t seed = 0;
    for (const auto& edge : eventEdge) {
        seed ^= static_cast<size_t>(edge) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
    for (const auto& signal : eventSignals) {
        seed ^= signal.index + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
    return seed;
}

TimingDomainAnalyzer::TimingDomainAnalyzer(const grh::Graph& graph)
    : graph_(graph) {}

std::unordered_map<EventKey, std::string, EventKeyHash> TimingDomainAnalyzer::analyzeTimingDomains() {
    std::unordered_map<EventKey, std::string, EventKeyHash> domains;
    int domainIndex = 0;

    for (const auto& op : graph_.operations()) {
        if (op.kind == grh::OperationKind::kRegisterWritePort ||
            op.kind == grh::OperationKind::kLatchWritePort) {
            EventKey key = extractEventKey(op);
            if (domains.find(key) == domains.end()) {
                domains[key] = generateDomainName(key, domainIndex++);
            }
        }
    }

    domainMap_ = domains;
    return domains;
}

std::unordered_map<grh::OperationId, std::string, grh::OperationIdHash>
TimingDomainAnalyzer::assignTimingDomains() {
    if (domainMap_.empty()) {
        analyzeTimingDomains();
    }

    std::unordered_map<grh::OperationId, std::string, grh::OperationIdHash> result;

    for (const auto& op : graph_.operations()) {
        if (op.kind == grh::OperationKind::kRegisterWritePort ||
            op.kind == grh::OperationKind::kLatchWritePort) {
            EventKey key = extractEventKey(op);
            auto it = domainMap_.find(key);
            if (it != domainMap_.end()) {
                result[op.id] = it->second;
            }
        } else {
            result[op.id] = "comb";
        }
    }

    opToDomain_ = result;
    return result;
}

std::vector<std::pair<grh::OperationId, grh::OperationId>>
TimingDomainAnalyzer::findCrossDomainEdges() {
    if (opToDomain_.empty()) {
        assignTimingDomains();
    }

    std::vector<std::pair<grh::OperationId, grh::OperationId>> result;

    for (const auto& op : graph_.operations()) {
        auto srcDomain = opToDomain_[op.id];
        for (const auto& operand : op.operands) {
            auto defOp = graph_.definingOp(operand);
            if (defOp.valid()) {
                auto dstDomain = opToDomain_[defOp];
                if (srcDomain != dstDomain) {
                    result.push_back({defOp, op.id});
                }
            }
        }
    }

    return result;
}

EventKey TimingDomainAnalyzer::extractEventKey(const grh::Operation& op) const {
    EventKey key;
    // Simplified implementation - extract event edges and signals
    // In real implementation, would parse eventEdge and eventSignals attributes
    return key;
}

std::string TimingDomainAnalyzer::generateDomainName(const EventKey& key, int index) {
    std::ostringstream oss;
    oss << "domain_" << index;
    return oss.str();
}

} // namespace wolvrix::lib::transform
