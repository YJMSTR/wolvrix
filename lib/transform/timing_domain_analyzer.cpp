#include "transform/timing_domain_analyzer.hpp"
#include <sstream>
#include <queue>

namespace wolvrix::lib::transform
{

namespace
{

bool isConcreteTimingDomain(const std::string &domain)
{
    return !domain.empty() && domain != "combinational" && domain != "cross_domain" &&
           domain != "malformed";
}

} // namespace

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
    int latchDomainIndex = 0;

    for (const auto& opId : graph_.operations()) {
        auto op = graph_.getOperation(opId);

        // Handle register and memory write ports (event-key roots)
        if (op.kind() == grh::OperationKind::kRegisterWritePort ||
            op.kind() == grh::OperationKind::kMemoryWritePort) {
            EventKey key = extractEventKey(op);

            // Reject malformed sequential roots (register/memory MUST have eventEdge)
            if (key.eventEdge.empty()) {
                malformedOps_.insert(op.id());
                continue;
            }

            if (domains.find(key) == domains.end()) {
                domains[key] = generateDomainName(key, domainIndex++);
            }
        }
        // Handle latch write ports separately (non-event domain class)
        else if (op.kind() == grh::OperationKind::kLatchWritePort) {
            // Latches don't have eventEdge - create separate domain per latch symbol
            auto latchSymbolAttr = op.attr("latchSymbol");
            if (latchSymbolAttr) {
                if (const auto* latchSymbol = std::get_if<std::string>(&*latchSymbolAttr)) {
                    std::string latchDomain = "latch_" + *latchSymbol;
                    latchDomains_[op.id()] = latchDomain;
                }
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

        // Handle malformed operations
        if (malformedOps_.count(op.id()) > 0) {
            result[op.id()] = "malformed";
            continue;
        }

        // Handle latch write ports (non-event domain)
        if (op.kind() == grh::OperationKind::kLatchWritePort) {
            auto it = latchDomains_.find(op.id());
            if (it != latchDomains_.end()) {
                result[op.id()] = it->second;
                seqDomains[op.id()] = it->second;
            }
            continue;
        }

        // Handle register and memory write ports (event-key roots)
        if (op.kind() == grh::OperationKind::kRegisterWritePort ||
            op.kind() == grh::OperationKind::kMemoryWritePort) {
            EventKey key = extractEventKey(op);
            auto it = domainMap_.find(key);
            if (it != domainMap_.end()) {
                result[op.id()] = it->second;
                seqDomains[op.id()] = it->second;
            }
        }
    }

    // Second pass: propagate domains through combinational logic
    // Track which ops are reachable from each domain to detect sharing
    std::unordered_map<grh::OperationId, std::unordered_set<std::string>, grh::OperationIdHash> opDomainCandidates;

    for (const auto& [opId, domain] : seqDomains) {
        std::queue<grh::OperationId> queue;
        std::unordered_set<grh::OperationId, grh::OperationIdHash> visited;

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
                        opDomainCandidates[defOpId].insert(domain);
                        visited.insert(defOpId);
                        queue.push(defOpId);
                    }
                }
            }
        }
    }

    // Assign domains: single-domain ops get their domain, multi-domain ops are cross-domain
    for (const auto& [opId, domains] : opDomainCandidates) {
        if (domains.size() == 1) {
            result[opId] = *domains.begin();
        } else {
            // Shared combinational logic - mark as cross-domain
            result[opId] = "cross_domain";
        }
    }

    for (const auto& opId : graph_.operations()) {
        if (result.find(opId) == result.end()) {
            result[opId] = "combinational";
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
        auto srcIt = opToDomain_.find(op.id());
        if (srcIt == opToDomain_.end()) {
            continue;
        }
        const auto& srcDomain = srcIt->second;
        for (const auto& operand : op.operands()) {
            auto value = graph_.getValue(operand);
            auto defOpId = value.definingOp();
            if (defOpId.valid()) {
                auto dstIt = opToDomain_.find(defOpId);
                if (dstIt == opToDomain_.end()) {
                    continue;
                }
                const auto& dstDomain = dstIt->second;
                if (srcDomain != dstDomain &&
                    isConcreteTimingDomain(srcDomain) &&
                    isConcreteTimingDomain(dstDomain)) {
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
    if (!key.eventEdge.empty() &&
        (operands.size() <= eventSignalStart ||
         operands.size() - eventSignalStart < key.eventEdge.size())) {
        // Malformed write port: eventEdge requires more event signals than provided
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
