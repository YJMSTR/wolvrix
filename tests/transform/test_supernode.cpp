#include "core/grh.hpp"
#include "transform/supernode_graph.hpp"
#include "transform/timing_domain_analyzer.hpp"
#include "transform/supernode_coarsener.hpp"
#include "transform/supernode_partitioner.hpp"

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
    }
    catch (const std::exception &ex)
    {
        return fail(ex.what());
    }

    std::cout << "All tests passed!\n";
    return 0;
}
