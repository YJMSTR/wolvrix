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
        std::hash<std::string> hasher;
        seed ^= hasher(edge) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
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

    for (const auto& opId : graph_.operations()) {
        auto op = graph_.getOperation(opId);
        if (op.kind() == grh::OperationKind::kRegisterWritePort ||
            op.kind() == grh::OperationKind::kLatchWritePort ||
            op.kind() == grh::OperationKind::kMemoryWritePort) {
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

    for (const auto& opId : graph_.operations()) {
        auto op = graph_.getOperation(opId);
        if (op.kind() == grh::OperationKind::kRegisterWritePort ||
            op.kind() == grh::OperationKind::kLatchWritePort ||
            op.kind() == grh::OperationKind::kMemoryWritePort) {
            EventKey key = extractEventKey(op);
            auto it = domainMap_.find(key);
            if (it != domainMap_.end()) {
                result[op.id()] = it->second;
            }
        } else {
            result[op.id()] = "comb";
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

    for (const auto& opId : graph_.operations()) {
        auto op = graph_.getOperation(opId);
        auto srcDomain = opToDomain_[op.id()];
        for (const auto& operand : op.operands()) {
            auto value = graph_.getValue(operand);
            auto defOpId = value.definingOp();
            if (defOpId.valid()) {
                auto dstDomain = opToDomain_[defOpId];
                if (srcDomain != dstDomain) {
                    result.push_back({defOpId, op.id()});
                }
            }
        }
    }

    return result;
}

EventKey TimingDomainAnalyzer::extractEventKey(const grh::Operation& op) const {
    EventKey key;

    // Parse eventEdge attribute as vector<string>
    auto eventEdgeAttr = op.attr("eventEdge");
    if (eventEdgeAttr) {
        if (const auto* vec = std::get_if<std::vector<std::string>>(&*eventEdgeAttr)) {
            key.eventEdge = *vec;
        }
    }

    // Extract event signal operands based on write port type
    // For register/latch write ports: operands are [updateCond, data, ...event signals...]
    // For memory write ports: operands are [updateCond, data, mask, address, ...event signals...]
    auto operands = op.operands();
    size_t eventSignalStart = 2; // Default for register/latch
    if (op.kind() == grh::OperationKind::kMemoryWritePort) {
        eventSignalStart = 4; // Skip updateCond, data, mask, address
    }

    for (size_t i = eventSignalStart; i < operands.size() && i - eventSignalStart < key.eventEdge.size(); ++i) {
        key.eventSignals.push_back(operands[i]);
    }

    return key;
}

std::string TimingDomainAnalyzer::generateDomainName(const EventKey& key, int index) {
    std::ostringstream oss;
    oss << "domain_" << index;
    return oss.str();
}

} // namespace wolvrix::lib::transform
