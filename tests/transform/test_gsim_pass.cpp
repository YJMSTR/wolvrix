#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/gsim.hpp"

#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace wolvrix::lib;
using namespace wolvrix::lib::transform;

namespace
{

int fail(const std::string &message)
{
    std::cerr << "[transform-gsim] " << message << '\n';
    return 1;
}

void expect(bool condition, const std::string &message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

grh::ValueId makeValue(grh::Graph &graph,
                       const std::string &name,
                       int32_t width = 1,
                       bool isSigned = false)
{
    return graph.createValue(graph.internSymbol(name), width, isSigned);
}

grh::ValueId makeConstant(grh::Graph &graph,
                          const std::string &valueName,
                          const std::string &opName,
                          int32_t width,
                          const std::string &literal)
{
    const auto value = graph.createValue(graph.internSymbol(valueName), width, false);
    const auto op = graph.createOperation(grh::OperationKind::kConstant,
                                          graph.internSymbol(opName));
    graph.addResult(op, value);
    graph.setAttr(op, "value", literal);
    graph.setAttr(op, "width", static_cast<int64_t>(width));
    graph.setAttr(op, "isSigned", false);
    return value;
}

grh::OperationId makeRegisterWrite(grh::Graph &graph,
                                   const std::string &opName,
                                   grh::ValueId updateCond,
                                   grh::ValueId nextValue,
                                   grh::ValueId maskValue,
                                   grh::ValueId clk,
                                   const std::string &regSymbol)
{
    const auto op = graph.createOperation(grh::OperationKind::kRegisterWritePort,
                                          graph.internSymbol(opName));
    graph.addOperand(op, updateCond);
    graph.addOperand(op, nextValue);
    graph.addOperand(op, maskValue);
    graph.addOperand(op, clk);
    graph.setAttr(op, "regSymbol", regSymbol);
    graph.setAttr(op, "clockSymbol", std::string("clk"));
    graph.setAttr(op, "eventEdge", std::vector<std::string>{"posedge"});
    return op;
}

std::optional<std::string> getAttrString(const grh::Operation &op,
                                         std::string_view key)
{
    auto attr = op.attr(key);
    if (!attr)
    {
        return std::nullopt;
    }
    if (const auto *value = std::get_if<std::string>(&*attr))
    {
        return *value;
    }
    return std::nullopt;
}

void testPassRegistration()
{
    const auto names = availableTransformPasses();
    bool sawGsim = false;
    for (const auto &name : names)
    {
        if (name == "gsim")
        {
            sawGsim = true;
            break;
        }
    }
    expect(sawGsim, "availableTransformPasses should include gsim");

    std::string error;
    auto pass = makePass("gsim", {}, error);
    expect(static_cast<bool>(pass), "makePass(gsim) should succeed without options");
    expect(error.empty(), "makePass(gsim) should not populate an error");
    expect(pass->id() == "gsim", "registered pass id should be gsim");
}

void testGraphOnlyPathWritesMetadata()
{
    grh::Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto inA = makeValue(graph, "a", 8, false);
    const auto inB = makeValue(graph, "b", 8, false);
    const auto clk = makeValue(graph, "clk", 1, false);
    graph.bindInputPort("a", inA);
    graph.bindInputPort("b", inB);
    graph.bindInputPort("clk", clk);

    const auto addOut = makeValue(graph, "sum", 8, false);
    const auto add = graph.createOperation(grh::OperationKind::kAdd, graph.internSymbol("add"));
    graph.addOperand(add, inA);
    graph.addOperand(add, inB);
    graph.addResult(add, addOut);

    const auto one = makeConstant(graph, "one", "one_const", 1, "1'b1");
    const auto mask = makeConstant(graph, "mask", "mask_const", 8, "8'hff");
    const auto write = makeRegisterWrite(graph, "reg_write", one, addOut, mask, clk, "state");

    const auto dbg = graph.createOperation(grh::OperationKind::kSystemTask, graph.internSymbol("display"));
    graph.addOperand(dbg, addOut);

    PassManager manager;
    manager.addPass(std::make_unique<GsimPass>(GsimOptions{"top"}));
    PassDiagnostics diags;
    const auto result = manager.run(design, diags);

    expect(result.success, "gsim graph-only run should succeed");
    expect(!result.changed, "gsim pass should be scratchpad-only");
    expect(!diags.hasError(), "gsim graph-only run should not emit errors");

    const auto *roots = design.getScratchpad<std::vector<int64_t>>("gsim.top.roots");
    const auto *groups = design.getScratchpad<std::map<std::string, std::vector<int64_t>>>("gsim.top.event_groups");
    const auto *topo = design.getScratchpad<std::vector<int64_t>>("gsim.top.topology.order");
    const auto *classifications = design.getScratchpad<std::map<int64_t, std::string>>("gsim.top.ops.classification");
    const auto *scheduleKind = design.getScratchpad<std::string>("gsim.top.schedule.kind");
    const auto *hyperKind = design.getScratchpad<std::string>("gsim.top.hypergraph.kind");

    expect(roots != nullptr, "gsim should write roots metadata");
    expect(groups != nullptr, "gsim should write event group metadata");
    expect(topo != nullptr, "gsim should write topology metadata");
    expect(classifications != nullptr, "gsim should write op classifications");
    expect(scheduleKind != nullptr && *scheduleKind == "placeholder",
           "gsim should write schedule placeholder metadata");
    expect(hyperKind != nullptr && *hyperKind == "placeholder",
           "gsim should write hypergraph placeholder metadata");

    expect(!roots->empty(), "gsim roots metadata should not be empty");
    expect(topo->size() == 5, "fixture should produce stable topo order for all ops");
    expect(groups->count("combinational") == 1, "gsim should group logic ops deterministically");
    expect(groups->count("reg:clk") == 1, "gsim should group register event roots by clock symbol");
    expect(groups->count("system-task") == 1, "gsim should preserve system tasks in metadata");
    expect(classifications->at(static_cast<int64_t>(dbg.index)) == "system-task",
           "system task should be classified deterministically");
    expect(classifications->at(static_cast<int64_t>(write.index)) == "stateful",
           "register write should be classified as stateful");
}

void testMultiHopInstancePathUsesSharedResolver()
{
    grh::Design design;
    auto &leaf = design.createGraph("leaf");
    auto &mid = design.createGraph("mid");
    auto &top = design.createGraph("top");
    design.markAsTop("top");

    const auto leafA = makeValue(leaf, "a", 8, false);
    const auto leafB = makeValue(leaf, "b", 8, false);
    const auto leafClk = makeValue(leaf, "clk", 1, false);
    leaf.bindInputPort("a", leafA);
    leaf.bindInputPort("b", leafB);
    leaf.bindInputPort("clk", leafClk);
    const auto leafSum = makeValue(leaf, "sum", 8, false);
    const auto leafAdd = leaf.createOperation(grh::OperationKind::kAdd, leaf.internSymbol("leaf_add"));
    leaf.addOperand(leafAdd, leafA);
    leaf.addOperand(leafAdd, leafB);
    leaf.addResult(leafAdd, leafSum);
    const auto one = makeConstant(leaf, "one", "one_const", 1, "1'b1");
    const auto mask = makeConstant(leaf, "mask", "mask_const", 8, "8'hff");
    makeRegisterWrite(leaf, "leaf_reg_write", one, leafSum, mask, leafClk, "state");

    const auto midA = makeValue(mid, "a", 8, false);
    const auto midB = makeValue(mid, "b", 8, false);
    const auto midClk = makeValue(mid, "clk", 1, false);
    const auto midY = makeValue(mid, "y", 8, false);
    mid.bindInputPort("a", midA);
    mid.bindInputPort("b", midB);
    mid.bindInputPort("clk", midClk);
    mid.bindOutputPort("y", midY);
    const auto midInst = mid.createOperation(grh::OperationKind::kInstance, mid.internSymbol("u_leaf_op"));
    mid.addOperand(midInst, midA);
    mid.addOperand(midInst, midB);
    mid.addOperand(midInst, midClk);
    mid.addResult(midInst, midY);
    mid.setAttr(midInst, "moduleName", std::string("leaf"));
    mid.setAttr(midInst, "instanceName", std::string("u_leaf"));
    mid.setAttr(midInst, "inputPortName", std::vector<std::string>{"a", "b", "clk"});
    mid.setAttr(midInst, "outputPortName", std::vector<std::string>{"y"});

    const auto topA = makeValue(top, "a", 8, false);
    const auto topB = makeValue(top, "b", 8, false);
    const auto topClk = makeValue(top, "clk", 1, false);
    const auto topY = makeValue(top, "y", 8, false);
    top.bindInputPort("a", topA);
    top.bindInputPort("b", topB);
    top.bindInputPort("clk", topClk);
    top.bindOutputPort("y", topY);
    const auto topInst = top.createOperation(grh::OperationKind::kInstance, top.internSymbol("u_mid_op"));
    top.addOperand(topInst, topA);
    top.addOperand(topInst, topB);
    top.addOperand(topInst, topClk);
    top.addResult(topInst, topY);
    top.setAttr(topInst, "moduleName", std::string("mid"));
    top.setAttr(topInst, "instanceName", std::string("u_mid"));
    top.setAttr(topInst, "inputPortName", std::vector<std::string>{"a", "b", "clk"});
    top.setAttr(topInst, "outputPortName", std::vector<std::string>{"y"});

    PassManager manager;
    manager.addPass(std::make_unique<GsimPass>(GsimOptions{"top.u_mid.u_leaf"}));
    PassDiagnostics diags;
    const auto result = manager.run(design, diags);

    expect(result.success, "gsim multi-hop target path should succeed");
    expect(!diags.hasError(), "gsim multi-hop target path should not emit errors");
    expect(design.hasScratchpad("gsim.leaf.topology.order"),
           "gsim should resolve multi-hop path to target child graph metadata namespace");
    expect(!design.hasScratchpad("gsim.mid.topology.order"),
           "gsim instance path should not analyze unrelated intermediate graph metadata");
}

void testMetadataOrderIsDeterministic()
{
    grh::Design design;
    auto &graph = design.createGraph("top");

    const auto a = makeValue(graph, "a", 8, false);
    const auto b = makeValue(graph, "b", 8, false);
    const auto out0 = makeValue(graph, "out0", 8, false);
    const auto out1 = makeValue(graph, "out1", 8, false);
    const auto op1 = graph.createOperation(grh::OperationKind::kAdd, graph.internSymbol("z_add"));
    graph.addOperand(op1, a);
    graph.addOperand(op1, b);
    graph.addResult(op1, out1);
    const auto op0 = graph.createOperation(grh::OperationKind::kSub, graph.internSymbol("a_sub"));
    graph.addOperand(op0, a);
    graph.addOperand(op0, b);
    graph.addResult(op0, out0);

    PassManager manager;
    manager.addPass(std::make_unique<GsimPass>(GsimOptions{"top"}));
    PassDiagnostics diags;
    const auto result = manager.run(design, diags);
    expect(result.success && !diags.hasError(), "determinism fixture should run successfully");

    const auto *topo = design.getScratchpad<std::vector<int64_t>>("gsim.top.topology.order");
    const auto *roots = design.getScratchpad<std::vector<int64_t>>("gsim.top.roots");
    const auto *groupNames = design.getScratchpad<std::vector<std::string>>("gsim.top.event_group_names");
    expect(topo != nullptr, "topology order metadata should exist");
    expect(roots != nullptr, "roots metadata should exist");
    expect(groupNames != nullptr, "event group names metadata should exist");
    expect(topo->size() == 2, "determinism fixture should only contain two ops");
    expect((*topo)[0] < (*topo)[1], "topology order should use stable ascending-id order for equal indegree ops");
    expect(roots->size() == 2 && (*roots)[0] < (*roots)[1],
           "roots metadata should be sorted deterministically");
    expect(groupNames->size() == 1 && groupNames->front() == "combinational",
           "event group names should be stable and sorted");
}

void testFailuresAreClear()
{
    {
        grh::Design design;
        auto &child = design.createGraph("child");
        auto &top = design.createGraph("top");
        const auto in = makeValue(top, "in");
        const auto out = makeValue(top, "out");
        const auto inst = top.createOperation(grh::OperationKind::kInstance, top.internSymbol("u_child_op"));
        top.addOperand(inst, in);
        top.addResult(inst, out);
        top.setAttr(inst, "moduleName", std::string("child"));
        top.setAttr(inst, "instanceName", std::string("u_child"));
        top.setAttr(inst, "inputPortName", std::vector<std::string>{"i"});
        top.setAttr(inst, "outputPortName", std::vector<std::string>{"o"});
        (void)child;

        PassManager manager;
        manager.addPass(std::make_unique<GsimPass>(GsimOptions{"top"}));
        PassDiagnostics diags;
        const auto result = manager.run(design, diags);
        expect(!result.success, "gsim should reject hierarchy prerequisite violations");
        bool sawExpected = false;
        for (const auto &diag : diags.messages())
        {
            if (diag.passName == "gsim" &&
                diag.message == "Design contains kInstance operations - must flatten before partitioning")
            {
                sawExpected = true;
                break;
            }
        }
        expect(sawExpected, "gsim should surface shared hierarchy precondition diagnostic");
    }

    {
        grh::Design design;
        auto &graph = design.createGraph("top");
        graph.createOperation(grh::OperationKind::kBlackbox, graph.internSymbol("bb"));

        PassManager manager;
        manager.addPass(std::make_unique<GsimPass>(GsimOptions{"top"}));
        PassDiagnostics diags;
        const auto result = manager.run(design, diags);
        expect(!result.success, "gsim should reject blackbox prerequisite violations");
        bool sawExpected = false;
        for (const auto &diag : diags.messages())
        {
            if (diag.passName == "gsim" &&
                diag.message == "Design contains kBlackbox operations - not supported")
            {
                sawExpected = true;
                break;
            }
        }
        expect(sawExpected, "gsim should surface shared blackbox precondition diagnostic");
    }

    {
        grh::Design design;
        design.createGraph("top");
        PassManager manager;
        manager.addPass(std::make_unique<GsimPass>(GsimOptions{"top.no_such_inst"}));
        PassDiagnostics diags;
        const auto result = manager.run(design, diags);
        expect(!result.success, "gsim should fail invalid target paths clearly");
        bool sawResolveFailure = false;
        for (const auto &diag : diags.messages())
        {
            if (diag.passName == "gsim" && diag.message == "failed to resolve gsim target path")
            {
                sawResolveFailure = true;
                break;
            }
        }
        expect(sawResolveFailure, "gsim should emit a clear target path resolution diagnostic");
    }
}

} // namespace

int main()
{
    try
    {
        testPassRegistration();
        testGraphOnlyPathWritesMetadata();
        testMultiHopInstancePathUsesSharedResolver();
        testMetadataOrderIsDeterministic();
        testFailuresAreClear();
    }
    catch (const std::exception &ex)
    {
        return fail(ex.what());
    }
    return 0;
}
