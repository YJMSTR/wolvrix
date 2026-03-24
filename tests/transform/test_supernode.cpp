#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/supernode_graph.hpp"
#include "transform/timing_domain_analyzer.hpp"
#include "transform/supernode_coarsener.hpp"
#include "transform/supernode_partitioner.hpp"
#include "transform/supernode_partition_pass.hpp"

#include <iostream>
#include <set>
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

grh::OperationId makeCombOp(grh::Graph &graph, std::string_view opName)
{
    return graph.createOperation(grh::OperationKind::kAdd, graph.internSymbol(opName));
}

std::vector<std::vector<uint32_t>> partitionLayout(const SuperNodeGraph &sg)
{
    std::vector<std::vector<uint32_t>> layout;
    for (const auto snId : sg.validNodeIds())
    {
        std::vector<uint32_t> members;
        for (const auto opId : sg.getNode(snId).members)
        {
            members.push_back(opId.index);
        }
        std::sort(members.begin(), members.end());
        layout.push_back(std::move(members));
    }
    std::sort(layout.begin(), layout.end());
    return layout;
}

class PartitionScratchpadChecker : public Pass
{
public:
    PartitionScratchpadChecker(std::set<uint32_t> expectedCombinationalOps,
                               std::set<uint32_t> expectedSequentialOps)
        : Pass("partition-scratchpad-checker", "partition-scratchpad-checker"),
          expectedCombinationalOps_(std::move(expectedCombinationalOps)),
          expectedSequentialOps_(std::move(expectedSequentialOps))
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
        using Members = std::unordered_map<SuperNodeId, std::vector<grh::OperationId>>;
        using Edges = std::unordered_map<SuperNodeId, std::vector<SuperNodeId>>;
        const auto *combinationalMap =
            getScratchpad<Mapping>("supernode.top.combinational.op_to_sn");
        const auto *sequentialMap =
            getScratchpad<Mapping>("supernode.top.domain_0.op_to_sn");
        if (combinationalMap == nullptr || sequentialMap == nullptr)
        {
            throw std::runtime_error("missing expected op_to_sn scratchpad keys");
        }

        const auto *combCount = getScratchpad<size_t>("supernode.top.combinational.count");
        const auto *seqCount = getScratchpad<size_t>("supernode.top.domain_0.count");
        const auto *combEdgeCount = getScratchpad<size_t>("supernode.top.combinational.edge_count");
        const auto *seqEdgeCount = getScratchpad<size_t>("supernode.top.domain_0.edge_count");
        const auto *combCutEdges = getScratchpad<size_t>("supernode.top.combinational.cut_edges");
        const auto *seqCutEdges = getScratchpad<size_t>("supernode.top.domain_0.cut_edges");
        const auto *combMaxSize = getScratchpad<size_t>("supernode.top.combinational.max_size");
        const auto *seqMaxSize = getScratchpad<size_t>("supernode.top.domain_0.max_size");
        const auto *combAvgSize = getScratchpad<double>("supernode.top.combinational.avg_size");
        const auto *seqAvgSize = getScratchpad<double>("supernode.top.domain_0.avg_size");
        const auto *combTiming = getScratchpad<std::string>("supernode.top.combinational.timing_domain");
        const auto *seqTiming = getScratchpad<std::string>("supernode.top.domain_0.timing_domain");
        const auto *combTopo = getScratchpad<std::vector<SuperNodeId>>("supernode.top.combinational.topo_order");
        const auto *seqTopo = getScratchpad<std::vector<SuperNodeId>>("supernode.top.domain_0.topo_order");
        const auto *combMembers = getScratchpad<Members>("supernode.top.combinational.sn_to_ops");
        const auto *seqMembers = getScratchpad<Members>("supernode.top.domain_0.sn_to_ops");
        const auto *combPreds = getScratchpad<Edges>("supernode.top.combinational.predecessors");
        const auto *seqPreds = getScratchpad<Edges>("supernode.top.domain_0.predecessors");
        const auto *combSuccs = getScratchpad<Edges>("supernode.top.combinational.successors");
        const auto *seqSuccs = getScratchpad<Edges>("supernode.top.domain_0.successors");
        const auto *crossDomainEdges =
            getScratchpad<std::vector<std::pair<grh::OperationId, grh::OperationId>>>(
                "supernode.top.cross_domain_edges");
        if (combCount == nullptr || seqCount == nullptr || combEdgeCount == nullptr ||
            seqEdgeCount == nullptr || combCutEdges == nullptr || seqCutEdges == nullptr ||
            combMaxSize == nullptr || seqMaxSize == nullptr || combAvgSize == nullptr ||
            seqAvgSize == nullptr || combTiming == nullptr || seqTiming == nullptr ||
            combTopo == nullptr || seqTopo == nullptr || combMembers == nullptr ||
            seqMembers == nullptr || combPreds == nullptr || seqPreds == nullptr ||
            combSuccs == nullptr || seqSuccs == nullptr || crossDomainEdges == nullptr)
        {
            throw std::runtime_error("missing extended scratchpad contract keys");
        }

        if (*seqCount != 1 || *seqEdgeCount != 0 || *seqCutEdges != 0 || *seqMaxSize == 0)
        {
            throw std::runtime_error(
                "unexpected sequential-domain statistics for minimal mixed graph: count=" +
                std::to_string(*seqCount) + " edge_count=" + std::to_string(*seqEdgeCount) +
                " cut_edges=" + std::to_string(*seqCutEdges) + " max_size=" +
                std::to_string(*seqMaxSize));
        }
        if (*combCount != combMembers->size() || *seqCount != seqMembers->size())
        {
            throw std::runtime_error("count metadata does not match sn_to_ops cardinality");
        }
        std::set<uint32_t> combinationalCovered;
        std::set<uint32_t> sequentialCovered;
        for (const auto &[opId, snId] : *combinationalMap)
        {
            combinationalCovered.insert(opId.index);
            if (combMembers->find(snId) == combMembers->end())
            {
                throw std::runtime_error("combinational op_to_sn references missing supernode");
            }
        }
        for (const auto &[opId, snId] : *sequentialMap)
        {
            sequentialCovered.insert(opId.index);
            if (seqMembers->find(snId) == seqMembers->end())
            {
                throw std::runtime_error("sequential op_to_sn references missing supernode");
            }
        }
        if (combinationalCovered != expectedCombinationalOps_)
        {
            throw std::runtime_error("combinational op_to_sn coverage does not match fixture operations");
        }
        if (sequentialCovered != expectedSequentialOps_)
        {
            throw std::runtime_error("sequential op_to_sn coverage does not match fixture operations");
        }
        if (combTopo->size() != *combCount || seqTopo->size() != *seqCount)
        {
            throw std::runtime_error("topological order size does not match domain node count");
        }
        if (combPreds->size() != *combCount || seqPreds->size() != *seqCount ||
            combSuccs->size() != *combCount || seqSuccs->size() != *seqCount)
        {
            throw std::runtime_error("graph-structure metadata does not cover every emitted supernode");
        }
        if (*combTiming != "combinational" || *seqTiming != "domain_0")
        {
            throw std::runtime_error("unexpected timing-domain or average-size metadata");
        }
        if (*combAvgSize <= 0.0 || *seqAvgSize <= 0.0 || *combMaxSize == 0)
        {
            throw std::runtime_error(
                "unexpected average/max size metadata: comb_avg=" +
                std::to_string(*combAvgSize) + " seq_avg=" + std::to_string(*seqAvgSize) +
                " comb_max=" + std::to_string(*combMaxSize) + " seq_max=" +
                std::to_string(*seqMaxSize));
        }
        if (!crossDomainEdges->empty())
        {
            throw std::runtime_error("unexpected cross-domain edges in single-domain sequential fixture");
        }
        if (*combCount != 1 || *seqCount != 1)
        {
            throw std::runtime_error("expected one supernode per domain in the minimal mixed fixture");
        }

        std::set<uint32_t> combinationalMembers;
        for (const auto &[snId, members] : *combMembers)
        {
            if (snId != (*combTopo)[0])
            {
                throw std::runtime_error("combinational topo_order should enumerate the emitted supernode id");
            }
            if (!combPreds->at(snId).empty() || !combSuccs->at(snId).empty())
            {
                throw std::runtime_error("combinational supernode should be isolated in the minimal mixed fixture");
            }
            for (const auto opId : members)
            {
                combinationalMembers.insert(opId.index);
            }
        }
        std::set<uint32_t> sequentialMembers;
        for (const auto &[snId, members] : *seqMembers)
        {
            if (snId != (*seqTopo)[0])
            {
                throw std::runtime_error("sequential topo_order should enumerate the emitted supernode id");
            }
            if (!seqPreds->at(snId).empty() || !seqSuccs->at(snId).empty())
            {
                throw std::runtime_error("sequential supernode should be isolated in the minimal mixed fixture");
            }
            for (const auto opId : members)
            {
                sequentialMembers.insert(opId.index);
            }
        }
        if (combinationalMembers != expectedCombinationalOps_ ||
            sequentialMembers != expectedSequentialOps_)
        {
            throw std::runtime_error("sn_to_ops membership does not exactly match the fixture contract");
        }
        return {};
    }

private:
    std::set<uint32_t> expectedCombinationalOps_;
    std::set<uint32_t> expectedSequentialOps_;
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

void testCoarsenerAllowsResetWriteToMergeWithCombinationalFanIn()
{
    grh::Design design;
    grh::Graph &graph = design.createGraph("top");

    const auto clk = graph.createValue(graph.internSymbol("clk"), 1, false);
    const auto rst = graph.createValue(graph.internSymbol("rst"), 1, false);
    const auto lhs = graph.createValue(graph.internSymbol("lhs"), 8, false);
    const auto rhs = graph.createValue(graph.internSymbol("rhs"), 8, false);
    const auto addOut = graph.createValue(graph.internSymbol("add_out"), 8, false);
    const auto add = graph.createOperation(grh::OperationKind::kAdd, graph.internSymbol("fanin_add"));
    graph.addOperand(add, lhs);
    graph.addOperand(add, rhs);
    graph.addResult(add, addOut);

    const auto zero = makeConstant(graph, "zero", "zero_const", 8, "8'h00");
    const auto one = makeConstant(graph, "one", "one_const", 1, "1'b1");
    const auto mask = makeConstant(graph, "mask", "mask_const", 8, "8'hff");
    const auto next = makeMux(graph, "next", "next_mux", rst, zero, addOut, 8);
    const auto write = makeRegisterWrite(graph, "reg_write", one, next, mask, clk, "reg_a");

    SuperNodeGraph sg;
    const auto snComb = sg.createSuperNode();
    const auto snWrite = sg.createSuperNode();
    sg.addMember(snComb, add);
    sg.addMember(snWrite, write);
    sg.getNode(snComb).timingDomain = "domain_0";
    sg.getNode(snWrite).timingDomain = "domain_0";
    sg.getNode(snComb).successors.insert(snWrite);
    sg.getNode(snWrite).predecessors.insert(snComb);

    SuperNodeCoarsener coarsener(sg, graph);
    coarsener.coarsen();

    expect(sg.nodeCount() == 1,
           "coarsener should still merge control-sensitive sequential nodes with combinational fan-in");
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

void testPartitionerAllowsResetWriteToMergeWithCombinationalFanIn()
{
    grh::Design design;
    grh::Graph &graph = design.createGraph("top");

    const auto clk = graph.createValue(graph.internSymbol("clk"), 1, false);
    const auto rst = graph.createValue(graph.internSymbol("rst"), 1, false);
    const auto lhs = graph.createValue(graph.internSymbol("lhs"), 8, false);
    const auto rhs = graph.createValue(graph.internSymbol("rhs"), 8, false);
    const auto addOut = graph.createValue(graph.internSymbol("add_out"), 8, false);
    const auto add = graph.createOperation(grh::OperationKind::kAdd, graph.internSymbol("fanin_add"));
    graph.addOperand(add, lhs);
    graph.addOperand(add, rhs);
    graph.addResult(add, addOut);

    const auto zero = makeConstant(graph, "zero", "zero_const", 8, "8'h00");
    const auto one = makeConstant(graph, "one", "one_const", 1, "1'b1");
    const auto mask = makeConstant(graph, "mask", "mask_const", 8, "8'hff");
    const auto next = makeMux(graph, "next", "next_mux", rst, zero, addOut, 8);
    const auto write = makeRegisterWrite(graph, "reg_write", one, next, mask, clk, "reg_a");

    SuperNodeGraph sg;
    const auto snComb = sg.createSuperNode();
    const auto snWrite = sg.createSuperNode();
    sg.addMember(snComb, add);
    sg.addMember(snWrite, write);
    sg.getNode(snComb).timingDomain = "domain_0";
    sg.getNode(snWrite).timingDomain = "domain_0";
    sg.getNode(snComb).successors.insert(snWrite);
    sg.getNode(snWrite).predecessors.insert(snComb);

    SuperNodePartitioner partitioner(sg, graph);
    partitioner.setMaxSuperNodeSize(8);
    partitioner.partition();

    expect(sg.nodeCount() == 1,
           "control-sensitive sequential nodes should still merge with compatible combinational fan-in");
}

void testPartitionerFindsStableTwoWayCut()
{
    grh::Design design;
    grh::Graph &graph = design.createGraph("top");

    SuperNodeGraph sg;
    std::vector<SuperNodeId> nodeIds;
    for (int i = 0; i < 4; ++i)
    {
        const auto op = makeCombOp(graph, "op" + std::to_string(i));
        const auto sn = sg.createSuperNode();
        sg.addMember(sn, op);
        sg.getNode(sn).timingDomain = "combinational";
        nodeIds.push_back(sn);
    }

    for (size_t i = 0; i + 1 < nodeIds.size(); ++i)
    {
        sg.getNode(nodeIds[i]).successors.insert(nodeIds[i + 1]);
        sg.getNode(nodeIds[i + 1]).predecessors.insert(nodeIds[i]);
    }

    const auto beforeNodeCount = sg.nodeCount();
    const auto beforeEdgeCount = sg.edgeCount();

    SuperNodePartitioner partitioner(sg, graph);
    partitioner.setMaxSuperNodeSize(2);
    partitioner.partition();

    expect(sg.nodeCount() == 2, "chain of four singletons should partition into two intervals under max size 2");
    expect(beforeNodeCount == 4 && beforeEdgeCount == 3,
           "expected known pre-partition validation baseline for the chain fixture");
    expect(sg.edgeCount() == 1 && sg.edgeCount() < beforeEdgeCount,
           "partition should reduce cut edges on the validation chain fixture");
    const auto layout = partitionLayout(sg);
    expect(layout.size() == 2 && layout[0].size() == 2 && layout[1].size() == 2,
           "expected stable two-by-two partition layout");
}

void testPartitionerRejectsImpossibleSizeConstraint()
{
    grh::Design design;
    grh::Graph &graph = design.createGraph("top");

    SuperNodeGraph sg;
    const auto op = makeCombOp(graph, "only_op");
    const auto sn = sg.createSuperNode();
    sg.addMember(sn, op);
    sg.getNode(sn).timingDomain = "combinational";

    SuperNodePartitioner partitioner(sg, graph);
    partitioner.setMaxSuperNodeSize(0);

    bool threw = false;
    try
    {
        partitioner.partition();
    }
    catch (const std::runtime_error &)
    {
        threw = true;
    }
    expect(threw, "partitioner should reject impossible size constraints");
}

void testPartitionerProducesStableLayoutAcrossRuns()
{
    grh::Design designA;
    grh::Graph &graphA = designA.createGraph("top");
    grh::Design designB;
    grh::Graph &graphB = designB.createGraph("top");

    auto buildChain = [](grh::Graph &graph) {
        SuperNodeGraph sg;
        std::vector<SuperNodeId> nodeIds;
        for (int i = 0; i < 4; ++i)
        {
            const auto op = makeCombOp(graph, "stable_op" + std::to_string(i));
            const auto sn = sg.createSuperNode();
            sg.addMember(sn, op);
            sg.getNode(sn).timingDomain = "combinational";
            nodeIds.push_back(sn);
        }
        for (size_t i = 0; i + 1 < nodeIds.size(); ++i)
        {
            sg.getNode(nodeIds[i]).successors.insert(nodeIds[i + 1]);
            sg.getNode(nodeIds[i + 1]).predecessors.insert(nodeIds[i]);
        }
        return sg;
    };

    auto sgA = buildChain(graphA);
    auto sgB = buildChain(graphB);

    SuperNodePartitioner partitionerA(sgA, graphA);
    partitionerA.setMaxSuperNodeSize(2);
    partitionerA.partition();
    SuperNodePartitioner partitionerB(sgB, graphB);
    partitionerB.setMaxSuperNodeSize(2);
    partitionerB.partition();

    expect(partitionLayout(sgA) == partitionLayout(sgB),
           "repeated runs on equivalent graphs should produce the same partition layout");
}

void testPartitionerReducesEdgesOnBranchedDagFixture()
{
    grh::Design design;
    grh::Graph &graph = design.createGraph("top");

    SuperNodeGraph sg;
    std::vector<SuperNodeId> nodeIds;
    for (int i = 0; i < 6; ++i)
    {
        const auto op = makeCombOp(graph, "branch_op" + std::to_string(i));
        const auto sn = sg.createSuperNode();
        sg.addMember(sn, op);
        sg.getNode(sn).timingDomain = "combinational";
        nodeIds.push_back(sn);
    }

    auto connect = [&](size_t from, size_t to) {
        sg.getNode(nodeIds[from]).successors.insert(nodeIds[to]);
        sg.getNode(nodeIds[to]).predecessors.insert(nodeIds[from]);
    };
    connect(0, 2);
    connect(1, 2);
    connect(2, 3);
    connect(2, 4);
    connect(3, 5);
    connect(4, 5);

    const auto beforeNodes = sg.nodeCount();
    const auto beforeEdges = sg.edgeCount();

    SuperNodePartitioner partitioner(sg, graph);
    partitioner.setMaxSuperNodeSize(2);
    partitioner.partition();

    expect(beforeNodes == 6 && beforeEdges == 6,
           "expected known branched-DAG baseline before partitioning");
    expect(!sg.hasCircularDependency(), "partitioned branched fixture must remain acyclic");
    expect(sg.nodeCount() < beforeNodes, "partition should reduce node count on branched DAG fixture");
    expect(sg.edgeCount() < beforeEdges, "partition should reduce edge count on branched DAG fixture");
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

    const auto oneOp = graph.getValue(one).definingOp();
    const auto maskOp = graph.getValue(mask).definingOp();

    PassManager manager;
    manager.addPass(std::make_unique<SuperNodePartitionPass>());
    manager.addPass(std::make_unique<PartitionScratchpadChecker>(
        std::set<uint32_t>{add.index},
        std::set<uint32_t>{oneOp.index, maskOp.index, write.index}));

    PassDiagnostics diags;
    const auto result = manager.run(design, diags);
    expect(result.success, "supernode partition pass should succeed on mixed sequential/combinational graph");
    expect(!diags.hasError(), "unexpected diagnostics while checking partition scratchpad coverage");
}

void testPartitionPassRejectsSharedCrossDomainLogic()
{
    grh::Design design;
    grh::Graph &graph = design.createGraph("top");

    const auto dataA = graph.createValue(graph.internSymbol("data_a"), 8, false);
    const auto dataB = graph.createValue(graph.internSymbol("data_b"), 8, false);
    const auto sharedValue = graph.createValue(graph.internSymbol("shared"), 8, false);
    const auto sharedAdd = graph.createOperation(grh::OperationKind::kAdd, graph.internSymbol("shared_add"));
    graph.addOperand(sharedAdd, dataA);
    graph.addOperand(sharedAdd, dataB);
    graph.addResult(sharedAdd, sharedValue);

    const auto clkA = graph.createValue(graph.internSymbol("clk_a"), 1, false);
    const auto clkB = graph.createValue(graph.internSymbol("clk_b"), 1, false);
    const auto one = makeConstant(graph, "one", "one_const", 1, "1'b1");
    const auto mask = makeConstant(graph, "mask", "mask_const", 8, "8'hff");
    makeRegisterWrite(graph, "reg_write_a", one, sharedValue, mask, clkA, "reg_a");
    makeRegisterWrite(graph, "reg_write_b", one, sharedValue, mask, clkB, "reg_b");

    PassManager manager;
    manager.addPass(std::make_unique<SuperNodePartitionPass>());

    PassDiagnostics diags;
    const auto result = manager.run(design, diags);
    expect(!result.success, "shared combinational logic across timing domains should follow the conservative failure path");
    expect(diags.hasError(), "expected diagnostics for shared cross-domain logic");
    expect(!diags.messages().empty(), "expected recorded diagnostics for shared cross-domain logic");
    bool sawExpectedDiagnostic = false;
    for (const auto &diag : diags.messages())
    {
        if (diag.passName == "supernode-partition" &&
            diag.message == "Design contains shared combinational logic across timing domains")
        {
            sawExpectedDiagnostic = true;
            break;
        }
    }
    expect(sawExpectedDiagnostic, "expected exact supernode-partition cross-domain diagnostic");
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
        testCoarsenerAllowsResetWriteToMergeWithCombinationalFanIn();
        testPartitionerDoesNotMergeDifferentResetSemantics();
        testPartitionerCanMergeIdenticalResetTrees();
        testPartitionerAllowsResetWriteToMergeWithCombinationalFanIn();
        testPartitionerFindsStableTwoWayCut();
        testPartitionerRejectsImpossibleSizeConstraint();
        testPartitionerProducesStableLayoutAcrossRuns();
        testPartitionerReducesEdgesOnBranchedDagFixture();
        testPartitionPassBuildsTotalGraphScratchpadCoverage();
        testPartitionPassRejectsSharedCrossDomainLogic();
    }
    catch (const std::exception &ex)
    {
        return fail(ex.what());
    }

    std::cout << "All tests passed!\n";
    return 0;
}
