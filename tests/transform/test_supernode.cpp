#include "transform/supernode_graph.hpp"
#include "transform/timing_domain_analyzer.hpp"
#include "transform/supernode_coarsener.hpp"
#include "transform/supernode_partitioner.hpp"
#include <cassert>
#include <iostream>

using namespace wolvrix::lib::transform;

void testSuperNodeGraphBasics() {
    SuperNodeGraph sg;

    // Test create
    auto id1 = sg.createSuperNode();
    auto id2 = sg.createSuperNode();
    assert(sg.nodeCount() == 2);

    // Test merge
    sg.merge(id1, id2);
    assert(sg.nodeCount() == 1);

    std::cout << "SuperNodeGraph basic tests passed\n";
}

void testTopologicalSort() {
    SuperNodeGraph sg;

    auto id1 = sg.createSuperNode();
    auto id2 = sg.createSuperNode();
    auto id3 = sg.createSuperNode();

    // Build DAG: 1 -> 2 -> 3
    sg.getNode(id1).successors.insert(id2);
    sg.getNode(id2).predecessors.insert(id1);
    sg.getNode(id2).successors.insert(id3);
    sg.getNode(id3).predecessors.insert(id2);

    auto sorted = sg.topologicalSort();
    assert(sorted.size() == 3);
    assert(!sg.hasCircularDependency());

    std::cout << "Topological sort tests passed\n";
}

int main() {
    testSuperNodeGraphBasics();
    testTopologicalSort();

    std::cout << "All tests passed!\n";
    return 0;
}
