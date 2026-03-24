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
        testTimingDomainAnalyzerDoesNotInventCrossDomainForSharedSameClockLogic();
    }
    catch (const std::exception &ex)
    {
        return fail(ex.what());
    }

    std::cout << "All tests passed!\n";
    return 0;
}
