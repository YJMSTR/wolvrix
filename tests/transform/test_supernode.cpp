#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/supernode_graph.hpp"
#include "transform/timing_domain_analyzer.hpp"
#include "transform/supernode_coarsener.hpp"
#include "transform/supernode_partitioner.hpp"
#include "transform/supernode_partition_pass.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace wolvrix::lib;
using namespace wolvrix::lib::transform;

namespace
{

int fail(const std::string &message)
{
    std::cerr << "[transform-supernode] " << message << '\n';
    return 1;
}

void expect(bool condition, const std::string &message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

grh::OperationId makeRegisterWrite(grh::Graph &graph,
                                   std::string_view opName,
                                   grh::ValueId updateCond,
                                   grh::ValueId nextValue,
                                   grh::ValueId maskValue,
                                   grh::ValueId clk,
                                   std::string_view regSymbol)
{
    const auto op = graph.createOperation(grh::OperationKind::kRegisterWritePort,
                                          graph.internSymbol(opName));
    graph.addOperand(op, updateCond);
    graph.addOperand(op, nextValue);
    graph.addOperand(op, maskValue);
    graph.addOperand(op, clk);
    graph.setAttr(op, "regSymbol", std::string(regSymbol));
    graph.setAttr(op, "eventEdge", std::vector<std::string>{"posedge"});
    return op;
}

grh::ValueId makeConstant(grh::Graph &graph,
                          std::string_view valueName,
                          std::string_view opName,
                          int32_t width,
                          std::string literal)
{
    const auto value = graph.createValue(graph.internSymbol(valueName), width, false);
    const auto op = graph.createOperation(grh::OperationKind::kConstant,
                                          graph.internSymbol(opName));
    graph.addResult(op, value);
    graph.setAttr(op, "constValue", std::move(literal));
    return value;
}

grh::ValueId makeMux(grh::Graph &graph,
                     std::string_view valueName,
                     std::string_view opName,
                     grh::ValueId condition,
                     grh::ValueId whenTrue,
                     grh::ValueId whenFalse,
                     int32_t width)
{
    const auto value = graph.createValue(graph.internSymbol(valueName), width, false);
    const auto op = graph.createOperation(grh::OperationKind::kMux,
                                          graph.internSymbol(opName));
    graph.addOperand(op, condition);
    graph.addOperand(op, whenTrue);
    graph.addOperand(op, whenFalse);
    graph.addResult(op, value);
    return value;
}

class PartitionScratchpadChecker : public Pass
{
public:
    PartitionScratchpadChecker(grh::OperationId expectedCombinationalOp,
                               grh::OperationId expectedSequentialOp)
        : Pass("partition-scratchpad-checker", "partition-scratchpad-checker"),
          expectedCombinationalOp_(expectedCombinationalOp),
          expectedSequentialOp_(expectedSequentialOp)
    {
    }

    PassResult run() override
    {
        const auto *domains = getScratchpad<std::vector<std::string>>("supernode.top.domains");
        if (domains == nullptr)
        {
            throw std::runtime_error("missing supernode.top.domains discovery key");
        }

        bool sawCombinational = false;
        bool sawSequential = false;
        for (const auto &domain : *domains)
        {
            sawCombinational |= domain == "combinational";
            sawSequential |= domain == "domain_0";
        }
        if (!sawCombinational)
        {
            throw std::runtime_error("expected combinational domain discovery entry");
        }
        if (!sawSequential)
        {
            throw std::runtime_error("expected sequential timing domain discovery entry");
        }

        using Mapping = std::unordered_map<grh::OperationId, SuperNodeId, grh::OperationIdHash>;
        const auto *combinationalMap =
            getScratchpad<Mapping>("supernode.top.combinational.op_to_sn");
        const auto *sequentialMap =
            getScratchpad<Mapping>("supernode.top.domain_0.op_to_sn");
        if (combinationalMap == nullptr || sequentialMap == nullptr)
        {
            throw std::runtime_error("missing expected op_to_sn scratchpad keys");
        }
        if (combinationalMap->find(expectedCombinationalOp_) == combinationalMap->end())
        {
            throw std::runtime_error("unrelated combinational op was not assigned to any supernode");
        }
        if (sequentialMap->find(expectedSequentialOp_) == sequentialMap->end())
        {
            throw std::runtime_error("sequential op was not assigned to its timing-domain supernode");
        }
        return {};
    }

private:
    grh::OperationId expectedCombinationalOp_;
    grh::OperationId expectedSequentialOp_;
};

void testSuperNodeGraphBasics()
{
    SuperNodeGraph sg;

    const auto id1 = sg.createSuperNode();
    const auto id2 = sg.createSuperNode();
    expect(sg.nodeCount() == 2, "expected two supernodes after creation");

    sg.merge(id1, id2);
    expect(sg.nodeCount() == 1, "expected merge to reduce node count");
}

void testTopologicalSort()
{
    SuperNodeGraph sg;

    const auto id1 = sg.createSuperNode();
    const auto id2 = sg.createSuperNode();
    const auto id3 = sg.createSuperNode();

    sg.getNode(id1).successors.insert(id2);
    sg.getNode(id2).predecessors.insert(id1);
    sg.getNode(id2).successors.insert(id3);
    sg.getNode(id3).predecessors.insert(id2);

    const auto sorted = sg.topologicalSort();
    expect(sorted.size() == 3, "expected topological order to include all nodes");
    expect(!sg.hasCircularDependency(), "unexpected cycle in DAG");
}

void testCycleDetection()
{
    SuperNodeGraph sg;

    const auto id1 = sg.createSuperNode();
    const auto id2 = sg.createSuperNode();
    const auto id3 = sg.createSuperNode();

    sg.getNode(id1).successors.insert(id2);
    sg.getNode(id2).predecessors.insert(id1);
    sg.getNode(id2).successors.insert(id3);
    sg.getNode(id3).predecessors.insert(id2);
    sg.getNode(id3).successors.insert(id1);
    sg.getNode(id1).predecessors.insert(id3);

    expect(sg.hasCircularDependency(), "expected cycle detection to report a cycle");
}

void testMergeConstraints()
{
    SuperNodeGraph sg;

    const auto id1 = sg.createSuperNode();
    const auto id2 = sg.createSuperNode();

    sg.getNode(id1).timingDomain = "clk1";
    sg.getNode(id2).timingDomain = "clk2";

    sg.merge(id1, id2);
    expect(sg.nodeCount() == 1, "graph-level merge should not reject differing domains");
}

void testEdgeCounting()
{
    SuperNodeGraph sg;

    const auto id1 = sg.createSuperNode();
    const auto id2 = sg.createSuperNode();
    const auto id3 = sg.createSuperNode();

    sg.getNode(id1).successors.insert(id2);
    sg.getNode(id2).predecessors.insert(id1);
    sg.getNode(id2).successors.insert(id3);
    sg.getNode(id3).predecessors.insert(id2);

    expect(sg.edgeCount() == 2, "expected edge counter to sum successor sets");
}

void testMergeRejectsContractionThatCreatesCycle()
{
    SuperNodeGraph sg;

    const auto idA = sg.createSuperNode();
    const auto idB = sg.createSuperNode();
    const auto idC = sg.createSuperNode();

    sg.getNode(idA).successors.insert(idB);
    sg.getNode(idB).predecessors.insert(idA);
    sg.getNode(idB).successors.insert(idC);
    sg.getNode(idC).predecessors.insert(idB);

    bool threw = false;
    try
    {
        sg.merge(idC, idA);
    }
    catch (const std::invalid_argument &)
    {
        threw = true;
    }

    expect(threw, "expected merge(c, a) on a->b->c to throw");
    expect(!sg.hasCircularDependency(), "failed merge must leave graph acyclic");
    expect(sg.nodeCount() == 3, "failed merge must not mutate node count");
}

void testMergeAllowsDirectContractionWithoutIntroducingCycle()
{
    SuperNodeGraph sg;

    const auto idA = sg.createSuperNode();
    const auto idB = sg.createSuperNode();
    const auto idC = sg.createSuperNode();

    sg.getNode(idA).successors.insert(idB);
    sg.getNode(idB).predecessors.insert(idA);
    sg.getNode(idB).successors.insert(idC);
    sg.getNode(idC).predecessors.insert(idB);

    sg.merge(idA, idB);

    expect(sg.nodeCount() == 2, "valid edge contraction should reduce node count");
    expect(!sg.hasCircularDependency(), "valid edge contraction must preserve acyclicity");
    expect(sg.successors(idA).count(idC) == 1, "merged node should reconnect to successor");
    expect(sg.predecessors(idC).count(idA) == 1, "successor should point back to merged target");
}

void testTopologicalSortUsesStableCanonicalOrder()
{
    SuperNodeGraph sg;

    const auto id0 = sg.createSuperNode();
    const auto id1 = sg.createSuperNode();
    const auto id2 = sg.createSuperNode();
    const auto id3 = sg.createSuperNode();

    sg.getNode(id0).successors.insert(id2);
    sg.getNode(id0).successors.insert(id1);
    sg.getNode(id2).predecessors.insert(id0);
    sg.getNode(id1).predecessors.insert(id0);
    sg.getNode(id1).successors.insert(id3);
    sg.getNode(id2).successors.insert(id3);
    sg.getNode(id3).predecessors.insert(id1);
    sg.getNode(id3).predecessors.insert(id2);

    const std::vector<SuperNodeId> expected{id0, id1, id2, id3};
    const auto first = sg.topologicalSort();
    const auto second = sg.topologicalSort();

    expect(first == expected, "topological sort must use ascending-id canonical order");
    expect(second == expected, "repeated topological sort must remain stable");
}

void testTimingDomainAnalyzerDoesNotInventCrossDomainForSharedSameClockLogic()
{
    grh::Design design;
    grh::Graph &graph = design.createGraph("top");

    const auto clk = graph.createValue(graph.internSymbol("clk"), 1, false);
    const auto enA = graph.createValue(graph.internSymbol("en_a"), 1, false);
    const auto enB = graph.createValue(graph.internSymbol("en_b"), 1, false);
    const auto dataA = graph.createValue(graph.internSymbol("data_a"), 8, false);
    const auto dataB = graph.createValue(graph.internSymbol("data_b"), 8, false);
    const auto mask = graph.createValue(graph.internSymbol("mask"), 8, false);

    const auto sharedValue = graph.createValue(graph.internSymbol("shared"), 8, false);
    const auto sharedAdd = graph.createOperation(grh::OperationKind::kAdd,
                                                 graph.internSymbol("shared_add"));
    graph.addOperand(sharedAdd, dataA);
    graph.addOperand(sharedAdd, dataB);
    graph.addResult(sharedAdd, sharedValue);

    const auto regWriteA = makeRegisterWrite(graph, "reg_write_a", enA, sharedValue, mask, clk, "reg_a");
    const auto regWriteB = makeRegisterWrite(graph, "reg_write_b", enB, sharedValue, mask, clk, "reg_b");

    TimingDomainAnalyzer analyzer(graph);
    const auto opToDomain = analyzer.assignTimingDomains();

    const auto sharedIt = opToDomain.find(sharedAdd);
    expect(sharedIt != opToDomain.end(), "expected shared combinational op to receive a domain");
    expect(sharedIt->second != "cross_domain",
           "shared combinational logic driven only by one clock domain must not be marked cross_domain");
    expect(opToDomain.at(regWriteA) == opToDomain.at(regWriteB),
           "same event key should produce the same sequential timing domain");
    expect(sharedIt->second == opToDomain.at(regWriteA),
           "shared combinational logic should inherit the single reachable timing domain");
}

void testCoarsenerDoesNotMergeDifferentResetSemantics()
{
    grh::Design design;
    grh::Graph &graph = design.createGraph("top");

    const auto clk = graph.createValue(graph.internSymbol("clk"), 1, false);
    const auto rstA = graph.createValue(graph.internSymbol("rst_a"), 1, false);
    const auto rstB = graph.createValue(graph.internSymbol("rst_b"), 1, false);
    const auto mask = makeConstant(graph, "mask", "mask_const", 8, "8'hff");
    const auto zero = makeConstant(graph, "zero", "zero_const", 8, "8'h00");
    const auto one = makeConstant(graph, "one", "one_const", 1, "1'b1");
    const auto data = graph.createValue(graph.internSymbol("data"), 8, false);

    const auto nextA = makeMux(graph, "next_a", "mux_a", rstA, zero, data, 8);
    const auto nextB = makeMux(graph, "next_b", "mux_b", rstB, zero, data, 8);

    const auto writeA = makeRegisterWrite(graph, "reg_write_a", one, nextA, mask, clk, "reg_a");
    const auto writeB = makeRegisterWrite(graph, "reg_write_b", one, nextB, mask, clk, "reg_b");

    SuperNodeGraph sg;
    const auto snA = sg.createSuperNode();
    const auto snB = sg.createSuperNode();
    sg.addMember(snA, writeA);
    sg.addMember(snB, writeB);
    sg.getNode(snA).timingDomain = "domain_0";
    sg.getNode(snB).timingDomain = "domain_0";

    SuperNodeCoarsener coarsener(sg, graph);
    coarsener.coarsen();

    expect(sg.nodeCount() == 2,
           "write ports with different reset semantics must not merge");
}

void testCoarsenerMergesIdenticalResetTrees()
{
    grh::Design design;
    grh::Graph &graph = design.createGraph("top");

    const auto clk = graph.createValue(graph.internSymbol("clk"), 1, false);
    const auto rst = graph.createValue(graph.internSymbol("rst"), 1, false);
    const auto mask = makeConstant(graph, "mask", "mask_const", 8, "8'hff");
    const auto zero = makeConstant(graph, "zero", "zero_const", 8, "8'h00");
    const auto one = makeConstant(graph, "one", "one_const", 1, "1'b1");
    const auto data = graph.createValue(graph.internSymbol("data"), 8, false);

    const auto nextA = makeMux(graph, "next_a", "mux_a", rst, zero, data, 8);
    const auto nextB = makeMux(graph, "next_b", "mux_b", rst, zero, data, 8);

    const auto writeA = makeRegisterWrite(graph, "reg_write_a", one, nextA, mask, clk, "reg_a");
    const auto writeB = makeRegisterWrite(graph, "reg_write_b", one, nextB, mask, clk, "reg_b");

    SuperNodeGraph sg;
    const auto snA = sg.createSuperNode();
    const auto snB = sg.createSuperNode();
    sg.addMember(snA, writeA);
    sg.addMember(snB, writeB);
    sg.getNode(snA).timingDomain = "domain_0";
    sg.getNode(snB).timingDomain = "domain_0";

    SuperNodeCoarsener coarsener(sg, graph);
    coarsener.coarsen();

    expect(sg.nodeCount() == 1,
           "write ports with the same reset tree should merge");
}

void testPartitionerDoesNotMergeDifferentResetSemantics()
{
    grh::Design design;
    grh::Graph &graph = design.createGraph("top");

    const auto clk = graph.createValue(graph.internSymbol("clk"), 1, false);
    const auto rstA = graph.createValue(graph.internSymbol("rst_a"), 1, false);
    const auto rstB = graph.createValue(graph.internSymbol("rst_b"), 1, false);
    const auto mask = makeConstant(graph, "mask", "mask_const", 8, "8'hff");
    const auto zero = makeConstant(graph, "zero", "zero_const", 8, "8'h00");
    const auto one = makeConstant(graph, "one", "one_const", 1, "1'b1");
    const auto data = graph.createValue(graph.internSymbol("data"), 8, false);

    const auto nextA = makeMux(graph, "next_a", "mux_a", rstA, zero, data, 8);
    const auto nextB = makeMux(graph, "next_b", "mux_b", rstB, zero, data, 8);

    const auto writeA = makeRegisterWrite(graph, "reg_write_a", one, nextA, mask, clk, "reg_a");
    const auto writeB = makeRegisterWrite(graph, "reg_write_b", one, nextB, mask, clk, "reg_b");

    SuperNodeGraph sg;
    const auto snA = sg.createSuperNode();
    const auto snB = sg.createSuperNode();
    sg.addMember(snA, writeA);
    sg.addMember(snB, writeB);
    sg.getNode(snA).timingDomain = "domain_0";
    sg.getNode(snB).timingDomain = "domain_0";

    SuperNodePartitioner partitioner(sg, graph);
    partitioner.setMaxSuperNodeSize(8);
    partitioner.partition();

    expect(sg.nodeCount() == 2,
           "partitioner must not merge incompatible reset/control signatures");
}

void testPartitionerCanMergeIdenticalResetTrees()
{
    grh::Design design;
    grh::Graph &graph = design.createGraph("top");

    const auto clk = graph.createValue(graph.internSymbol("clk"), 1, false);
    const auto rst = graph.createValue(graph.internSymbol("rst"), 1, false);
    const auto mask = makeConstant(graph, "mask", "mask_const", 8, "8'hff");
    const auto zero = makeConstant(graph, "zero", "zero_const", 8, "8'h00");
    const auto one = makeConstant(graph, "one", "one_const", 1, "1'b1");
    const auto data = graph.createValue(graph.internSymbol("data"), 8, false);

    const auto nextA = makeMux(graph, "next_a", "mux_a", rst, zero, data, 8);
    const auto nextB = makeMux(graph, "next_b", "mux_b", rst, zero, data, 8);

    const auto writeA = makeRegisterWrite(graph, "reg_write_a", one, nextA, mask, clk, "reg_a");
    const auto writeB = makeRegisterWrite(graph, "reg_write_b", one, nextB, mask, clk, "reg_b");

    SuperNodeGraph sg;
    const auto snA = sg.createSuperNode();
    const auto snB = sg.createSuperNode();
    sg.addMember(snA, writeA);
    sg.addMember(snB, writeB);
    sg.getNode(snA).timingDomain = "domain_0";
    sg.getNode(snB).timingDomain = "domain_0";

    SuperNodePartitioner partitioner(sg, graph);
    partitioner.setMaxSuperNodeSize(8);
    partitioner.partition();

    expect(sg.nodeCount() == 1,
           "partitioner should still merge compatible reset/control signatures");
}

void testPartitionPassBuildsTotalGraphScratchpadCoverage()
{
    grh::Design design;
    grh::Graph &graph = design.createGraph("top");

    const auto lhs = graph.createValue(graph.internSymbol("lhs"), 8, false);
    const auto rhs = graph.createValue(graph.internSymbol("rhs"), 8, false);
    const auto dangling = graph.createValue(graph.internSymbol("dangling"), 8, false);
    const auto add = graph.createOperation(grh::OperationKind::kAdd,
                                           graph.internSymbol("dangling_add"));
    graph.addOperand(add, lhs);
    graph.addOperand(add, rhs);
    graph.addResult(add, dangling);

    const auto clk = graph.createValue(graph.internSymbol("clk"), 1, false);
    const auto data = graph.createValue(graph.internSymbol("data"), 8, false);
    const auto one = makeConstant(graph, "one", "one_const", 1, "1'b1");
    const auto mask = makeConstant(graph, "mask", "mask_const", 8, "8'hff");
    const auto write = makeRegisterWrite(graph, "reg_write", one, data, mask, clk, "reg_a");

    PassManager manager;
    manager.addPass(std::make_unique<SuperNodePartitionPass>());
    manager.addPass(std::make_unique<PartitionScratchpadChecker>(add, write));

    PassDiagnostics diags;
    const auto result = manager.run(design, diags);
    expect(result.success, "supernode partition pass should succeed on mixed sequential/combinational graph");
    expect(!diags.hasError(), "unexpected diagnostics while checking partition scratchpad coverage");
}

} // namespace

int main()
{
    try
    {
        testSuperNodeGraphBasics();
        testTopologicalSort();
        testCycleDetection();
        testMergeConstraints();
        testEdgeCounting();
        testMergeRejectsContractionThatCreatesCycle();
        testMergeAllowsDirectContractionWithoutIntroducingCycle();
        testTopologicalSortUsesStableCanonicalOrder();
        testTimingDomainAnalyzerDoesNotInventCrossDomainForSharedSameClockLogic();
        testCoarsenerDoesNotMergeDifferentResetSemantics();
        testCoarsenerMergesIdenticalResetTrees();
        testPartitionerDoesNotMergeDifferentResetSemantics();
        testPartitionerCanMergeIdenticalResetTrees();
        testPartitionPassBuildsTotalGraphScratchpadCoverage();
    }
    catch (const std::exception &ex)
    {
        return fail(ex.what());
    }

    std::cout << "All tests passed!\n";
    return 0;
}
