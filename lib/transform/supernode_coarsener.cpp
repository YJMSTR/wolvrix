#include "transform/supernode_coarsener.hpp"
#include <algorithm>
#include <sstream>
#include <unordered_map>

namespace wolvrix::lib::transform
{

namespace
{

std::string attributeString(const grh::Operation &op, std::string_view key)
{
    const auto attr = op.attr(key);
    if (!attr)
    {
        return {};
    }
    if (const auto *text = std::get_if<std::string>(&*attr))
    {
        return *text;
    }
    return {};
}

std::string constantLiteral(const grh::Operation &op)
{
    const auto attr = op.attr("constValue");
    if (!attr)
    {
        return {};
    }
    if (const auto *literal = std::get_if<std::string>(&*attr))
    {
        return *literal;
    }
    return {};
}

std::string valueSignature(const grh::Graph &graph, grh::ValueId valueId, size_t depth = 0)
{
    if (!valueId.valid())
    {
        return "invalid";
    }
    if (depth > 8)
    {
        return "depth_limit";
    }

    const auto value = graph.getValue(valueId);
    const auto defOpId = value.definingOp();
    if (!defOpId.valid())
    {
        return "value:" + std::string(value.symbolText());
    }

    const auto defOp = graph.getOperation(defOpId);
    std::ostringstream oss;
    oss << "op:" << grh::toString(defOp.kind());

    if (defOp.kind() == grh::OperationKind::kConstant)
    {
        oss << "[" << constantLiteral(defOp) << "]";
    }
    else if (defOp.kind() == grh::OperationKind::kRegisterReadPort)
    {
        oss << "[" << attributeString(defOp, "regSymbol") << "]";
    }
    else if (defOp.kind() == grh::OperationKind::kLatchReadPort)
    {
        oss << "[" << attributeString(defOp, "latchSymbol") << "]";
    }

    oss << "(";
    bool first = true;
    for (const auto operand : defOp.operands())
    {
        if (!first)
        {
            oss << ",";
        }
        first = false;
        oss << valueSignature(graph, operand, depth + 1);
    }
    oss << ")";
    return oss.str();
}

bool isConstOne(const grh::Graph &graph, grh::ValueId valueId)
{
    if (!valueId.valid())
    {
        return false;
    }
    const auto value = graph.getValue(valueId);
    const auto defOpId = value.definingOp();
    if (!defOpId.valid())
    {
        return false;
    }
    const auto defOp = graph.getOperation(defOpId);
    if (defOp.kind() != grh::OperationKind::kConstant)
    {
        return false;
    }

    const auto literal = constantLiteral(defOp);
    return literal == "1'b1" || literal == "1'h1" || literal == "1";
}

bool hasResetLikeControl(const grh::Graph &graph, const grh::Operation &op)
{
    const auto eventEdgeAttr = op.attr("eventEdge");
    if (eventEdgeAttr)
    {
        if (const auto *edges = std::get_if<std::vector<std::string>>(&*eventEdgeAttr))
        {
            if (edges->size() > 1)
            {
                return true;
            }
        }
    }

    const auto operands = op.operands();
    if (operands.size() >= 2)
    {
        if (!isConstOne(graph, operands[0]))
        {
            return true;
        }
        const auto nextValue = graph.getValue(operands[1]);
        const auto defOpId = nextValue.definingOp();
        if (defOpId.valid() && graph.getOperation(defOpId).kind() == grh::OperationKind::kMux)
        {
            return true;
        }
    }

    return false;
}

std::string sequentialControlSignature(const grh::Graph &graph, const grh::Operation &op);

std::vector<std::string> nodeControlSignatures(const grh::Graph &graph, const SuperNode &node)
{
    std::vector<std::string> signatures;
    for (const auto opId : node.members)
    {
        const auto op = graph.getOperation(opId);
        if (op.kind() == grh::OperationKind::kRegisterWritePort ||
            op.kind() == grh::OperationKind::kMemoryWritePort ||
            op.kind() == grh::OperationKind::kLatchWritePort)
        {
            if (hasResetLikeControl(graph, op))
            {
                signatures.push_back(sequentialControlSignature(graph, op));
            }
        }
    }
    std::sort(signatures.begin(), signatures.end());
    return signatures;
}

std::string sequentialControlSignature(const grh::Graph &graph, const grh::Operation &op)
{
    std::ostringstream oss;
    oss << grh::toString(op.kind()) << "|";

    const auto eventEdgeAttr = op.attr("eventEdge");
    if (eventEdgeAttr)
    {
        if (const auto *edges = std::get_if<std::vector<std::string>>(&*eventEdgeAttr))
        {
            oss << "edges:";
            for (const auto &edge : *edges)
            {
                oss << edge << ",";
            }
        }
    }

    const auto operands = op.operands();
    const size_t eventStart = op.kind() == grh::OperationKind::kMemoryWritePort ? 4 : 3;
    oss << "|ctrl:";
    for (size_t index = 0; index < std::min(eventStart, operands.size()); ++index)
    {
        if (index != 0)
        {
            oss << ";";
        }
        oss << valueSignature(graph, operands[index]);
    }
    oss << "|events:";
    for (size_t index = eventStart; index < operands.size(); ++index)
    {
        if (index != eventStart)
        {
            oss << ";";
        }
        oss << valueSignature(graph, operands[index]);
    }
    return oss.str();
}

} // namespace

SuperNodeCoarsener::SuperNodeCoarsener(SuperNodeGraph& sg, const grh::Graph& graph)
    : sg_(sg), graph_(graph) {}

void SuperNodeCoarsener::coarsen() {
    bool changed = true;
    while (changed) {
        changed = false;
        changed |= mergeResetAll();
        changed |= mergeWhenNodes();
        if (changed) {
            resort();
        }
        changed |= mergeOut1();
        changed |= mergeIn1();
        changed |= mergeSublings();
    }
}

bool SuperNodeCoarsener::mergeResetAll() {
    bool changed = false;

    std::unordered_map<std::string, std::vector<SuperNodeId>> resetGroups;

    for (const auto& snId : sg_.validNodeIds()) {
        const auto& node = sg_.getNode(snId);
        std::vector<std::string> signatures;

        for (const auto& opId : node.members) {
            auto op = graph_.getOperation(opId);
            if (op.kind() == wolvrix::lib::grh::OperationKind::kRegisterWritePort ||
                op.kind() == wolvrix::lib::grh::OperationKind::kMemoryWritePort ||
                op.kind() == wolvrix::lib::grh::OperationKind::kLatchWritePort) {
                if (hasResetLikeControl(graph_, op)) {
                    signatures.push_back(sequentialControlSignature(graph_, op));
                }
            }
        }

        if (!signatures.empty()) {
            std::sort(signatures.begin(), signatures.end());
            std::ostringstream key;
            for (size_t i = 0; i < signatures.size(); ++i) {
                if (i != 0) {
                    key << "||";
                }
                key << signatures[i];
            }
            resetGroups[key.str()].push_back(snId);
        }
    }

    for (const auto& [sig, nodes] : resetGroups) {
        if (nodes.size() > 1) {
            SuperNodeId target = nodes[0];
            for (size_t i = 1; i < nodes.size(); i++) {
                if (canMerge(target, nodes[i])) {
                    doMerge(target, nodes[i]);
                    changed = true;
                }
            }
        }
    }
    return changed;
}

bool SuperNodeCoarsener::mergeWhenNodes() {
    bool changed = false;

    // Group nodes by their kMux condition operands
    // Key = condition operand's defining operation ID
    std::unordered_map<std::string, std::vector<SuperNodeId>> condGroups;

    for (const auto& snId : sg_.validNodeIds()) {
        const auto& node = sg_.getNode(snId);

        // Check if this supernode contains kMux operations
        std::string condSig;
        for (const auto& opId : node.members) {
            auto op = graph_.getOperation(opId);
            if (op.kind() == wolvrix::lib::grh::OperationKind::kMux) {
                auto operands = op.operands();
                if (operands.size() >= 3) {
                    condSig = valueSignature(graph_, operands[0]);
                    break;
                }
            }
        }

        if (!condSig.empty()) {
            condGroups[condSig].push_back(snId);
        }
    }

    // Merge nodes with identical kMux conditions
    for (const auto& [sig, nodes] : condGroups) {
        if (nodes.size() > 1) {
            SuperNodeId target = nodes[0];
            for (size_t i = 1; i < nodes.size(); i++) {
                if (canMerge(target, nodes[i]) && haveSamePredecessors(target, nodes[i])) {
                    doMerge(target, nodes[i]);
                    changed = true;
                }
            }
        }
    }
    return changed;
}

bool SuperNodeCoarsener::mergeOut1() {
    bool changed = false;
    std::vector<SuperNodeId> toMerge;
    for (const auto& snId : sg_.validNodeIds()) {
        if (sg_.successors(snId).size() == 1) {
            auto succId = *sg_.successors(snId).begin();
            if (canMerge(snId, succId)) {
                toMerge.push_back(snId);
            }
        }
    }
    for (auto id : toMerge) {
        if (sg_.isValid(id) && sg_.successors(id).size() == 1) {
            auto succId = *sg_.successors(id).begin();
            if (sg_.isValid(succId)) {
                doMerge(succId, id);
                changed = true;
            }
        }
    }
    return changed;
}

bool SuperNodeCoarsener::mergeIn1() {
    bool changed = false;
    std::vector<SuperNodeId> toMerge;
    for (const auto& snId : sg_.validNodeIds()) {
        if (sg_.predecessors(snId).size() == 1) {
            auto predId = *sg_.predecessors(snId).begin();
            if (canMerge(predId, snId)) {
                toMerge.push_back(snId);
            }
        }
    }
    for (auto id : toMerge) {
        if (sg_.isValid(id) && sg_.predecessors(id).size() == 1) {
            auto predId = *sg_.predecessors(id).begin();
            if (sg_.isValid(predId)) {
                doMerge(predId, id);
                changed = true;
            }
        }
    }
    return changed;
}

bool SuperNodeCoarsener::mergeSublings() {
    bool changed = false;
    std::unordered_map<uint64_t, std::vector<SuperNodeId>> groups;
    for (const auto& snId : sg_.validNodeIds()) {
        uint64_t hash = computeDeterministicHash(snId);
        groups[hash].push_back(snId);
    }
    for (const auto& [hash, nodes] : groups) {
        if (nodes.size() > 1) {
            for (size_t i = 1; i < nodes.size(); i++) {
                if (sg_.isValid(nodes[0]) && sg_.isValid(nodes[i]) &&
                    canMerge(nodes[0], nodes[i]) && haveSamePredecessors(nodes[0], nodes[i])) {
                    doMerge(nodes[0], nodes[i]);
                    changed = true;
                }
            }
        }
    }
    return changed;
}

void SuperNodeCoarsener::resort() {
    auto sorted = sg_.topologicalSort();
    for (size_t i = 0; i < sorted.size(); i++) {
        sg_.getNode(sorted[i]).topologicalOrder = i;
    }
}

bool SuperNodeCoarsener::canMerge(SuperNodeId snId1, SuperNodeId snId2) const {
    if (!sg_.isValid(snId1) || !sg_.isValid(snId2)) {
        return false;
    }
    const auto& node1 = sg_.getNode(snId1);
    const auto& node2 = sg_.getNode(snId2);
    if (node1.memberCount() + node2.memberCount() > maxSuperNodeSize_) {
        return false;
    }
    if (node1.timingDomain != node2.timingDomain) {
        return false;
    }
    const auto node1Control = nodeControlSignatures(graph_, node1);
    const auto node2Control = nodeControlSignatures(graph_, node2);
    if (!node1Control.empty() || !node2Control.empty()) {
        if (node1Control != node2Control) {
            return false;
        }
    }
    return true;
}

void SuperNodeCoarsener::doMerge(SuperNodeId targetId, SuperNodeId sourceId) {
    sg_.merge(targetId, sourceId);
}

uint64_t SuperNodeCoarsener::computeHash(SuperNodeId snId) const {
    uint64_t hash = 0;
    const auto& preds = sg_.predecessors(snId);
    for (auto predId : preds) {
        hash ^= predId + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    }
    return hash;
}

uint64_t SuperNodeCoarsener::computeDeterministicHash(SuperNodeId snId) const {
    // Sort predecessors for deterministic hashing
    std::vector<SuperNodeId> sortedPreds(sg_.predecessors(snId).begin(), sg_.predecessors(snId).end());
    std::sort(sortedPreds.begin(), sortedPreds.end());

    uint64_t hash = 0;
    for (auto predId : sortedPreds) {
        hash ^= predId + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    }
    return hash;
}

bool SuperNodeCoarsener::haveSamePredecessors(SuperNodeId snId1, SuperNodeId snId2) const {
    const auto& preds1 = sg_.predecessors(snId1);
    const auto& preds2 = sg_.predecessors(snId2);
    return preds1 == preds2;
}

} // namespace wolvrix::lib::transform
