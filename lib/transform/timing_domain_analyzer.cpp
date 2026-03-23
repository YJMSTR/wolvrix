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
    std::unordered_map<grh::OperationId, std::string, grh::OperationIdHash> seqDomains;

    // First pass: assign domains to sequential operations
    for (const auto& opId : graph_.operations()) {
        auto op = graph_.getOperation(opId);
        if (op.kind() == grh::OperationKind::kRegisterWritePort ||
            op.kind() == grh::OperationKind::kLatchWritePort ||
            op.kind() == grh::OperationKind::kMemoryWritePort) {
            EventKey key = extractEventKey(op);

            // Check for malformed write port (missing event data)
            if (key.eventEdge.empty() && key.eventSignals.empty()) {
                auto eventEdgeAttr = op.attr("eventEdge");
                if (eventEdgeAttr && !std::get_if<std::vector<std::string>>(&*eventEdgeAttr)->empty()) {
                    // Has eventEdge attribute but failed to extract - malformed
                    result[op.id()] = "malformed";
                    continue;
                }
            }

            auto it = domainMap_.find(key);
            if (it != domainMap_.end()) {
                result[op.id()] = it->second;
                seqDomains[op.id()] = it->second;
            }
        }
    }

    // Second pass: propagate domains through combinational logic
    // Use BFS to propagate from sequential roots
    std::unordered_set<grh::OperationId, grh::OperationIdHash> visited;
    for (const auto& [opId, domain] : seqDomains) {
        std::queue<grh::OperationId> queue;
        queue.push(opId);
        visited.insert(opId);

        while (!queue.empty()) {
            auto currentId = queue.front();
            queue.pop();
            auto current = graph_.getOperation(currentId);

            // Propagate domain to operands (backward through combinational cone)
            for (const auto& operand : current.operands()) {
                auto value = graph_.getValue(operand);
                auto defOpId = value.definingOp();
                if (defOpId.valid() && visited.find(defOpId) == visited.end()) {
                    auto defOp = graph_.getOperation(defOpId);
                    // Only propagate to combinational operations
                    if (defOp.kind() != grh::OperationKind::kRegisterWritePort &&
                        defOp.kind() != grh::OperationKind::kLatchWritePort &&
                        defOp.kind() != grh::OperationKind::kMemoryWritePort) {
                        result[defOpId] = domain;
                        visited.insert(defOpId);
                        queue.push(defOpId);
                    }
                }
            }
        }
    }

    // Third pass: assign remaining operations to "comb" domain
    for (const auto& opId : graph_.operations()) {
        if (result.find(opId) == result.end()) {
            result[opId] = "comb";
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
    // Register write ports: [updateCond, nextValue, maskValue, ...event signals] (events start at index 3)
    // Memory write ports: [updateCond, address, data, mask, ...event signals] (events start at index 4)
    auto operands = op.operands();
    size_t eventSignalStart = 3; // Default for register/latch (after updateCond, nextValue, maskValue)

    if (op.kind() == grh::OperationKind::kMemoryWritePort) {
        eventSignalStart = 4; // After updateCond, address, data, mask
    } else if (op.kind() == grh::OperationKind::kLatchWritePort) {
        eventSignalStart = 3; // After updateCond, nextValue, maskValue
    }

    // Validate that we have event signals if eventEdge is present
    if (!key.eventEdge.empty() && operands.size() <= eventSignalStart) {
        // Malformed write port: has eventEdge but no event signal operands
        // Mark as special domain for error reporting
        key.eventEdge.clear();
        key.eventSignals.clear();
        return key;
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
