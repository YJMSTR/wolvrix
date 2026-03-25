#include "transform/supernode_control.hpp"

#include <algorithm>
#include <sstream>

#include "slang/numeric/SVInt.h"

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

    auto literal = constantLiteral(defOp);
    literal.erase(std::remove_if(literal.begin(), literal.end(), [](unsigned char ch) {
        return std::isspace(ch) || ch == '_';
    }), literal.end());
    std::transform(literal.begin(), literal.end(), literal.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (literal == "1")
    {
        return true;
    }
    try
    {
        auto parsed = slang::SVInt::fromString(literal);
        if (parsed.hasUnknown())
        {
            return false;
        }
        return bool(parsed == slang::SVInt(parsed.getBitWidth(), uint64_t(1), parsed.isSigned()));
    }
    catch (const std::exception &)
    {
        return false;
    }
}

} // namespace

std::string structuralValueSignature(const grh::Graph &graph,
                                     grh::ValueId valueId,
                                     size_t depth)
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
        oss << structuralValueSignature(graph, operand, depth + 1);
    }
    oss << ")";
    return oss.str();
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

std::string sequentialControlSignature(const grh::Graph &graph,
                                       const grh::Operation &op)
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
        oss << structuralValueSignature(graph, operands[index]);
    }
    oss << "|events:";
    for (size_t index = eventStart; index < operands.size(); ++index)
    {
        if (index != eventStart)
        {
            oss << ";";
        }
        oss << structuralValueSignature(graph, operands[index]);
    }
    return oss.str();
}

std::vector<std::string> nodeControlSignatures(const grh::Graph &graph,
                                               const SuperNode &node)
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
    signatures.erase(std::unique(signatures.begin(), signatures.end()), signatures.end());
    return signatures;
}

bool haveCompatibleSequentialControl(const grh::Graph &graph,
                                     const SuperNode &lhs,
                                     const SuperNode &rhs)
{
    const auto lhsControl = nodeControlSignatures(graph, lhs);
    const auto rhsControl = nodeControlSignatures(graph, rhs);
    if (lhsControl.empty() || rhsControl.empty())
    {
        return true;
    }
    return lhsControl == rhsControl;
}

} // namespace wolvrix::lib::transform
