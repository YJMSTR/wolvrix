#include "emit/gsim_cpp.hpp"
#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/gsim.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace wolvrix::lib::emit;
using namespace wolvrix::lib::grh;
using namespace wolvrix::lib::transform;

namespace
{

int fail(const std::string &message)
{
    std::cerr << "[emit-gsim-cpp] " << message << '\n';
    return 1;
}

void expect(bool condition, const std::string &message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::string readFile(const std::filesystem::path &path)
{
    std::ifstream stream(path);
    if (!stream.is_open())
    {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

bool contains(std::string_view text, std::string_view needle)
{
    return text.find(needle) != std::string_view::npos;
}

ValueId makeValue(Graph &graph,
                  const std::string &name,
                  int32_t width = 1,
                  bool isSigned = false)
{
    return graph.createValue(graph.internSymbol(name), width, isSigned);
}

ValueId makeConstant(Graph &graph,
                     const std::string &valueName,
                     const std::string &opName,
                     int32_t width,
                     const std::string &literal)
{
    const auto value = graph.createValue(graph.internSymbol(valueName), width, false);
    const auto op = graph.createOperation(OperationKind::kConstant,
                                          graph.internSymbol(opName));
    graph.addResult(op, value);
    graph.setAttr(op, "value", literal);
    graph.setAttr(op, "width", static_cast<int64_t>(width));
    graph.setAttr(op, "isSigned", false);
    return value;
}

OperationId makeRegisterWrite(Graph &graph,
                              const std::string &opName,
                              ValueId updateCond,
                              ValueId nextValue,
                              ValueId maskValue,
                              ValueId clk,
                              const std::string &regSymbol)
{
    const auto op = graph.createOperation(OperationKind::kRegisterWritePort,
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

void addInstance(Graph &graph,
                 std::string_view instanceName,
                 std::string_view moduleName,
                 std::vector<ValueId> operands,
                 std::vector<ValueId> results,
                 std::vector<std::string> inputPortNames,
                 std::vector<std::string> outputPortNames)
{
    const auto op = graph.createOperation(OperationKind::kInstance, graph.internSymbol(std::string(instanceName) + "_op"));
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
}

Design buildSingleGraphDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto inA = makeValue(graph, "a", 8, false);
    const auto inB = makeValue(graph, "b", 8, false);
    const auto clk = makeValue(graph, "clk", 1, false);
    graph.bindInputPort("a", inA);
    graph.bindInputPort("b", inB);
    graph.bindInputPort("clk", clk);

    const auto addOut = makeValue(graph, "sum", 8, false);
    const auto outY = makeValue(graph, "y", 8, false);
    graph.bindOutputPort("y", outY);
    const auto add = graph.createOperation(OperationKind::kAdd, graph.internSymbol("add"));
    graph.addOperand(add, inA);
    graph.addOperand(add, inB);
    graph.addResult(add, addOut);

    const auto assign = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_y"));
    graph.addOperand(assign, addOut);
    graph.addResult(assign, outY);

    const auto one = makeConstant(graph, "one", "one_const", 1, "1'b1");
    const auto mask = makeConstant(graph, "mask", "mask_const", 8, "8'hff");
    const auto write = makeRegisterWrite(graph, "reg_write", one, addOut, mask, clk, "state");

    const auto dbg = graph.createOperation(OperationKind::kSystemTask, graph.internSymbol("display"));
    graph.addOperand(dbg, addOut);

    (void)write;
    return design;
}

Design buildHierDesign()
{
    Design design;
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
    const auto leafAdd = leaf.createOperation(OperationKind::kAdd, leaf.internSymbol("leaf_add"));
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
    addInstance(mid,
                "u_leaf",
                "leaf",
                {midA, midB, midClk},
                {midY},
                {"a", "b", "clk"},
                {"y"});

    const auto topA = makeValue(top, "a", 8, false);
    const auto topB = makeValue(top, "b", 8, false);
    const auto topClk = makeValue(top, "clk", 1, false);
    const auto topY = makeValue(top, "y", 8, false);
    top.bindInputPort("a", topA);
    top.bindInputPort("b", topB);
    top.bindInputPort("clk", topClk);
    top.bindOutputPort("y", topY);
    addInstance(top,
                "u_mid",
                "mid",
                {topA, topB, topClk},
                {topY},
                {"a", "b", "clk"},
                {"y"});

    return design;
}

Design buildCrossRootSharedLeafDesign()
{
    Design design;
    auto &leaf = design.createGraph("leaf");
    auto &top0 = design.createGraph("top0");
    auto &top1 = design.createGraph("top1");
    design.markAsTop("top0");
    design.markAsTop("top1");

    const auto leafA = makeValue(leaf, "a", 8, false);
    const auto leafB = makeValue(leaf, "b", 8, false);
    const auto leafClk = makeValue(leaf, "clk", 1, false);
    const auto leafY = makeValue(leaf, "y", 8, false);
    leaf.bindInputPort("a", leafA);
    leaf.bindInputPort("b", leafB);
    leaf.bindInputPort("clk", leafClk);
    leaf.bindOutputPort("y", leafY);
    const auto leafSum = makeValue(leaf, "sum", 8, false);
    const auto leafAdd = leaf.createOperation(OperationKind::kAdd, leaf.internSymbol("leaf_add"));
    leaf.addOperand(leafAdd, leafA);
    leaf.addOperand(leafAdd, leafB);
    leaf.addResult(leafAdd, leafSum);
    const auto leafAssign = leaf.createOperation(OperationKind::kAssign, leaf.internSymbol("leaf_assign_y"));
    leaf.addOperand(leafAssign, leafSum);
    leaf.addResult(leafAssign, leafY);
    const auto one = makeConstant(leaf, "one", "one_const", 1, "1'b1");
    const auto mask = makeConstant(leaf, "mask", "mask_const", 8, "8'hff");
    makeRegisterWrite(leaf, "leaf_reg_write", one, leafSum, mask, leafClk, "state");

    for (auto *top : {&top0, &top1})
    {
        const auto a = makeValue(*top, "a", 8, false);
        const auto b = makeValue(*top, "b", 8, false);
        const auto clk = makeValue(*top, "clk", 1, false);
        const auto y = makeValue(*top, "y", 8, false);
        top->bindInputPort("a", a);
        top->bindInputPort("b", b);
        top->bindInputPort("clk", clk);
        top->bindOutputPort("y", y);
        addInstance(*top,
                    "u_leaf",
                    "leaf",
                    {a, b, clk},
                    {y},
                    {"a", "b", "clk"},
                    {"y"});
    }

    return design;
}

void runGsim(Design &design, const std::string &path)
{
    PassManager manager;
    manager.addPass(std::make_unique<GsimPass>(GsimOptions{path}));
    PassDiagnostics diags;
    const auto result = manager.run(design, diags);
    expect(result.success, "gsim should succeed for test fixture");
    expect(!diags.hasError(), "gsim should not emit diagnostics for test fixture");
}

void expectDiagnosticsContain(const EmitDiagnostics &diags, std::string_view needle)
{
    bool found = false;
    for (const auto &diag : diags.messages())
    {
        if (diag.message.find(needle) != std::string::npos || diag.context.find(needle) != std::string::npos)
        {
            found = true;
            break;
        }
    }
    expect(found, std::string("expected diagnostics to contain: ") + std::string(needle));
}

std::filesystem::path artifactRoot()
{
#ifndef WOLF_SV_EMIT_ARTIFACT_DIR
#error "WOLF_SV_EMIT_ARTIFACT_DIR must be defined"
#endif
    return std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "gsim_cpp";
}

void cleanDir(const std::filesystem::path &path)
{
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

void testHappyPathAfterRunningGsim()
{
    Design design = buildSingleGraphDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "happy";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("top_metadata");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp happy path should succeed");
    expect(!diags.hasError(), "EmitGsimCpp happy path should not emit errors");
    expect(result.artifacts.size() == 2, "EmitGsimCpp should report header and source artifacts");

    const std::filesystem::path headerPath = dir / "top_metadata.hpp";
    const std::filesystem::path sourcePath = dir / "top_metadata.cpp";
    expect(std::filesystem::exists(headerPath), "EmitGsimCpp should create header artifact");
    expect(std::filesystem::exists(sourcePath), "EmitGsimCpp should create source artifact");

    const std::string header = readFile(headerPath);
    const std::string source = readFile(sourcePath);
    expect(contains(header, "class SSimTop"), "header should expose the downstream simulator-facing SSimTop class");
    expect(contains(header, "void set_reset(unsigned reset)"), "header should expose set_reset for downstream GSIM runtime");
    expect(contains(header, "void step()"), "header should expose step for downstream GSIM runtime");
    expect(contains(header, "get_difftest__DOT__uart__DOT__out__DOT__valid()"), "header should expose downstream UART out valid accessor");
    expect(contains(header, "get_difftest__DOT__uart__DOT__out__DOT__ch()"), "header should expose downstream UART out char accessor");
    expect(contains(header, "get_difftest__DOT__uart__DOT__in__DOT__valid()"), "header should expose downstream UART in valid accessor");
    expect(contains(header, "set_difftest__DOT__uart__DOT__in__DOT__ch"), "header should expose downstream UART input mutator");
    expect(contains(header, "get_difftest__DOT__exit()"), "header should expose difftest exit accessor");
    expect(contains(header, "get_difftest__DOT__step()"), "header should expose difftest step accessor");
    expect(!contains(header, "difftest_exit_ = 1;"), "generated downstream step should not force difftest exit on every non-reset step");
    expect(contains(header, "set_difftest__DOT__perfCtrl__DOT__clean"), "header should expose perf clean mutator");
    expect(contains(header, "set_difftest__DOT__perfCtrl__DOT__dump"), "header should expose perf dump mutator");
    expect(contains(header, "set_difftest__DOT__logCtrl__DOT__begin"), "header should expose log begin mutator");
    expect(contains(header, "set_difftest__DOT__logCtrl__DOT__end"), "header should expose log end mutator");
    expect(contains(header, "struct GsimMetadata_top"), "header should still declare graph-specific metadata struct");
    expect(contains(source, "metadata.graph_symbol = \"top\";"), "source should embed graph symbol from scratchpad metadata");
    expect(contains(source, "metadata.scratchpad_namespace = \"gsim.top\";"), "source should embed scratchpad namespace");
    expect(contains(source, "metadata.schedule_kind = \"activity-v1\";"), "source should emit concrete schedule contract kind");
    expect(contains(source, "metadata.hypergraph_kind = \"activity-connectivity-v1\";"), "source should emit concrete hypergraph contract kind");
    expect(contains(source, "metadata.schedule_activity_order = {"), "source should serialize schedule activity ordering");
    expect(contains(source, "metadata.hypergraph_edge_sinks = {"), "source should serialize hypergraph sink metadata");
    expect(contains(source, "bool validate_top_metadata"), "source should emit validation helper");

    // Compile verification: generated C++ must compile with strict flags
    {
        const std::filesystem::path wrapperPath = dir / "compile_check.cpp";
        {
            std::ofstream wrapper(wrapperPath);
            wrapper << "#include \"top_metadata.hpp\"\n";
            wrapper << "#include \"top_metadata.cpp\"\n";
            wrapper << "int main() { SSimTop sim; sim.set_reset(1); sim.step(); return 0; }\n";
        }
        const std::string compileCmd =
            "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
            " -fsyntax-only " + wrapperPath.string() + " 2>&1";
        const int compileResult = std::system(compileCmd.c_str());
        expect(compileResult == 0, "emitted C++ should compile with -std=c++17 -Wall -Wextra -Werror");
    }
}

void testFailureWithoutPriorMetadata()
{
    Design design = buildSingleGraphDesign();

    const auto dir = artifactRoot() / "missing_metadata";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(!result.success, "EmitGsimCpp should fail without prior gsim metadata");
    expect(diags.hasError(), "EmitGsimCpp should emit diagnostics for missing metadata");
    expectDiagnosticsContain(diags, "missing required gsim scratchpad metadata");
}

void testFailureOnPlaceholderContract()
{
    Design design = buildSingleGraphDesign();
    runGsim(design, "top");
    design.setScratchpad(std::string("gsim.top.schedule.kind"), std::string("placeholder"));

    const auto dir = artifactRoot() / "placeholder_contract";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(!result.success, "EmitGsimCpp should reject placeholder schedule metadata");
    expect(diags.hasError(), "placeholder schedule metadata should produce diagnostics");
    expectDiagnosticsContain(diags, "schedule metadata contract mismatch");
}

void testFailureOnNamespacePathMismatch()
{
    Design design = buildSingleGraphDesign();
    runGsim(design, "top");
    design.setScratchpad(std::string("gsim.top.graph_symbol"), std::string("wrong_graph"));

    const auto dir = artifactRoot() / "namespace_mismatch";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(!result.success, "EmitGsimCpp should fail on namespace/path mismatch");
    expect(diags.hasError(), "EmitGsimCpp should emit diagnostics for namespace/path mismatch");
    expectDiagnosticsContain(diags, "gsim scratchpad namespace/path mismatch");
}

void testFailureOnStaleMetadataAfterMutation()
{
    Design design = buildSingleGraphDesign();
    runGsim(design, "top");

    auto *graph = design.findGraph("top");
    expect(graph != nullptr, "stale metadata fixture should resolve top graph");
    const auto staleInput = makeValue(*graph, "stale_added_in", 8, false);
    const auto staleOutput = makeValue(*graph, "stale_added_out", 8, false);
    const auto staleOp = graph->createOperation(OperationKind::kNot, graph->internSymbol("stale_not"));
    graph->addOperand(staleOp, staleInput);
    graph->addResult(staleOp, staleOutput);

    const auto dir = artifactRoot() / "stale_metadata";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(!result.success, "EmitGsimCpp should reject stale metadata after graph mutation");
    expect(diags.hasError(), "stale metadata should produce diagnostics");
    expectDiagnosticsContain(diags, "gsim scratchpad metadata is stale");
}

void testFailureOnStaleMetadataAfterDestructiveMutation()
{
    Design design = buildSingleGraphDesign();
    runGsim(design, "top");

    auto *graph = design.findGraph("top");
    expect(graph != nullptr, "destructive stale metadata fixture should resolve top graph");
    const auto assignOp = graph->findOperation("assign_y");
    expect(assignOp.valid(), "destructive stale metadata fixture should find output assign op");
    expect(graph->removeOutputPort("y"), "destructive stale metadata fixture should remove output port");
    expect(graph->eraseOpUnchecked(assignOp), "destructive stale metadata fixture should erase the output assign op");

    const auto dir = artifactRoot() / "stale_metadata_destructive";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(!result.success, "EmitGsimCpp should reject stale metadata after destructive graph mutation");
    expect(diags.hasError(), "destructive stale metadata should produce diagnostics");
    expectDiagnosticsContain(diags, "gsim scratchpad metadata is stale");
}

void testGraphOnlyAndMultiHopTargetSelectionConsistency()
{
    {
        Design design = buildHierDesign();
        runGsim(design, "leaf");

        const auto dir = artifactRoot() / "graph_only_path";
        cleanDir(dir);

        EmitDiagnostics diags;
        EmitGsimCpp emitter(&diags);
        EmitOptions options;
        options.outputDir = dir.string();
        options.attributes["path"] = "leaf";
        options.topOverrides = {"top"};

        const EmitResult result = emitter.emit(design, options);
        expect(result.success, "EmitGsimCpp graph-only path selection should succeed");
        expect(!diags.hasError(), "EmitGsimCpp graph-only path selection should not emit errors");

        const std::string source = readFile(dir / "gsim_leaf.cpp");
        expect(contains(source, "metadata.selection_path = \"leaf\";"), "graph-only selection should preserve selection path");
        expect(contains(source, "metadata.scratchpad_namespace = \"gsim.leaf\";"), "graph-only selection should use leaf namespace");
    }

    {
        Design design = buildHierDesign();
        runGsim(design, "top.u_mid.u_leaf");

        const auto dir = artifactRoot() / "multi_hop_path";
        cleanDir(dir);

        EmitDiagnostics diags;
        EmitGsimCpp emitter(&diags);
        EmitOptions options;
        options.outputDir = dir.string();
        options.attributes["path"] = "top.u_mid.u_leaf";
        options.topOverrides = {"top"};

        const EmitResult result = emitter.emit(design, options);
        expect(result.success, "EmitGsimCpp multi-hop path selection should succeed");
        expect(!diags.hasError(), "EmitGsimCpp multi-hop path selection should not emit errors");

        const std::string source = readFile(dir / "gsim_leaf.cpp");
        expect(contains(source, "metadata.selection_path = \"top.u_mid.u_leaf\";"), "multi-hop selection should preserve instance path");
        expect(contains(source, "metadata.scratchpad_namespace = \"gsim.leaf.path.top$u_mid$u_leaf\";"), "multi-hop selection should keep instance-scoped namespace");
        expect(contains(source, "metadata.graph_symbol = \"leaf\";"), "multi-hop selection should resolve to leaf graph metadata");
    }
}

void testCrossRootInstancePathsStayDistinct()
{
    Design design = buildCrossRootSharedLeafDesign();
    runGsim(design, "top0.u_leaf");
    runGsim(design, "top1.u_leaf");

    {
        const auto dir = artifactRoot() / "cross_root_top0";
        cleanDir(dir);
        EmitDiagnostics diags;
        EmitGsimCpp emitter(&diags);
        EmitOptions options;
        options.outputDir = dir.string();
        options.attributes["path"] = "top0.u_leaf";
        options.topOverrides = {"top0", "top1"};
        const EmitResult result = emitter.emit(design, options);
        expect(result.success, "EmitGsimCpp should succeed for top0 shared-leaf path");
        const std::string source = readFile(dir / "gsim_leaf.cpp");
        expect(contains(source, "metadata.scratchpad_namespace = \"gsim.leaf.path.top0$u_leaf\";"),
               "top0 shared-leaf path should use root-qualified namespace");
    }

    {
        const auto dir = artifactRoot() / "cross_root_top1";
        cleanDir(dir);
        EmitDiagnostics diags;
        EmitGsimCpp emitter(&diags);
        EmitOptions options;
        options.outputDir = dir.string();
        options.attributes["path"] = "top1.u_leaf";
        options.topOverrides = {"top0", "top1"};
        const EmitResult result = emitter.emit(design, options);
        expect(result.success, "EmitGsimCpp should succeed for top1 shared-leaf path");
        const std::string source = readFile(dir / "gsim_leaf.cpp");
        expect(contains(source, "metadata.scratchpad_namespace = \"gsim.leaf.path.top1$u_leaf\";"),
               "top1 shared-leaf path should use distinct root-qualified namespace");
    }
}

} // namespace

void testBehavioralCompileAndRun()
{
    // Build a simple combinational design: y = a & b
    Design design = buildSingleGraphDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "behavioral";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("sim_test");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "behavioral test emit should succeed");

    // Write test driver
    const std::filesystem::path driverPath = dir / "driver.cpp";
    {
        std::ofstream driver(driverPath);
        driver << "#include \"sim_test.hpp\"\n";
        driver << "#include \"sim_test.cpp\"\n";
        driver << "#include <cstdio>\n";
        driver << "int main() {\n";
        driver << "    SSimTop sim;\n";
        driver << "    sim.set_reset(1); sim.step();\n";
        driver << "    sim.set_a(3); sim.set_b(5); sim.step();\n";
        driver << "    if (sim.get_y() != 8) { printf(\"FAIL: y=%d expected 8\\n\", sim.get_y()); return 1; }\n";
        driver << "    printf(\"BEHAVIORAL PASS\\n\");\n";
        driver << "    return 0;\n";
        driver << "}\n";
    }

    // Compile and run
    const std::string exePath = (dir / "driver").string();
    const std::string compileCmd = "g++ -std=c++17 -Wall -Wextra -Werror -I " +
        dir.string() + " -o " + exePath + " " + driverPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "behavioral driver should compile");

    const std::string runCmd = exePath + " 2>&1";
    expect(std::system(runCmd.c_str()) == 0, "behavioral driver should run and pass");
}

void testPortOrderDecl()
{
    Design design = buildSingleGraphDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "port_order_decl";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("po_decl");
    options.topOverrides = {"top"};
    options.portOrderStrategy = PortOrderStrategy::Decl;

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "port_order=decl should succeed");
}

void testPortOrderAlpha()
{
    Design design = buildSingleGraphDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "port_order_alpha";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("po_alpha");
    options.topOverrides = {"top"};
    options.portOrderStrategy = PortOrderStrategy::Alpha;

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "port_order=alpha should succeed");

    // Verify alpha ordering: ports should appear in alphabetical order in the header
    const std::string header = readFile(dir / "po_alpha.hpp");
    auto posA = header.find("set_a(");
    auto posB = header.find("set_b(");
    auto posClk = header.find("set_clk(");
    expect(posA != std::string::npos && posB != std::string::npos && posClk != std::string::npos,
           "alpha-ordered header should contain all input port setters");
    expect(posA < posB && posB < posClk, "alpha ordering should put a < b < clk");
}

void testPortOrderCustom()
{
    Design design = buildSingleGraphDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "port_order_custom";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("po_custom");
    options.topOverrides = {"top"};
    options.portOrderStrategy = PortOrderStrategy::Custom;
    options.portOrderNames = {"clk", "b", "a", "y"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "port_order=custom should succeed");

    // Verify custom ordering: clk, b, a in header
    const std::string header = readFile(dir / "po_custom.hpp");
    auto posClk = header.find("set_clk(");
    auto posB = header.find("set_b(");
    auto posA = header.find("set_a(");
    expect(posClk != std::string::npos && posB != std::string::npos && posA != std::string::npos,
           "custom-ordered header should contain all input port setters");
    expect(posClk < posB && posB < posA, "custom ordering should put clk < b < a");
}

void testPortOrderInvalidName()
{
    Design design = buildSingleGraphDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "port_order_invalid";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("po_invalid");
    options.topOverrides = {"top"};
    options.portOrderStrategy = PortOrderStrategy::Custom;
    options.portOrderNames = {"nonexistent_port"};

    const EmitResult result = emitter.emit(design, options);
    expect(!result.success, "port_order=custom with nonexistent name should fail");
    expect(diags.hasError(), "should emit error for nonexistent port name");
}

int main()
{
    try
    {
        testHappyPathAfterRunningGsim();
        testFailureWithoutPriorMetadata();
        testFailureOnPlaceholderContract();
        testFailureOnNamespacePathMismatch();
        testFailureOnStaleMetadataAfterMutation();
        testFailureOnStaleMetadataAfterDestructiveMutation();
        testGraphOnlyAndMultiHopTargetSelectionConsistency();
        testCrossRootInstancePathsStayDistinct();
        testBehavioralCompileAndRun();
        testPortOrderDecl();
        testPortOrderAlpha();
        testPortOrderCustom();
        testPortOrderInvalidName();
    }
    catch (const std::exception &ex)
    {
        return fail(ex.what());
    }
    return 0;
}
