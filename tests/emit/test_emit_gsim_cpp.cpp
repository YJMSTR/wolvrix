#include "emit/gsim_cpp.hpp"
#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/gsim.hpp"

#include <filesystem>
#include <fstream>
#include <cstdlib>
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

ValueId makeRegister(Graph &graph,
                     const std::string &valueName,
                     const std::string &opName,
                     int32_t width,
                     const std::string &regSymbol)
{
    const auto value = graph.createValue(graph.internSymbol(valueName), width, false);
    const auto op = graph.createOperation(OperationKind::kRegister, graph.internSymbol(opName));
    graph.addResult(op, value);
    graph.setAttr(op, "regSymbol", regSymbol);
    graph.setAttr(op, "width", static_cast<int64_t>(width));
    graph.setAttr(op, "isSigned", false);
    graph.setAttr(op, "initValue", std::string("8'd0"));
    return value;
}

ValueId makeRegisterRead(Graph &graph,
                         const std::string &valueName,
                         const std::string &opName,
                         int32_t width,
                         const std::string &regSymbol)
{
    const auto value = graph.createValue(graph.internSymbol(valueName), width, false);
    const auto op = graph.createOperation(OperationKind::kRegisterReadPort, graph.internSymbol(opName));
    graph.addResult(op, value);
    graph.setAttr(op, "regSymbol", regSymbol);
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

Design buildStatefulOutputDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto inA = makeValue(graph, "a", 8, false);
    const auto clk = makeValue(graph, "clk", 1, false);
    graph.bindInputPort("a", inA);
    graph.bindInputPort("clk", clk);

    const auto regStorage = makeRegister(graph, "state_storage", "state_reg", 8, "state");
    (void)regStorage;
    const auto stateRead = makeRegisterRead(graph, "state_read", "state_read_op", 8, "state");
    graph.bindOutputPort("y", stateRead);

    const auto one = makeConstant(graph, "one", "one_const", 1, "1'b1");
    const auto mask = makeConstant(graph, "mask", "mask_const", 8, "8'hff");
    makeRegisterWrite(graph, "reg_write", one, inA, mask, clk, "state");
    return design;
}

Design buildConcatDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto inA = makeValue(graph, "a", 4, false);
    const auto inB = makeValue(graph, "b", 4, false);
    graph.bindInputPort("a", inA);
    graph.bindInputPort("b", inB);

    const auto outY = makeValue(graph, "y", 8, false);
    graph.bindOutputPort("y", outY);

    const auto concat = graph.createOperation(OperationKind::kConcat, graph.internSymbol("concat_y"));
    graph.addOperand(concat, inA);
    graph.addOperand(concat, inB);
    graph.addResult(concat, outY);

    return design;
}

Design buildDpicImportNoOpDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto inA = makeValue(graph, "a", 8, false);
    const auto inB = makeValue(graph, "b", 8, false);
    graph.bindInputPort("a", inA);
    graph.bindInputPort("b", inB);

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

    const auto dpiImport = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("dpi_capture"));
    graph.setAttr(dpiImport, "argsDirection", std::vector<std::string>{"input"});
    graph.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{8});
    graph.setAttr(dpiImport, "argsName", std::vector<std::string>{"value"});
    graph.setAttr(dpiImport, "argsSigned", std::vector<bool>{false});
    graph.setAttr(dpiImport, "argsType", std::vector<std::string>{"logic"});
    graph.setAttr(dpiImport, "hasReturn", false);
    graph.setAttr(dpiImport, "returnType", std::string("void"));

    return design;
}

Design buildEqDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto inA = makeValue(graph, "a", 8, false);
    const auto inB = makeValue(graph, "b", 8, false);
    graph.bindInputPort("a", inA);
    graph.bindInputPort("b", inB);

    const auto outY = makeValue(graph, "y", 1, false);
    graph.bindOutputPort("y", outY);

    const auto eq = graph.createOperation(OperationKind::kEq, graph.internSymbol("eq_y"));
    graph.addOperand(eq, inA);
    graph.addOperand(eq, inB);
    graph.addResult(eq, outY);

    return design;
}

Design buildMaskedNotDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto in = makeValue(graph, "in", 1, false);
    graph.bindInputPort("in", in);

    const auto out = makeValue(graph, "out", 1, false);
    graph.bindOutputPort("out", out);

    const auto notOp = graph.createOperation(OperationKind::kNot, graph.internSymbol("not_out"));
    graph.addOperand(notOp, in);
    graph.addResult(notOp, out);

    return design;
}

Design buildSliceDynamicDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto in = makeValue(graph, "in", 8, false);
    const auto index = makeValue(graph, "index", 3, false);
    graph.bindInputPort("in", in);
    graph.bindInputPort("index", index);

    const auto out = makeValue(graph, "out", 3, false);
    graph.bindOutputPort("out", out);

    const auto slice = graph.createOperation(OperationKind::kSliceDynamic, graph.internSymbol("dyn_slice"));
    graph.addOperand(slice, in);
    graph.addOperand(slice, index);
    graph.addResult(slice, out);
    graph.setAttr(slice, "sliceWidth", static_cast<int64_t>(3));

    return design;
}

Design buildNoCommitStatefulOutputDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto inA = makeValue(graph, "a", 8, false);
    const auto clk = makeValue(graph, "clk", 1, false);
    graph.bindInputPort("a", inA);
    graph.bindInputPort("clk", clk);

    const auto regStorage = makeRegister(graph, "state_storage", "state_reg", 8, "state");
    (void)regStorage;
    const auto stateRead = makeRegisterRead(graph, "state_read", "state_read_op", 8, "state");
    graph.bindOutputPort("y", stateRead);

    const auto zero = makeConstant(graph, "zero", "zero_const", 1, "1'b0");
    const auto mask = makeConstant(graph, "mask", "mask_const", 8, "8'hff");
    makeRegisterWrite(graph, "reg_write", zero, inA, mask, clk, "state");
    return design;
}

Design buildWideMuxDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto inA = makeValue(graph, "a", 128, false);
    const auto inB = makeValue(graph, "b", 128, false);
    const auto sel = makeValue(graph, "sel", 1, false);
    graph.bindInputPort("a", inA);
    graph.bindInputPort("b", inB);
    graph.bindInputPort("sel", sel);

    const auto outY = makeValue(graph, "y", 128, false);
    graph.bindOutputPort("y", outY);

    const auto mux = graph.createOperation(OperationKind::kMux, graph.internSymbol("wide_mux"));
    graph.addOperand(mux, sel);
    graph.addOperand(mux, inB);
    graph.addOperand(mux, inA);
    graph.addResult(mux, outY);

    return design;
}

Design buildThreeStageRegisterPipelineDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto clk = makeValue(graph, "clk", 1, false);
    const auto inD = makeValue(graph, "d", 1, false);
    graph.bindInputPort("clk", clk);
    graph.bindInputPort("d", inD);

    (void)makeRegister(graph, "stage1_storage", "stage1_reg", 1, "stage1");
    (void)makeRegister(graph, "stage2_storage", "stage2_reg", 1, "stage2");
    (void)makeRegister(graph, "stage3_storage", "stage3_reg", 1, "stage3");

    const auto stage1Read = makeRegisterRead(graph, "stage1_read", "stage1_read_op", 1, "stage1");
    const auto stage2Read = makeRegisterRead(graph, "stage2_read", "stage2_read_op", 1, "stage2");
    const auto stage3Read = makeRegisterRead(graph, "stage3_read", "stage3_read_op", 1, "stage3");
    graph.bindOutputPort("q", stage3Read);

    const auto one = makeConstant(graph, "one", "one_const", 1, "1'b1");
    const auto mask = makeConstant(graph, "mask", "mask_const", 1, "1'b1");
    makeRegisterWrite(graph, "stage1_write", one, inD, mask, clk, "stage1");
    makeRegisterWrite(graph, "stage2_write", one, stage1Read, mask, clk, "stage2");
    makeRegisterWrite(graph, "stage3_write", one, stage2Read, mask, clk, "stage3");

    return design;
}

Design buildKeyClockCarrierDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto key = makeValue(graph, "KEY", 2, false);
    const auto d = makeValue(graph, "D", 1, false);
    graph.bindInputPort("KEY", key);
    graph.bindInputPort("D", d);

    (void)makeRegister(graph, "q_storage", "q_reg", 1, "q");
    const auto qRead = makeRegisterRead(graph, "q_read", "q_read_op", 1, "q");
    graph.bindOutputPort("Q", qRead);

    const auto idx0 = makeConstant(graph, "idx0", "idx0_const", 1, "1'b0");
    const auto idx1 = makeConstant(graph, "idx1", "idx1_const", 1, "1'b1");
    const auto clkBit = makeValue(graph, "clk_bit", 1, false);
    const auto enBit = makeValue(graph, "en_bit", 1, false);

    const auto clkSlice = graph.createOperation(OperationKind::kSliceDynamic, graph.internSymbol("key_clk_slice"));
    graph.addOperand(clkSlice, key);
    graph.addOperand(clkSlice, idx0);
    graph.addResult(clkSlice, clkBit);
    graph.setAttr(clkSlice, "sliceWidth", static_cast<int64_t>(1));

    const auto enSlice = graph.createOperation(OperationKind::kSliceDynamic, graph.internSymbol("key_en_slice"));
    graph.addOperand(enSlice, key);
    graph.addOperand(enSlice, idx1);
    graph.addResult(enSlice, enBit);
    graph.setAttr(enSlice, "sliceWidth", static_cast<int64_t>(1));

    const auto mask = makeConstant(graph, "mask", "mask_const", 1, "1'b1");
    const auto write = graph.createOperation(OperationKind::kRegisterWritePort, graph.internSymbol("q_write"));
    graph.addOperand(write, enBit);
    graph.addOperand(write, d);
    graph.addOperand(write, mask);
    graph.addOperand(write, clkBit);
    graph.setAttr(write, "regSymbol", std::string("q"));
    graph.setAttr(write, "clockSymbol", std::string("clock"));
    graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});

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

void compileAndRunHarness(const std::filesystem::path &dir,
                          const std::string &baseName,
                          const std::string &sourceText)
{
    const std::filesystem::path runnerPath = dir / (baseName + "_runner.cpp");
    const std::filesystem::path binaryPath = dir / (baseName + "_runner");
    {
        std::ofstream out(runnerPath);
        if (!out.is_open())
        {
            throw std::runtime_error("failed to write runtime harness source");
        }
        out << sourceText;
    }

    const char *compiler = std::getenv("CXX");
    const std::string cxx = (compiler && *compiler) ? compiler : "c++";
    const std::string compileCmd =
        cxx + " -std=c++20 -I " + dir.string() + " " + runnerPath.string() + " -o " + binaryPath.string();
    if (std::system(compileCmd.c_str()) != 0)
    {
        throw std::runtime_error("failed to compile emitted runtime harness");
    }
    if (std::system(binaryPath.string().c_str()) != 0)
    {
        throw std::runtime_error("emitted runtime harness execution failed");
    }
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
    expect(result.artifacts.size() == 3, "EmitGsimCpp should report header, source, and manifest artifacts");

    const std::filesystem::path headerPath = dir / "top_metadata.hpp";
    const std::filesystem::path sourcePath = dir / "top_metadata.cpp";
    const std::filesystem::path manifestPath = dir / "top_metadata.manifest";
    expect(std::filesystem::exists(headerPath), "EmitGsimCpp should create header artifact");
    expect(std::filesystem::exists(sourcePath), "EmitGsimCpp should create source artifact");
    expect(std::filesystem::exists(manifestPath), "EmitGsimCpp should create manifest artifact");

    const std::string header = readFile(headerPath);
    const std::string source = readFile(sourcePath);
    const std::string manifest = readFile(manifestPath);
    expect(contains(header, "class SSimTop"), "header should expose the downstream simulator-facing SSimTop class");
    expect(contains(header, "void set_reset(unsigned reset)"), "header should expose set_reset for downstream GSIM runtime");
    expect(contains(header, "void settle()"), "header should expose settle for downstream GSIM runtime");
    expect(contains(header, "void commit_step()"), "header should expose commit_step for downstream GSIM runtime");
    expect(contains(header, "void step()"), "header should expose step for downstream GSIM runtime");
    expect(contains(header, "get_difftest__DOT__uart__DOT__out__DOT__valid()"), "header should expose downstream UART out valid accessor");
    expect(contains(header, "get_difftest__DOT__uart__DOT__out__DOT__ch()"), "header should expose downstream UART out char accessor");
    expect(contains(header, "get_difftest__DOT__uart__DOT__in__DOT__valid()"), "header should expose downstream UART in valid accessor");
    expect(contains(header, "set_difftest__DOT__uart__DOT__in__DOT__ch"), "header should expose downstream UART input mutator");
    expect(contains(header, "get_difftest__DOT__exit()"), "header should expose difftest exit accessor");
    expect(contains(header, "get_difftest__DOT__step()"), "header should expose difftest step accessor");
    expect(!contains(header, "difftest_exit_ = 1;"), "generated downstream step should not force difftest exit on every non-reset step");
    expect(!contains(header, "// output_y_ = ...;"), "small emitted models should not leave output placeholders in the runtime");
    expect(contains(header, "output_y_ = (input_a_ + input_b_);"), "small emitted models should lower output behavior into executable assignments");
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
    expect(contains(manifest, "top_metadata.cpp"), "manifest should list the canonical main source");
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

void testSingleClockRuntimeCompileAndRun()
{
    Design design = buildStatefulOutputDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "runtime_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("runtime_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp runtime fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp runtime fixture should not emit errors");
    const std::string header = readFile(dir / "runtime_top.hpp");
    expect(contains(header, "void settle()"), "runtime fixture should expose settle");
    expect(contains(header, "void commit_step()"), "runtime fixture should expose commit_step");
    expect(!contains(header, "\n        ++difftest_step_;\n"), "runtime fixture should not increment difftest_step unconditionally");
    expect(contains(header, "if (committed_) { ++difftest_step_; }"), "runtime fixture should gate difftest_step increments on committed sequential work");
    expect(contains(header, "output_y_ = reg_state;"), "runtime fixture should drive emitted outputs from executable state expressions");

    const std::string runner = R"CPP(
#include "runtime_top.hpp"
#include <cstdint>

int main() {
    SSimTop sim;
    sim.set_a(9);
    sim.set_clk(0);
    sim.settle();
    sim.commit_step();
    if (sim.get_y() != 0) {
        return 1;
    }
    if (sim.get_difftest__DOT__step() != 0) {
        return 2;
    }
    sim.set_reset(0);
    sim.set_clk(1);
    sim.settle();
    sim.commit_step();
    if (sim.get_y() != 9) {
        return 3;
    }
    if (sim.get_difftest__DOT__step() != 1) {
        return 4;
    }
    sim.set_clk(1);
    sim.step();
    if (sim.get_difftest__DOT__step() != 1) {
        return 5;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "runtime_top", runner);
}

void testDpicImportNoOpCompileAndRun()
{
    Design design = buildDpicImportNoOpDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "dpic_import_noop_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("dpic_import_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp should ignore standalone kDpicImport ops");
    expect(!diags.hasError(), "EmitGsimCpp should not report standalone kDpicImport ops as unsupported");

    const std::string header = readFile(dir / "dpic_import_top.hpp");
    expect(contains(header, "output_y_ = (input_a_ + input_b_);"),
           "dpi-import no-op fixture should still lower surrounding logic");

    const std::string runner = R"CPP(
#include "dpic_import_top.hpp"
#include <cstdint>

int main() {
    SSimTop sim;
    sim.set_a(3);
    sim.set_b(4);
    sim.step();
    if (sim.get_y() != 7) {
        return 1;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "dpic_import_top", runner);
}

void testConcatCompileAndRun()
{
    Design design = buildConcatDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "concat_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("concat_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp concat fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp concat fixture should not emit errors");

    const std::string runner = R"CPP(
#include "concat_top.hpp"
#include <cstdint>

int main() {
    SSimTop sim;
    sim.set_a(0xA);
    sim.set_b(0x3);
    sim.step();
    if (sim.get_y() != 0xA3) {
        return 1;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "concat_top", runner);
}

void testEqCompileAndRun()
{
    Design design = buildEqDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "eq_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("eq_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp eq fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp eq fixture should not emit errors");

    const std::string runner = R"CPP(
#include "eq_top.hpp"
#include <cstdint>

int main() {
    SSimTop sim;
    sim.set_a(0x5A);
    sim.set_b(0x5A);
    sim.step();
    if (sim.get_y() != 1) {
        return 1;
    }
    sim.set_a(0x5A);
    sim.set_b(0xA5);
    sim.step();
    if (sim.get_y() != 0) {
        return 2;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "eq_top", runner);
}

void testBitwiseNotMasksToDeclaredWidth()
{
    Design design = buildMaskedNotDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "masked_not_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("masked_not_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp masked-not fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp masked-not fixture should not emit errors");

    const std::string runner = R"CPP(
#include "masked_not_top.hpp"
#include <cstdint>

int main() {
    SSimTop sim;
    sim.set_in(0);
    sim.step();
    if (sim.get_out() != 1) {
        return 1;
    }
    sim.set_in(1);
    sim.step();
    if (sim.get_out() != 0) {
        return 2;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "masked_not_top", runner);
}

void testDynamicSliceCompileAndRun()
{
    Design design = buildSliceDynamicDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "slice_dynamic_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("slice_dynamic_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp slice-dynamic fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp slice-dynamic fixture should not emit errors");

    const std::string header = readFile(dir / "slice_dynamic_top.hpp");
    expect(contains(header, "output_out_ ="), "slice-dynamic fixture should lower the output assignment");
    expect(contains(header, ">= 8") && contains(header, "? 0ULL"),
           "slice-dynamic lowering should zero-fill out-of-range shifts instead of clamping the index");

    const std::string runner = R"CPP(
#include "slice_dynamic_top.hpp"
#include <cstdint>

int main() {
    SSimTop sim;
    sim.set_in(0b10110110);
    sim.set_index(0);
    sim.step();
    if (sim.get_out() != 0b110) {
        return 1;
    }
    sim.set_index(2);
    sim.step();
    if (sim.get_out() != 0b101) {
        return 2;
    }
    sim.set_index(7);
    sim.step();
    if (sim.get_out() != 0b001) {
        return 3;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "slice_dynamic_top", runner);
}

void testWideVectorPortsInitializeAndCompile()
{
    Design design = buildWideMuxDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "wide_vector_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("wide_vector_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp wide-vector fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp wide-vector fixture should not emit errors");

    const std::string header = readFile(dir / "wide_vector_top.hpp");
    expect(contains(header, "std::vector<std::uint64_t> input_a_ = {}"),
           "wide-vector fixture should initialize vector inputs with {}");
    expect(contains(header, "std::vector<std::uint64_t> output_y_ = {}"),
           "wide-vector fixture should initialize vector outputs with {}");

    const std::string runner = R"CPP(
#include "wide_vector_top.hpp"
#include <cstdint>
#include <vector>

int main() {
    SSimTop sim;
    sim.set_a(std::vector<std::uint64_t>{0x1ULL, 0x2ULL});
    sim.set_b(std::vector<std::uint64_t>{0xAULL, 0xBULL});
    sim.set_sel(1);
    sim.step();
    const auto out = sim.get_y();
    if (out.size() != 2 || out[0] != 0xAULL || out[1] != 0xBULL) {
        return 1;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "wide_vector_top", runner);
}

void testRegisterPipelineUsesNonBlockingSemantics()
{
    Design design = buildThreeStageRegisterPipelineDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "pipeline_runtime";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("pipeline_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp pipeline fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp pipeline fixture should not emit errors");

    const std::string runner = R"CPP(
#include "pipeline_top.hpp"
#include <array>
#include <cstdint>

static void tick(SSimTop& sim, std::uint8_t d) {
    sim.set_d(d);
    sim.set_clk(0);
    sim.step();
    sim.set_clk(1);
    sim.step();
}

int main() {
    SSimTop sim;
    const std::array<std::uint8_t, 7> stimuli{{1, 0, 1, 1, 0, 0, 1}};
    std::uint8_t stage1 = 0;
    std::uint8_t stage2 = 0;
    for (const auto din : stimuli) {
        tick(sim, din);
        if (sim.get_q() != stage2) {
            return 1;
        }
        const auto nextStage1 = din;
        const auto nextStage2 = stage1;
        stage1 = nextStage1;
        stage2 = nextStage2;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "pipeline_top", runner);
}

void testKeyBitClockCarrierDoesNotEmitMissingInputClockAlias()
{
    Design design = buildKeyClockCarrierDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "key_clock_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("key_clock_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp key-clock fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp key-clock fixture should not emit errors");

    const std::string header = readFile(dir / "key_clock_top.hpp");
    expect(!contains(header, "input_clock_"),
           "key-clock fixture should not emit unresolved input_clock_ alias");
    expect(contains(header, "input_KEY_"),
           "key-clock fixture should drive the clock from the KEY input carrier");

    const std::string runner = R"CPP(
#include "key_clock_top.hpp"
#include <cstdint>

int main() {
    SSimTop sim;
    sim.set_KEY(0b10);
    sim.set_D(1);
    sim.step();
    sim.set_KEY(0b11);
    sim.step();
    if (sim.get_Q() != 1) {
        return 1;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "key_clock_top", runner);
}

void testEmitMetadataToggleSkipsLargeMetadataPayload()
{
    Design design = buildConcatDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "emit_metadata_toggle";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("metadata_toggle_top");
    options.topOverrides = {"top"};
    options.attributes["emit_metadata"] = "0";

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp metadata-toggle fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp metadata-toggle fixture should not emit errors");

    const std::string header = readFile(dir / "metadata_toggle_top.hpp");
    const std::string source = readFile(dir / "metadata_toggle_top.cpp");
    expect(!contains(header, "std::vector<std::int64_t> roots"),
           "emit_metadata=0 should omit heavyweight metadata fields from the header");
    expect(!contains(source, "metadata.op_descriptors"),
           "emit_metadata=0 should omit heavyweight metadata population from the source");
    expect(contains(source, "return metadata;"),
           "emit_metadata=0 should still emit a lightweight metadata factory");
}

void testEdgeWithoutCommitDoesNotAdvanceStep()
{
    Design design = buildNoCommitStatefulOutputDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "no_commit_runtime";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("no_commit_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp no-commit fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp no-commit fixture should not emit errors");

    const std::string header = readFile(dir / "no_commit_top.hpp");
    expect(!contains(header, "input_clock_"),
           "sequential runtime should not reference missing input_clock_ alias when the input port is clk");
    expect(contains(header, "input_clk_"),
           "sequential runtime should keep the declared input_clk_ storage name");

    const std::string runner = R"CPP(
#include "no_commit_top.hpp"
#include <cstdint>

int main() {
    SSimTop sim;
    sim.set_a(9);
    sim.set_clk(0);
    sim.set_reset(0);
    sim.settle();
    sim.commit_step();
    if (sim.get_difftest__DOT__step() != 0) {
        return 1;
    }
    sim.set_clk(1);
    sim.settle();
    sim.commit_step();
    if (sim.get_difftest__DOT__step() != 0) {
        return 2;
    }
    if (sim.get_y() != 0) {
        return 3;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "no_commit_top", runner);
}

} // namespace

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
        testSingleClockRuntimeCompileAndRun();
        testDpicImportNoOpCompileAndRun();
        testConcatCompileAndRun();
        testEqCompileAndRun();
        testBitwiseNotMasksToDeclaredWidth();
        testDynamicSliceCompileAndRun();
        testWideVectorPortsInitializeAndCompile();
        testRegisterPipelineUsesNonBlockingSemantics();
        testKeyBitClockCarrierDoesNotEmitMissingInputClockAlias();
        testEmitMetadataToggleSkipsLargeMetadataPayload();
        testEdgeWithoutCommitDoesNotAdvanceStep();
    }
    catch (const std::exception &ex)
    {
        return fail(ex.what());
    }
    return 0;
}
