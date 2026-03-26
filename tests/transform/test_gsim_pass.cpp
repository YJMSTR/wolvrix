#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/gsim.hpp"

#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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

grh::ValueId makeConst(grh::Graph &graph,
                       const std::string &valueName,
                       const std::string &opName,
                       int32_t width,
                       const std::string &literal)
{
    const auto value = graph.createValue(graph.internSymbol(valueName), width, false);
    const auto op = graph.createOperation(grh::OperationKind::kConstant,
                                          graph.internSymbol(opName));
    graph.addResult(op, value);
    graph.setAttr(op, "constValue", literal);
    return value;
}

grh::OperationId addInstance(grh::Graph &graph,
                             std::string_view instanceName,
                             std::string_view moduleName,
                             std::initializer_list<grh::ValueId> operands,
                             std::initializer_list<grh::ValueId> results,
                             std::vector<std::string> inputPortNames,
                             std::vector<std::string> outputPortNames)
{
    const auto op = graph.createOperation(grh::OperationKind::kInstance,
                                          graph.internSymbol(std::string(instanceName) + "_op"));
    graph.setAttr(op, "instanceName", std::string(instanceName));
    graph.setAttr(op, "moduleName", std::string(moduleName));
    graph.setAttr(op, "inputPortName", std::move(inputPortNames));
    graph.setAttr(op, "outputPortName", std::move(outputPortNames));
    graph.setAttr(op, "inoutPortName", std::vector<std::string>{});
    for (const auto operand : operands)
    {
        graph.addOperand(op, operand);
    }
    for (const auto result : results)
    {
        graph.addResult(op, result);
    }
    return op;
}

bool hasOpKind(const grh::Graph &graph, grh::OperationKind kind)
{
    for (const auto opId : graph.operations())
    {
        if (graph.getOperation(opId).kind() == kind)
        {
            return true;
        }
    }
    return false;
}

bool hasDiagMessage(const PassDiagnostics &diags,
                    std::string_view passName,
                    std::string_view message,
                    std::string_view contextNeedle = {})
{
    for (const auto &diag : diags.messages())
    {
        if (diag.passName == passName && diag.message == message)
        {
            if (contextNeedle.empty() || diag.context.find(contextNeedle) != std::string::npos)
            {
                return true;
            }
        }
    }
    return false;
}

PassManagerResult runGsim(grh::Design &design,
                          std::string_view path,
                          PassDiagnostics &diags)
{
    PassManager manager;
    manager.addPass(std::make_unique<GsimPass>(GsimOptions{std::string(path)}));
    return manager.run(design, diags);
}

PassManagerResult runPipeline(grh::Design &design,
                              std::initializer_list<std::string_view> passNames,
                              PassDiagnostics &diags)
{
    PassManager manager;
    for (const auto passName : passNames)
    {
        std::string error;
        auto pass = makePass(passName, {}, error);
        expect(static_cast<bool>(pass), "expected prerequisite pass to be registered: " + std::string(passName));
        expect(error.empty(), "unexpected prerequisite pass construction error for " + std::string(passName));
        manager.addPass(std::move(pass));
    }
    return manager.run(design, diags);
}

void expectGsimSuccess(const PassManagerResult &result,
                       const PassDiagnostics &diags,
                       const std::string &message)
{
    expect(result.success, message);
    expect(!diags.hasError(), message + " should not emit errors");
}

void expectGsimFailure(const PassManagerResult &result,
                       const PassDiagnostics &diags,
                       const std::string &message)
{
    expect(!result.success, message);
    expect(diags.hasError(), message + " should emit diagnostics");
}

void buildLeafStatefulGraph(grh::Graph &graph)
{
    const auto inA = makeValue(graph, "a", 8, false);
    const auto inB = makeValue(graph, "b", 8, false);
    const auto clk = makeValue(graph, "clk", 1, false);
    graph.bindInputPort("a", inA);
    graph.bindInputPort("b", inB);
    graph.bindInputPort("clk", clk);

    const auto addOut = makeValue(graph, "sum", 8, false);
    const auto outY = makeValue(graph, "y", 8, false);
    graph.bindOutputPort("y", outY);
    const auto add = graph.createOperation(grh::OperationKind::kAdd, graph.internSymbol("add"));
    graph.addOperand(add, inA);
    graph.addOperand(add, inB);
    graph.addResult(add, addOut);

    const auto assign = graph.createOperation(grh::OperationKind::kAssign, graph.internSymbol("assign_y"));
    graph.addOperand(assign, addOut);
    graph.addResult(assign, outY);

    const auto one = makeConstant(graph, "one", "one_const", 1, "1'b1");
    const auto mask = makeConstant(graph, "mask", "mask_const", 8, "8'hff");
    makeRegisterWrite(graph, "reg_write", one, addOut, mask, clk, "state");
}

grh::Design buildHierarchyFixture()
{
    grh::Design design;
    auto &leaf = design.createGraph("leaf");
    auto &top = design.createGraph("top");
    design.markAsTop("top");

    const auto leafA = makeValue(leaf, "a", 8, false);
    const auto leafB = makeValue(leaf, "b", 8, false);
    const auto leafY = makeValue(leaf, "y", 8, false);
    leaf.bindInputPort("a", leafA);
    leaf.bindInputPort("b", leafB);
    leaf.bindOutputPort("y", leafY);
    const auto leafSum = makeValue(leaf, "sum", 8, false);
    const auto leafAdd = leaf.createOperation(grh::OperationKind::kAdd, leaf.internSymbol("leaf_add"));
    leaf.addOperand(leafAdd, leafA);
    leaf.addOperand(leafAdd, leafB);
    leaf.addResult(leafAdd, leafSum);
    const auto leafAssign = leaf.createOperation(grh::OperationKind::kAssign, leaf.internSymbol("leaf_assign_y"));
    leaf.addOperand(leafAssign, leafSum);
    leaf.addResult(leafAssign, leafY);

    const auto topA = makeValue(top, "a", 8, false);
    const auto topB = makeValue(top, "b", 8, false);
    const auto topY = makeValue(top, "y", 8, false);
    top.bindInputPort("a", topA);
    top.bindInputPort("b", topB);
    top.bindOutputPort("y", topY);
    addInstance(top,
                "u_leaf",
                "leaf",
                {topA, topB},
                {topY},
                {"a", "b"},
                {"y"});

    return design;
}

grh::Design buildDebugFixture()
{
    grh::Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    buildLeafStatefulGraph(graph);
    const auto sumValue = graph.findValue("sum");
    expect(sumValue.valid(), "debug fixture should expose sum value");
    const auto dbg = graph.createOperation(grh::OperationKind::kSystemTask, graph.internSymbol("display"));
    graph.addOperand(dbg, sumValue);
    graph.setAttr(dbg, "name", std::string("$display"));
    return design;
}

grh::Design buildRetimableMemoryFixture(bool addSecondWrite = false)
{
    grh::Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto clk = makeValue(graph, "clk", 1, false);
    const auto en = makeValue(graph, "en", 1, false);
    const auto addrD = makeValue(graph, "addr_d", 2, false);
    graph.bindInputPort("clk", clk);
    graph.bindInputPort("en", en);
    graph.bindInputPort("addr_d", addrD);

    const auto addrMask = makeConst(graph, "addr_mask", "addr_mask_op", 2, "2'b11");
    const auto memMask = makeConst(graph, "mem_mask", "mem_mask_op", 8, "8'hff");

    const auto mem = graph.createOperation(grh::OperationKind::kMemory, graph.internSymbol("mem0"));
    graph.setAttr(mem, "width", static_cast<int64_t>(8));
    graph.setAttr(mem, "row", static_cast<int64_t>(4));
    graph.setAttr(mem, "isSigned", false);
    graph.setAttr(mem, "initKind", std::vector<std::string>{"literal"});
    graph.setAttr(mem, "initFile", std::vector<std::string>{""});
    graph.setAttr(mem, "initValue", std::vector<std::string>{"8'h34"});
    graph.setAttr(mem, "initStart", std::vector<int64_t>{-1});
    graph.setAttr(mem, "initLen", std::vector<int64_t>{0});

    const auto reg = graph.createOperation(grh::OperationKind::kRegister, graph.internSymbol("addr_q"));
    graph.setAttr(reg, "width", static_cast<int64_t>(2));
    graph.setAttr(reg, "isSigned", false);
    graph.setAttr(reg, "initValue", std::string("2'd1"));

    const auto regWrite = graph.createOperation(grh::OperationKind::kRegisterWritePort,
                                                graph.internSymbol("addr_q_write"));
    graph.setAttr(regWrite, "regSymbol", std::string("addr_q"));
    graph.setAttr(regWrite, "clockSymbol", std::string("clk"));
    graph.setAttr(regWrite, "eventEdge", std::vector<std::string>{"posedge"});
    graph.addOperand(regWrite, en);
    graph.addOperand(regWrite, addrD);
    graph.addOperand(regWrite, addrMask);
    graph.addOperand(regWrite, clk);

    const auto regRead = graph.createOperation(grh::OperationKind::kRegisterReadPort,
                                               graph.internSymbol("addr_q_read"));
    graph.setAttr(regRead, "regSymbol", std::string("addr_q"));
    const auto addrQ = makeValue(graph, "addr_q_val", 2, false);
    graph.addResult(regRead, addrQ);

    const auto memRead = graph.createOperation(grh::OperationKind::kMemoryReadPort,
                                               graph.internSymbol("mem0_read"));
    graph.setAttr(memRead, "memSymbol", std::string("mem0"));
    graph.addOperand(memRead, addrQ);
    const auto data = makeValue(graph, "data", 8, false);
    graph.addResult(memRead, data);
    graph.bindOutputPort("data", data);

    const auto wen = makeValue(graph, "wen", 1, false);
    const auto waddr = makeValue(graph, "waddr", 2, false);
    const auto wdata = makeValue(graph, "wdata", 8, false);
    graph.bindInputPort("wen", wen);
    graph.bindInputPort("waddr", waddr);
    graph.bindInputPort("wdata", wdata);

    const auto memWrite = graph.createOperation(grh::OperationKind::kMemoryWritePort,
                                                graph.internSymbol("mem0_write"));
    graph.setAttr(memWrite, "memSymbol", std::string("mem0"));
    graph.setAttr(memWrite, "clockSymbol", std::string("clk"));
    graph.setAttr(memWrite, "eventEdge", std::vector<std::string>{"posedge"});
    graph.addOperand(memWrite, wen);
    graph.addOperand(memWrite, waddr);
    graph.addOperand(memWrite, wdata);
    graph.addOperand(memWrite, memMask);
    graph.addOperand(memWrite, clk);

    if (addSecondWrite)
    {
        const auto wen2 = makeValue(graph, "wen2", 1, false);
        const auto waddr2 = makeValue(graph, "waddr2", 2, false);
        const auto wdata2 = makeValue(graph, "wdata2", 8, false);
        graph.bindInputPort("wen2", wen2);
        graph.bindInputPort("waddr2", waddr2);
        graph.bindInputPort("wdata2", wdata2);

        const auto memWrite2 = graph.createOperation(grh::OperationKind::kMemoryWritePort,
                                                     graph.internSymbol("mem0_write_2"));
        graph.setAttr(memWrite2, "memSymbol", std::string("mem0"));
        graph.setAttr(memWrite2, "clockSymbol", std::string("clk"));
        graph.setAttr(memWrite2, "eventEdge", std::vector<std::string>{"posedge"});
        graph.addOperand(memWrite2, wen2);
        graph.addOperand(memWrite2, waddr2);
        graph.addOperand(memWrite2, wdata2);
        graph.addOperand(memWrite2, memMask);
        graph.addOperand(memWrite2, clk);
    }

    return design;
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
    const auto *scheduleOrder = design.getScratchpad<std::vector<std::string>>("gsim.top.schedule.activity_order");
    const auto *scheduleMembers = design.getScratchpad<std::map<std::string, std::vector<int64_t>>>("gsim.top.schedule.activity_members");
    const auto *scheduleClasses = design.getScratchpad<std::map<std::string, std::string>>("gsim.top.schedule.activity_classes");
    const auto *hyperKind = design.getScratchpad<std::string>("gsim.top.hypergraph.kind");
    const auto *hyperContract = design.getScratchpad<std::string>("gsim.top.hypergraph.contract");
    const auto *hyperNodes = design.getScratchpad<std::vector<std::string>>("gsim.top.hypergraph.node_names");
    const auto *hyperEdges = design.getScratchpad<std::vector<std::string>>("gsim.top.hypergraph.edge_names");
    const auto *hyperSinks = design.getScratchpad<std::map<std::string, std::vector<int64_t>>>("gsim.top.hypergraph.edge_sinks");
    const auto *graphRevision = design.getScratchpad<int64_t>("gsim.top.graph_revision");

    expect(roots != nullptr, "gsim should write roots metadata");
    expect(groups != nullptr, "gsim should write event group metadata");
    expect(topo != nullptr, "gsim should write topology metadata");
    expect(classifications != nullptr, "gsim should write op classifications");
    expect(scheduleKind != nullptr && *scheduleKind == "activity-v1",
           "gsim should write concrete schedule metadata kind");
    expect(scheduleOrder != nullptr && !scheduleOrder->empty(),
           "gsim should write schedule activity ordering");
    expect(scheduleMembers != nullptr && scheduleMembers->count("activity.reg:clk") == 1,
           "gsim should write schedule activity membership for stateful groups");
    expect(scheduleClasses != nullptr && scheduleClasses->at("activity.reg:clk") == "stateful",
           "gsim should classify register-driven activities as stateful");
    expect(hyperKind != nullptr && *hyperKind == "activity-connectivity-v1",
           "gsim should write concrete hypergraph metadata kind");
    expect(hyperContract != nullptr && *hyperContract == "gsim.activity.hypergraph.v1",
           "gsim should record the hypergraph contract version");
    expect(hyperNodes != nullptr && !hyperNodes->empty(),
           "gsim should write hypergraph node metadata");
    expect(hyperEdges != nullptr && !hyperEdges->empty(),
           "gsim should write hypergraph edge metadata");
    expect(hyperSinks != nullptr && hyperSinks->count("edge.reg:clk") == 1,
           "gsim should write hypergraph sink metadata for event groups");
    expect(graphRevision != nullptr && *graphRevision == static_cast<int64_t>(graph.revision()),
           "gsim should record the analyzed graph revision in scratchpad metadata");

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
    expect(design.hasScratchpad("gsim.leaf.path.u_mid$u_leaf.topology.order"),
           "gsim should keep multi-hop metadata instance-scoped when the target graph is shared");
    expect(!design.hasScratchpad("gsim.leaf.topology.order"),
           "gsim instance path should not collapse shared-module metadata into the graph-only namespace");
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

void testPrerequisiteAuditPipelineCoverage()
{
    {
        auto design = buildHierarchyFixture();
        PassDiagnostics directDiags;
        const auto directResult = runGsim(design, "top", directDiags);
        expectGsimFailure(directResult, directDiags, "gsim should reject hierarchy before hier-flatten");
        expect(hasDiagMessage(directDiags,
                              "gsim",
                              "Design contains kInstance operations - must flatten before partitioning",
                              "Graph: top"),
               "hierarchy rejection should name the top graph");

        auto normalized = buildHierarchyFixture();
        PassDiagnostics pipelineDiags;
        const auto pipelineResult = runPipeline(normalized, {"hier-flatten"}, pipelineDiags);
        expect(pipelineResult.success, "hier-flatten prerequisite pipeline should succeed before gsim");
        expect(!pipelineDiags.hasError(), "hier-flatten prerequisite pipeline should not emit errors before gsim");
        expect(pipelineResult.changed, "hier-flatten should change hierarchy fixture");
        expect(!hasOpKind(*normalized.findGraph("top"), grh::OperationKind::kInstance),
               "hier-flatten should remove instances before gsim");
        PassDiagnostics normalizedDiags;
        const auto normalizedResult = runGsim(normalized, "top", normalizedDiags);
        expectGsimSuccess(normalizedResult, normalizedDiags,
                          "gsim should succeed after hier-flatten normalizes hierarchy");
    }

    {
        auto design = buildDebugFixture();
        PassDiagnostics directDiags;
        const auto directResult = runGsim(design, "top", directDiags);
        expectGsimSuccess(directResult, directDiags, "gsim should tolerate debug ops before cleanup");
        expect(design.hasScratchpad("gsim.top.event_groups"),
               "debug fixture should still produce baseline gsim metadata");

        auto cleaned = buildDebugFixture();
        PassDiagnostics pipelineDiags;
        const auto pipelineResult = runPipeline(cleaned, {"strip-debug"}, pipelineDiags);
        expect(pipelineResult.success, "strip-debug prerequisite pipeline should succeed before gsim");
        expect(!pipelineDiags.hasError(), "strip-debug prerequisite pipeline should not emit errors before gsim");
        expect(pipelineResult.changed, "strip-debug should split debug fixture");
        auto *logic = cleaned.findGraph("top_logic_part");
        auto *debug = cleaned.findGraph("top_debug_part");
        expect(logic != nullptr, "strip-debug should create logic partition for gsim target");
        expect(debug != nullptr, "strip-debug should preserve removed debug partition");
        expect(!hasOpKind(*logic, grh::OperationKind::kSystemTask),
               "logic partition should be free of system tasks before gsim");
        expect(hasOpKind(*debug, grh::OperationKind::kSystemTask),
               "debug partition should retain stripped system tasks");
        PassDiagnostics normalizedDiags;
        const auto normalizedResult = runGsim(cleaned, "top_logic_part", normalizedDiags);
        expectGsimSuccess(normalizedResult, normalizedDiags,
                          "gsim should succeed on strip-debug logic partition");
        expect(cleaned.hasScratchpad("gsim.top_logic_part.topology.order"),
               "gsim metadata should target the strip-debug logic partition");
    }

    {
        auto design = buildRetimableMemoryFixture();
        PassDiagnostics directDiags;
        const auto directResult = runGsim(design, "top", directDiags);
        expectGsimSuccess(directResult, directDiags,
                          "gsim should accept supported memory read paths before retiming");
        expect(design.hasScratchpad("gsim.top.event_groups"),
               "memory fixture should produce baseline gsim metadata");

        auto normalized = buildRetimableMemoryFixture();
        auto *beforeGraph = normalized.findGraph("top");
        expect(beforeGraph != nullptr, "memory fixture should have top graph");
        expect(hasOpKind(*beforeGraph, grh::OperationKind::kMemoryReadPort),
               "memory fixture should start with a memory read op");
        PassDiagnostics pipelineDiags;
        const auto pipelineResult = runPipeline(normalized, {"memory-read-retime"}, pipelineDiags);
        expect(pipelineResult.success, "memory-read-retime prerequisite pipeline should succeed before gsim");
        expect(!pipelineDiags.hasError(), "memory-read-retime prerequisite pipeline should not emit errors before gsim");
        expect(pipelineResult.changed, "memory-read-retime should rewrite single-write memory fixture");
        auto *afterGraph = normalized.findGraph("top");
        expect(afterGraph != nullptr, "retimed memory fixture should still have top graph");
        expect(hasOpKind(*afterGraph, grh::OperationKind::kMemoryReadPort),
               "retimed fixture should remain analyzable with memory read state ops");
        PassDiagnostics normalizedDiags;
        const auto normalizedResult = runGsim(normalized, "top", normalizedDiags);
        expectGsimSuccess(normalizedResult, normalizedDiags,
                          "gsim should succeed after memory-read-retime cleanup");
    }

    {
        auto design = buildRetimableMemoryFixture(true);
        PassDiagnostics directDiags;
        const auto directResult = runGsim(design, "top", directDiags);
        expectGsimSuccess(directResult, directDiags,
                          "gsim should accept multi-write memories without fake cleanup requirements");

        auto normalized = buildRetimableMemoryFixture(true);
        PassDiagnostics pipelineDiags;
        const auto pipelineResult = runPipeline(normalized, {"memory-read-retime"}, pipelineDiags);
        expect(pipelineResult.success, "multi-write memory-read-retime pipeline should not fail");
        expect(!pipelineDiags.hasError(), "multi-write memory-read-retime pipeline should not emit errors");
        expect(!pipelineResult.changed,
               "memory-read-retime should explicitly skip unsupported multi-write memories today");
        PassDiagnostics normalizedDiags;
        const auto normalizedResult = runGsim(normalized, "top", normalizedDiags);
        expectGsimSuccess(normalizedResult, normalizedDiags,
                          "gsim should still succeed when memory-read-retime leaves multi-write memory unchanged");
    }

    {
        grh::Design design;
        auto &graph = design.createGraph("top");
        design.markAsTop("top");
        graph.createOperation(grh::OperationKind::kBlackbox, graph.internSymbol("bb"));

        PassDiagnostics directDiags;
        const auto directResult = runGsim(design, "top", directDiags);
        expectGsimFailure(directResult, directDiags,
                          "gsim should reject blackboxes before normalization");
        expect(hasDiagMessage(directDiags,
                              "gsim",
                              "Design contains kBlackbox operations - not supported",
                              "Graph: top"),
               "blackbox rejection should name the target graph");

        PassDiagnostics simplifyDiags;
        const auto simplifyResult = runPipeline(design, {"simplify"}, simplifyDiags);
        expect(simplifyResult.success, "simplify should not fail on blackbox fixture");
        expect(!simplifyDiags.hasError(), "simplify should not emit errors on blackbox fixture");
        expect(!simplifyResult.changed, "simplify should not pretend to normalize blackboxes away");
        PassDiagnostics afterSimplifyDiags;
        const auto afterSimplifyResult = runGsim(design, "top", afterSimplifyDiags);
        expectGsimFailure(afterSimplifyResult, afterSimplifyDiags,
                          "gsim should still reject blackboxes after unrelated normalization passes");
        expect(hasDiagMessage(afterSimplifyDiags,
                              "gsim",
                              "Design contains kBlackbox operations - not supported",
                              "Graph: top"),
               "blackbox rejection should remain explicit after simplify");
    }
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

        PassDiagnostics diags;
        const auto result = runGsim(design, "top", diags);
        expectGsimFailure(result, diags, "gsim should reject hierarchy prerequisite violations");
        expect(hasDiagMessage(diags,
                              "gsim",
                              "Design contains kInstance operations - must flatten before partitioning"),
               "gsim should surface shared hierarchy precondition diagnostic");
    }

    {
        grh::Design design;
        auto &graph = design.createGraph("top");
        graph.createOperation(grh::OperationKind::kBlackbox, graph.internSymbol("bb"));

        PassDiagnostics diags;
        const auto result = runGsim(design, "top", diags);
        expectGsimFailure(result, diags, "gsim should reject blackbox prerequisite violations");
        expect(hasDiagMessage(diags,
                              "gsim",
                              "Design contains kBlackbox operations - not supported"),
               "gsim should surface shared blackbox precondition diagnostic");
    }

    {
        for (const std::string &path : {".top", "top.", "top..u_leaf"})
        {
            grh::Design design;
            design.createGraph("top");
            PassDiagnostics diags;
            const auto result = runGsim(design, path, diags);
            expectGsimFailure(result, diags, "gsim should reject malformed target paths with empty segments");
            bool sawResolveFailure = false;
            bool sawEmptySegmentContext = false;
            for (const auto &diag : diags.messages())
            {
                if (diag.passName == "gsim" && diag.message == "failed to resolve gsim target path")
                {
                    sawResolveFailure = true;
                    if (diag.context.find("empty path segments") != std::string::npos)
                    {
                        sawEmptySegmentContext = true;
                    }
                }
            }
            expect(sawResolveFailure, "gsim should emit a clear malformed target path diagnostic");
            expect(sawEmptySegmentContext, "gsim malformed target path diagnostic should mention empty segments");
        }
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
        testPrerequisiteAuditPipelineCoverage();
        testFailuresAreClear();
    }
    catch (const std::exception &ex)
    {
        return fail(ex.what());
    }
    return 0;
}
