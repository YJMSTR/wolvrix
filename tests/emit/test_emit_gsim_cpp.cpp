#include "emit/gsim_cpp.hpp"
#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/gsim.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
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

std::vector<std::string> readLines(const std::filesystem::path &path)
{
    std::vector<std::string> lines;
    std::ifstream stream(path);
    if (!stream.is_open())
    {
        return lines;
    }
    std::string line;
    while (std::getline(stream, line))
    {
        if (!line.empty())
        {
            lines.push_back(line);
        }
    }
    return lines;
}

std::vector<std::filesystem::path> findMetadataShards(const std::filesystem::path &dir,
                                                      std::string_view baseName)
{
    std::vector<std::filesystem::path> shards;
    if (!std::filesystem::exists(dir))
    {
        return shards;
    }
    const std::string prefix = std::string(baseName) + "__meta_";
    for (const auto &entry : std::filesystem::directory_iterator(dir))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (entry.path().extension() == ".cpp" && name.rfind(prefix, 0) == 0)
        {
            shards.push_back(entry.path());
        }
    }
    std::sort(shards.begin(), shards.end());
    return shards;
}

std::vector<std::filesystem::path> findBehaviorShards(const std::filesystem::path &dir,
                                                      std::string_view baseName)
{
    std::vector<std::filesystem::path> shards;
    if (!std::filesystem::exists(dir))
    {
        return shards;
    }
    const std::string prefix = std::string(baseName) + "__step_";
    for (const auto &entry : std::filesystem::directory_iterator(dir))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (entry.path().extension() == ".cpp" && name.rfind(prefix, 0) == 0)
        {
            shards.push_back(entry.path());
        }
    }
    std::sort(shards.begin(), shards.end());
    return shards;
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

Design buildReplicateSignExtendDesign()
{
    Design design;
    auto &graph = design.createGraph("signext_top");
    design.markAsTop("signext_top");

    const auto in = makeValue(graph, "in", 8, false);
    const auto out = makeValue(graph, "out", 32, false);
    graph.bindInputPort("in", in);
    graph.bindOutputPort("out", out);

    const auto signBit = makeValue(graph, "sign_bit", 1, false);
    const auto slice = graph.createOperation(OperationKind::kSliceStatic, graph.internSymbol("slice_sign"));
    graph.addOperand(slice, in);
    graph.addResult(slice, signBit);
    graph.setAttr(slice, "sliceStart", static_cast<int64_t>(7));
    graph.setAttr(slice, "sliceEnd", static_cast<int64_t>(7));

    const auto replicated = makeValue(graph, "replicated_sign", 24, false);
    const auto replicate = graph.createOperation(OperationKind::kReplicate, graph.internSymbol("replicate_sign"));
    graph.addOperand(replicate, signBit);
    graph.addResult(replicate, replicated);
    graph.setAttr(replicate, "rep", static_cast<int64_t>(24));

    const auto concatValue = makeValue(graph, "concat_out", 32, false);
    const auto concat = graph.createOperation(OperationKind::kConcat, graph.internSymbol("concat_signext"));
    graph.addOperand(concat, replicated);
    graph.addOperand(concat, in);
    graph.addResult(concat, concatValue);

    const auto assign = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_out"));
    graph.addOperand(assign, concatValue);
    graph.addResult(assign, out);

    return design;
}

Design buildLinearAddChainDesign(std::size_t depth)
{
    Design design;
    auto &graph = design.createGraph("chain_top");
    design.markAsTop("chain_top");

    const auto in = makeValue(graph, "in", 8, false);
    graph.bindInputPort("in", in);

    ValueId current = in;
    for (std::size_t i = 0; i < depth; ++i)
    {
        const auto one = makeConstant(graph,
                                      "chain_one_" + std::to_string(i),
                                      "chain_one_const_" + std::to_string(i),
                                      8,
                                      "8'h01");
        const auto next = makeValue(graph, "chain_val_" + std::to_string(i), 8, false);
        const auto add = graph.createOperation(OperationKind::kAdd, graph.internSymbol("chain_add_" + std::to_string(i)));
        graph.addOperand(add, current);
        graph.addOperand(add, one);
        graph.addResult(add, next);
        current = next;
    }

    const auto out = makeValue(graph, "y", 8, false);
    graph.bindOutputPort("y", out);
    const auto assign = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_chain_out"));
    graph.addOperand(assign, current);
    graph.addResult(assign, out);

    return design;
}

Design buildMemoryBehaviorDesign()
{
    Design design;
    auto &graph = design.createGraph("mem_top");
    design.markAsTop("mem_top");

    const auto clk = makeValue(graph, "clk", 1, false);
    const auto wen = makeValue(graph, "wen", 1, false);
    const auto raddr = makeValue(graph, "raddr", 2, false);
    const auto waddr = makeValue(graph, "waddr", 2, false);
    const auto wdata = makeValue(graph, "wdata", 8, false);
    graph.bindInputPort("clk", clk);
    graph.bindInputPort("wen", wen);
    graph.bindInputPort("raddr", raddr);
    graph.bindInputPort("waddr", waddr);
    graph.bindInputPort("wdata", wdata);

    const auto out = makeValue(graph, "y", 8, false);
    graph.bindOutputPort("y", out);

    const auto memMask = makeConstant(graph, "mem_mask", "mem_mask_const", 8, "8'hff");

    const auto mem = graph.createOperation(OperationKind::kMemory, graph.internSymbol("mem0"));
    graph.setAttr(mem, "width", static_cast<int64_t>(8));
    graph.setAttr(mem, "row", static_cast<int64_t>(4));
    graph.setAttr(mem, "isSigned", false);
    graph.setAttr(mem, "initKind", std::vector<std::string>{"literal"});
    graph.setAttr(mem, "initFile", std::vector<std::string>{""});
    graph.setAttr(mem, "initValue", std::vector<std::string>{"8'h00"});
    graph.setAttr(mem, "initStart", std::vector<int64_t>{-1});
    graph.setAttr(mem, "initLen", std::vector<int64_t>{0});

    const auto memRead = graph.createOperation(OperationKind::kMemoryReadPort, graph.internSymbol("mem0_read"));
    graph.setAttr(memRead, "memSymbol", std::string("mem0"));
    graph.addOperand(memRead, raddr);
    const auto readData = makeValue(graph, "read_data", 8, false);
    graph.addResult(memRead, readData);

    const auto assign = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_y"));
    graph.addOperand(assign, readData);
    graph.addResult(assign, out);

    const auto memWrite = graph.createOperation(OperationKind::kMemoryWritePort, graph.internSymbol("mem0_write"));
    graph.setAttr(memWrite, "memSymbol", std::string("mem0"));
    graph.setAttr(memWrite, "clockSymbol", std::string("clk"));
    graph.setAttr(memWrite, "eventEdge", std::vector<std::string>{"posedge"});
    graph.addOperand(memWrite, wen);
    graph.addOperand(memWrite, waddr);
    graph.addOperand(memWrite, wdata);
    graph.addOperand(memWrite, memMask);
    graph.addOperand(memWrite, clk);

    return design;
}

Design buildDpicIgnoredDesign()
{
    Design design;
    auto &graph = design.createGraph("dpic_top");
    design.markAsTop("dpic_top");

    const auto clk = makeValue(graph, "clk", 1, false);
    const auto in = makeValue(graph, "in", 8, false);
    graph.bindInputPort("clk", clk);
    graph.bindInputPort("in", in);

    const auto out = makeValue(graph, "y", 8, false);
    graph.bindOutputPort("y", out);
    const auto assign = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_y"));
    graph.addOperand(assign, in);
    graph.addResult(assign, out);

    const auto cond = makeConstant(graph, "dpi_cond", "dpi_cond_const", 1, "1'b1");
    const auto import = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("dpi_capture"));
    graph.setAttr(import, "argsDirection", std::vector<std::string>{"input"});
    graph.setAttr(import, "argsWidth", std::vector<int64_t>{8});
    graph.setAttr(import, "argsName", std::vector<std::string>{"value"});
    graph.setAttr(import, "argsSigned", std::vector<bool>{false});
    graph.setAttr(import, "argsType", std::vector<std::string>{"logic"});
    graph.setAttr(import, "hasReturn", false);
    graph.setAttr(import, "returnWidth", static_cast<int64_t>(0));
    graph.setAttr(import, "returnSigned", false);
    graph.setAttr(import, "returnType", std::string("void"));

    const auto call = graph.createOperation(OperationKind::kDpicCall, graph.internSymbol("dpi_call"));
    graph.setAttr(call, "targetImportSymbol", std::string("dpi_capture"));
    graph.setAttr(call, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(call, "inArgName", std::vector<std::string>{"value"});
    graph.setAttr(call, "outArgName", std::vector<std::string>{});
    graph.setAttr(call, "inoutArgName", std::vector<std::string>{});
    graph.setAttr(call, "hasReturn", false);
    graph.addOperand(call, cond);
    graph.addOperand(call, in);
    graph.addOperand(call, clk);

    return design;
}

Design buildDpicReturnDesign()
{
    Design design;
    auto &graph = design.createGraph("dpic_ret_top");
    design.markAsTop("dpic_ret_top");

    const auto clk = makeValue(graph, "clk", 1, false);
    const auto in = makeValue(graph, "in", 8, false);
    graph.bindInputPort("clk", clk);
    graph.bindInputPort("in", in);

    const auto out = makeValue(graph, "y", 8, false);
    graph.bindOutputPort("y", out);

    const auto cond = makeConstant(graph, "dpi_cond", "dpi_cond_const", 1, "1'b1");
    const auto import = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("dpi_add1"));
    graph.setAttr(import, "argsDirection", std::vector<std::string>{"input"});
    graph.setAttr(import, "argsWidth", std::vector<int64_t>{8});
    graph.setAttr(import, "argsName", std::vector<std::string>{"value"});
    graph.setAttr(import, "argsSigned", std::vector<bool>{false});
    graph.setAttr(import, "argsType", std::vector<std::string>{"logic"});
    graph.setAttr(import, "hasReturn", true);
    graph.setAttr(import, "returnWidth", static_cast<int64_t>(8));
    graph.setAttr(import, "returnSigned", false);
    graph.setAttr(import, "returnType", std::string("logic"));

    const auto dpiResult = makeValue(graph, "dpi_ret", 8, false);
    const auto call = graph.createOperation(OperationKind::kDpicCall, graph.internSymbol("dpi_ret_call"));
    graph.setAttr(call, "targetImportSymbol", std::string("dpi_add1"));
    graph.setAttr(call, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(call, "inArgName", std::vector<std::string>{"value"});
    graph.setAttr(call, "outArgName", std::vector<std::string>{});
    graph.setAttr(call, "inoutArgName", std::vector<std::string>{});
    graph.setAttr(call, "hasReturn", true);
    graph.addOperand(call, cond);
    graph.addOperand(call, in);
    graph.addOperand(call, clk);
    graph.addResult(call, dpiResult);

    const auto assign = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_y"));
    graph.addOperand(assign, dpiResult);
    graph.addResult(assign, out);

    return design;
}

Design buildDpicReturnOutputDesign()
{
    Design design;
    auto &graph = design.createGraph("dpic_ret_out_top");
    design.markAsTop("dpic_ret_out_top");

    const auto clk = makeValue(graph, "clk", 1, false);
    const auto in = makeValue(graph, "in", 8, false);
    graph.bindInputPort("clk", clk);
    graph.bindInputPort("in", in);

    const auto outRet = makeValue(graph, "y_ret", 8, false);
    const auto outMirror = makeValue(graph, "y_mirror", 8, false);
    graph.bindOutputPort("y_ret", outRet);
    graph.bindOutputPort("y_mirror", outMirror);

    const auto cond = makeConstant(graph, "dpi_cond", "dpi_cond_const", 1, "1'b1");
    const auto import = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("dpi_bump_and_copy"));
    graph.setAttr(import, "argsDirection", std::vector<std::string>{"input", "output"});
    graph.setAttr(import, "argsWidth", std::vector<int64_t>{8, 8});
    graph.setAttr(import, "argsName", std::vector<std::string>{"value", "mirror"});
    graph.setAttr(import, "argsSigned", std::vector<bool>{false, false});
    graph.setAttr(import, "argsType", std::vector<std::string>{"logic", "logic"});
    graph.setAttr(import, "hasReturn", true);
    graph.setAttr(import, "returnWidth", static_cast<int64_t>(8));
    graph.setAttr(import, "returnSigned", false);
    graph.setAttr(import, "returnType", std::string("logic"));

    const auto dpiRet = makeValue(graph, "dpi_ret", 8, false);
    const auto dpiMirror = makeValue(graph, "dpi_mirror", 8, false);
    const auto call = graph.createOperation(OperationKind::kDpicCall, graph.internSymbol("dpi_ret_out_call"));
    graph.setAttr(call, "targetImportSymbol", std::string("dpi_bump_and_copy"));
    graph.setAttr(call, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(call, "inArgName", std::vector<std::string>{"value"});
    graph.setAttr(call, "outArgName", std::vector<std::string>{"mirror"});
    graph.setAttr(call, "inoutArgName", std::vector<std::string>{});
    graph.setAttr(call, "hasReturn", true);
    graph.addOperand(call, cond);
    graph.addOperand(call, in);
    graph.addOperand(call, clk);
    graph.addResult(call, dpiRet);
    graph.addResult(call, dpiMirror);

    const auto assignRet = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_y_ret"));
    graph.addOperand(assignRet, dpiRet);
    graph.addResult(assignRet, outRet);

    const auto assignMirror = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_y_mirror"));
    graph.addOperand(assignMirror, dpiMirror);
    graph.addResult(assignMirror, outMirror);

    return design;
}

Design buildDpicStringInputDesign()
{
    Design design;
    auto &graph = design.createGraph("dpic_string_top");
    design.markAsTop("dpic_string_top");

    const auto clk = makeValue(graph, "clk", 1, false);
    graph.bindInputPort("clk", clk);

    const auto out = makeValue(graph, "y", 8, false);
    graph.bindOutputPort("y", out);
    const auto outConst = makeConstant(graph, "out_const", "out_const_op", 8, "8'h2a");
    const auto assign = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_y"));
    graph.addOperand(assign, outConst);
    graph.addResult(assign, out);

    const auto cond = makeConstant(graph, "dpi_cond", "dpi_string_cond_const", 1, "1'b1");
    const auto filename = graph.createValue(graph.internSymbol("filename"), 8 * 21, false, ValueType::String);
    const auto filenameConst = graph.createOperation(OperationKind::kConstant, graph.internSymbol("filename_const"));
    graph.addResult(filenameConst, filename);
    graph.setAttr(filenameConst, "constValue", std::string("unit/test/assert.sv"));

    const auto line = makeConstant(graph, "line", "line_const", 64, "64'sd123");

    const auto import = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("dpi_capture_path"));
    graph.setAttr(import, "argsDirection", std::vector<std::string>{"input", "input"});
    graph.setAttr(import, "argsWidth", std::vector<int64_t>{static_cast<int64_t>(8 * 21), 64});
    graph.setAttr(import, "argsName", std::vector<std::string>{"filename", "line"});
    graph.setAttr(import, "argsSigned", std::vector<bool>{false, true});
    graph.setAttr(import, "argsType", std::vector<std::string>{"string", "longint"});
    graph.setAttr(import, "hasReturn", false);

    const auto call = graph.createOperation(OperationKind::kDpicCall, graph.internSymbol("dpi_string_call"));
    graph.setAttr(call, "targetImportSymbol", std::string("dpi_capture_path"));
    graph.setAttr(call, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(call, "inArgName", std::vector<std::string>{"filename", "line"});
    graph.setAttr(call, "outArgName", std::vector<std::string>{});
    graph.setAttr(call, "inoutArgName", std::vector<std::string>{});
    graph.setAttr(call, "hasReturn", false);
    graph.addOperand(call, cond);
    graph.addOperand(call, filename);
    graph.addOperand(call, line);
    graph.addOperand(call, clk);

    return design;
}

Design buildUnknownConstantDesign()
{
    Design design;
    auto &graph = design.createGraph("unknown_const_top");
    design.markAsTop("unknown_const_top");

    const auto out = makeValue(graph, "y", 8, false);
    graph.bindOutputPort("y", out);

    const auto unknownConst = makeConstant(graph, "unknown_const", "unknown_const_op", 8, "8'hxx");
    const auto assign = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_y"));
    graph.addOperand(assign, unknownConst);
    graph.addResult(assign, out);

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
    expect(result.artifacts.size() == 3, "EmitGsimCpp should report header, source, and manifest artifacts");

    const std::filesystem::path headerPath = dir / "top_metadata.hpp";
    const std::filesystem::path sourcePath = dir / "top_metadata.cpp";
    const std::filesystem::path manifestPath = dir / "top_metadata.manifest";
    expect(std::filesystem::exists(headerPath), "EmitGsimCpp should create header artifact");
    expect(std::filesystem::exists(sourcePath), "EmitGsimCpp should create source artifact");
    expect(std::filesystem::exists(manifestPath), "EmitGsimCpp should create manifest artifact");

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

void testMetadataShardManifestAndCleanup()
{
    Design design = buildSingleGraphDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "metadata_shards";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("split_metadata");
    options.topOverrides = {"top"};
    options.attributes["metadata_shard_max_bytes"] = "128";

    const EmitResult splitResult = emitter.emit(design, options);
    expect(splitResult.success, "EmitGsimCpp split metadata emission should succeed");
    expect(!diags.hasError(), "split metadata emission should not emit diagnostics");

    const std::filesystem::path headerPath = dir / "split_metadata.hpp";
    const std::filesystem::path sourcePath = dir / "split_metadata.cpp";
    const std::filesystem::path manifestPath = dir / "split_metadata.manifest";
    expect(std::filesystem::exists(headerPath), "split metadata emission should keep stable header path");
    expect(std::filesystem::exists(sourcePath), "split metadata emission should keep stable source path");
    expect(std::filesystem::exists(manifestPath), "split metadata emission should write a manifest");

    const auto shards = findMetadataShards(dir, "split_metadata");
    expect(!shards.empty(), "split metadata emission should create at least one shard with a tiny threshold");

    const auto manifestLines = readLines(manifestPath);
    expect(!manifestLines.empty(), "split metadata manifest should list managed source files");
    expect(manifestLines.front() == "split_metadata.cpp", "split metadata manifest should start with the canonical source");
    for (const auto &shard : shards)
    {
        expect(std::find(manifestLines.begin(), manifestLines.end(), shard.filename().string()) != manifestLines.end(),
               "split metadata manifest should include every shard");
    }

    const std::string source = readFile(sourcePath);
    expect(!contains(source, "metadata.schedule_activity_order = {"),
           "canonical source should stay lightweight when metadata is sharded");

    {
        const std::filesystem::path wrapperPath = dir / "split_compile_check.cpp";
        std::ofstream wrapper(wrapperPath);
        wrapper << "#include \"split_metadata.hpp\"\n";
        for (const auto &name : manifestLines)
        {
            wrapper << "#include \"" << name << "\"\n";
        }
        wrapper << "int main() { auto metadata = wolvrix::gsim::make_top_metadata(); return metadata.op_count < 0; }\n";
        const std::string compileCmd =
            "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
            " -fsyntax-only " + wrapperPath.string() + " 2>&1";
        expect(std::system(compileCmd.c_str()) == 0, "split metadata source set should compile with strict flags");
    }

    EmitDiagnostics rerunDiags;
    EmitGsimCpp rerunEmitter(&rerunDiags);
    EmitOptions rerunOptions;
    rerunOptions.outputDir = dir.string();
    rerunOptions.outputFilename = std::string("split_metadata");
    rerunOptions.topOverrides = {"top"};
    rerunOptions.attributes["metadata_shard_max_bytes"] = "10485760";

    const EmitResult rerunResult = rerunEmitter.emit(design, rerunOptions);
    expect(rerunResult.success, "rerun without sharding should still succeed");
    expect(!rerunDiags.hasError(), "rerun without sharding should not emit diagnostics");
    expect(findMetadataShards(dir, "split_metadata").empty(),
           "rerun without sharding should remove stale metadata shards from the previous manifest");

    const auto rerunManifestLines = readLines(manifestPath);
    expect(rerunManifestLines.size() == 1 && rerunManifestLines.front() == "split_metadata.cpp",
           "rerun manifest should only keep the canonical source when no shards are needed");
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

    // Verify declaration order: a, b, clk (as registered in buildSingleGraphDesign)
    const std::string header = readFile(dir / "po_decl.hpp");
    auto posA = header.find("set_a(");
    auto posB = header.find("set_b(");
    auto posClk = header.find("set_clk(");
    expect(posA != std::string::npos && posB != std::string::npos && posClk != std::string::npos,
           "decl-ordered header should contain all input port setters");
    expect(posA < posB && posB < posClk, "decl ordering should preserve a < b < clk registration order");
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

void testPortOrderDuplicateName()
{
    Design design = buildSingleGraphDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "port_order_dup";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("po_dup");
    options.topOverrides = {"top"};
    options.portOrderStrategy = PortOrderStrategy::Custom;
    options.portOrderNames = {"clk", "b", "b", "a"};

    const EmitResult result = emitter.emit(design, options);
    expect(!result.success, "port_order=custom with duplicate name should fail");
    expect(diags.hasError(), "should emit error for duplicate port name");
}

void testVersionMismatchRejection()
{
    Design design = buildSingleGraphDesign();
    runGsim(design, "top");

    // Mutate schedule version to 2
    design.setScratchpad<int64_t>("gsim.top.schedule.version", 2);

    const auto dir = artifactRoot() / "version_mismatch";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("vm_test");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(!result.success, "emit should reject schedule.version != 1");
    expect(diags.hasError(), "should emit version mismatch diagnostic");
}

void testHypergraphVersionMismatchRejection()
{
    Design design = buildSingleGraphDesign();
    runGsim(design, "top");

    // Mutate hypergraph version to 99
    design.setScratchpad<int64_t>("gsim.top.hypergraph.version", 99);

    const auto dir = artifactRoot() / "hg_version_mismatch";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("hgvm_test");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(!result.success, "emit should reject hypergraph.version != 1");
    expect(diags.hasError(), "should emit version mismatch diagnostic");
}

void testRegisterLatencyBehavior()
{
    // Build a design where output is driven through a register
    Design design;
    auto &graph = design.createGraph("reg_top");
    design.markAsTop("reg_top");

    const auto inA = makeValue(graph, "a", 8, false);
    const auto inClk = makeValue(graph, "clk", 1, false);
    graph.bindInputPort("a", inA);
    graph.bindInputPort("clk", inClk);

    const auto outY = makeValue(graph, "y", 8, false);
    graph.bindOutputPort("y", outY);

    // Register: stores 'a' on clock edge
    const auto regOut = makeValue(graph, "reg_state_out", 8, false);
    const auto regOp = graph.createOperation(OperationKind::kRegister, graph.internSymbol("state"));
    graph.addResult(regOp, regOut);

    // Register read port -> output
    const auto readVal = makeValue(graph, "state_read", 8, false);
    const auto readOp = graph.createOperation(OperationKind::kRegisterReadPort, graph.internSymbol("state_rp"));
    graph.setAttr(readOp, "regSymbol", std::string("state"));
    graph.addResult(readOp, readVal);

    const auto assignY = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_y"));
    graph.addOperand(assignY, readVal);
    graph.addResult(assignY, outY);

    // Register write port: always write 'a'
    const auto one = makeConstant(graph, "one", "one_c", 1, "1'b1");
    const auto mask = makeConstant(graph, "mask", "mask_c", 8, "8'hff");
    makeRegisterWrite(graph, "state_wp", one, inA, mask, inClk, "state");

    // Run gsim
    runGsim(design, "reg_top");

    const auto dir = artifactRoot() / "reg_latency";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("reg_sim");
    options.topOverrides = {"reg_top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "register latency emit should succeed");

    // Write driver that checks 1-cycle latency
    const std::filesystem::path driverPath = dir / "reg_driver.cpp";
    {
        std::ofstream driver(driverPath);
        driver << "#include \"reg_sim.hpp\"\n";
        driver << "#include \"reg_sim.cpp\"\n";
        driver << "#include <cstdio>\n";
        driver << "int main() {\n";
        driver << "    SSimTop sim;\n";
        driver << "    sim.set_reset(1); sim.step();\n";
        driver << "    // After reset, output should be 0\n";
        driver << "    if (sim.get_y() != 0) { printf(\"FAIL: after reset y=%d expected 0\\n\", sim.get_y()); return 1; }\n";
        driver << "    sim.set_a(42); sim.step();\n";
        driver << "    // Register captures 42, but output shows old value (0) due to 1-cycle latency\n";
        driver << "    if (sim.get_y() != 0) { printf(\"FAIL: step1 y=%d expected 0\\n\", sim.get_y()); return 1; }\n";
        driver << "    sim.step();\n";
        driver << "    // Now output should show captured value (42)\n";
        driver << "    if (sim.get_y() != 42) { printf(\"FAIL: step2 y=%d expected 42\\n\", sim.get_y()); return 1; }\n";
        driver << "    printf(\"REGISTER LATENCY PASS\\n\");\n";
        driver << "    return 0;\n";
        driver << "}\n";
    }

    const std::string exePath = (dir / "reg_driver_exe").string();
    const std::string compileCmd = "g++ -std=c++17 -Wall -Wextra -I " +
        dir.string() + " -o " + exePath + " " + driverPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "register latency driver should compile");
    expect(std::system(exePath.c_str()) == 0, "register latency driver should pass");
}

void testReplicateSignExtendBehavior()
{
    Design design = buildReplicateSignExtendDesign();
    runGsim(design, "signext_top");

    const auto dir = artifactRoot() / "replicate_signext";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("signext_sim");
    options.topOverrides = {"signext_top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "replicate sign-extend emit should succeed");
    expect(!diags.hasError(), "replicate sign-extend emit should not emit diagnostics");

    const std::filesystem::path driverPath = dir / "signext_driver.cpp";
    {
        std::ofstream driver(driverPath);
        driver << "#include \"signext_sim.hpp\"\n";
        driver << "#include \"signext_sim.cpp\"\n";
        driver << "#include <cstdint>\n";
        driver << "#include <cstdio>\n";
        driver << "int main() {\n";
        driver << "    SSimTop sim;\n";
        driver << "    sim.set_reset(1); sim.step();\n";
        driver << "    sim.set_in(static_cast<std::uint8_t>(0x7f)); sim.step();\n";
        driver << "    if (sim.get_out() != static_cast<std::uint32_t>(0x0000007fU)) {\n";
        driver << "        std::printf(\"FAIL: pos out=%u expected=%u\\n\", sim.get_out(), static_cast<std::uint32_t>(0x0000007fU));\n";
        driver << "        return 1;\n";
        driver << "    }\n";
        driver << "    sim.set_in(static_cast<std::uint8_t>(0x80)); sim.step();\n";
        driver << "    if (sim.get_out() != static_cast<std::uint32_t>(0xffffff80U)) {\n";
        driver << "        std::printf(\"FAIL: neg out=%u expected=%u\\n\", sim.get_out(), static_cast<std::uint32_t>(0xffffff80U));\n";
        driver << "        return 1;\n";
        driver << "    }\n";
        driver << "    return 0;\n";
        driver << "}\n";
    }

    const std::string exePath = (dir / "signext_driver_exe").string();
    const std::string compileCmd = "g++ -std=c++17 -Wall -Wextra -Werror -I " +
        dir.string() + " -o " + exePath + " " + driverPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "replicate sign-extend driver should compile");
    expect(std::system(exePath.c_str()) == 0, "replicate sign-extend driver should pass");
}

void testLargeCombinationalChainUsesMaterializedTemporaries()
{
    Design design = buildLinearAddChainDesign(128);
    runGsim(design, "chain_top");

    const auto dir = artifactRoot() / "materialized_temps";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("chain_sim");
    options.topOverrides = {"chain_top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "linear add chain emit should succeed");
    expect(!diags.hasError(), "linear add chain emit should not emit diagnostics");

    const std::string header = readFile(dir / "chain_sim.hpp");
    const std::string source = readFile(dir / "chain_sim.cpp");
    expect(!contains(header, "sim_tmp_v"),
           "large combinational chains should keep materialized temporaries out of the public header");
    expect(!contains(header, "output_y_ = sim_tmp_v"),
           "header should not inline step behavior for large combinational chains");
    expect(contains(source, "const std::uint8_t sim_tmp_v"),
           "large combinational chains should materialize intermediate temporaries in the emitted source");
    expect(contains(source, "output_y_ = sim_tmp_v"),
           "output assignments should consume a materialized temporary rather than an inlined expression tree");

    const std::filesystem::path driverPath = dir / "chain_driver.cpp";
    {
        std::ofstream driver(driverPath);
        driver << "#include \"chain_sim.hpp\"\n";
        driver << "#include \"chain_sim.cpp\"\n";
        driver << "#include <cstdio>\n";
        driver << "int main() {\n";
        driver << "    SSimTop sim;\n";
        driver << "    sim.set_reset(1); sim.step();\n";
        driver << "    sim.set_in(5); sim.step();\n";
        driver << "    if (sim.get_y() != 133) { std::printf(\"FAIL %u\\n\", static_cast<unsigned>(sim.get_y())); return 1; }\n";
        driver << "    return 0;\n";
        driver << "}\n";
    }

    const std::string exePath = (dir / "chain_driver").string();
    const std::string compileCmd =
        "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -o " + exePath + " " + driverPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "materialized temporary driver should compile");
    expect(std::system(exePath.c_str()) == 0, "materialized temporary driver should run successfully");
}

void testBehaviorShardsManifestAndCompile()
{
    Design design = buildLinearAddChainDesign(256);
    runGsim(design, "chain_top");

    const auto dir = artifactRoot() / "behavior_shards";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("chain_split");
    options.topOverrides = {"chain_top"};
    options.attributes["behavior_shard_max_bytes"] = "256";

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "behavior-sharded emit should succeed");
    expect(!diags.hasError(), "behavior-sharded emit should not emit diagnostics");

    const auto behaviorShards = findBehaviorShards(dir, "chain_split");
    expect(!behaviorShards.empty(), "tiny behavior shard threshold should produce step shards");

    const auto manifestLines = readLines(dir / "chain_split.manifest");
    expect(!manifestLines.empty(), "behavior-sharded emit should still write a manifest");
    for (const auto &shard : behaviorShards)
    {
        expect(std::find(manifestLines.begin(), manifestLines.end(), shard.filename().string()) != manifestLines.end(),
               "behavior shard should be listed in the managed source manifest");
    }

    const std::string header = readFile(dir / "chain_split.hpp");
    const std::string source = readFile(dir / "chain_split.cpp");
    expect(!contains(header, "sim_tmp_v"),
           "behavior sharding should keep implementation details out of the header");
    expect(!contains(source, "sim_tmp_v"),
           "canonical source should stay lightweight when behavior is sharded");

    const std::filesystem::path wrapperPath = dir / "behavior_shard_compile.cpp";
    {
        std::ofstream wrapper(wrapperPath);
        wrapper << "#include \"chain_split.hpp\"\n";
        for (const auto &name : manifestLines)
        {
            wrapper << "#include \"" << name << "\"\n";
        }
        wrapper << "int main() { SSimTop sim; sim.set_reset(1); sim.step(); sim.set_in(5); sim.step(); return sim.get_y() == static_cast<std::uint8_t>(5 + 256) ? 0 : 1; }\n";
    }

    const std::string exePath = (dir / "behavior_shard_driver").string();
    const std::string compileCmd =
        "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -o " + exePath + " " + wrapperPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "behavior-sharded source set should compile");
    expect(std::system(exePath.c_str()) == 0, "behavior-sharded source set should run");
}

void testMemoryLoweringBehavior()
{
    Design design = buildMemoryBehaviorDesign();
    runGsim(design, "mem_top");

    const auto dir = artifactRoot() / "memory_behavior";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("mem_sim");
    options.topOverrides = {"mem_top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "memory lowering emit should succeed");
    expect(!diags.hasError(), "memory lowering emit should not emit diagnostics");

    const std::string header = readFile(dir / "mem_sim.hpp");
    const std::string source = readFile(dir / "mem_sim.cpp");
    expect(contains(header, "std::vector<std::uint8_t> mem_mem0_"),
           "memory lowering should declare vector-backed storage");
    expect(contains(source, "std::fill(mem_mem0_.begin(), mem_mem0_.end(), 0);"),
           "memory lowering should reset vector-backed storage in the emitted source");

    const std::filesystem::path driverPath = dir / "mem_driver.cpp";
    {
        std::ofstream driver(driverPath);
        driver << "#include \"mem_sim.hpp\"\n";
        driver << "#include \"mem_sim.cpp\"\n";
        driver << "#include <cstdio>\n";
        driver << "int main() {\n";
        driver << "    SSimTop sim;\n";
        driver << "    sim.set_reset(1); sim.step();\n";
        driver << "    sim.set_raddr(1); sim.set_wen(1); sim.set_waddr(1); sim.set_wdata(42); sim.step();\n";
        driver << "    if (sim.get_y() != 0) { std::printf(\"FAIL write-step %u\\n\", static_cast<unsigned>(sim.get_y())); return 1; }\n";
        driver << "    sim.set_wen(0); sim.step();\n";
        driver << "    if (sim.get_y() != 42) { std::printf(\"FAIL readback %u\\n\", static_cast<unsigned>(sim.get_y())); return 1; }\n";
        driver << "    return 0;\n";
        driver << "}\n";
    }

    const std::string exePath = (dir / "mem_driver").string();
    const std::string compileCmd =
        "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -o " + exePath + " " + driverPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "memory lowering driver should compile");
    expect(std::system(exePath.c_str()) == 0, "memory lowering driver should run successfully");
}

void testDpicSideEffectCallBehavior()
{
    Design design = buildDpicIgnoredDesign();
    runGsim(design, "dpic_top");

    const auto dir = artifactRoot() / "dpic_ignored";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("dpic_sim");
    options.topOverrides = {"dpic_top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "dpic-ignored emit should succeed");
    expect(!diags.hasError(), "dpic-ignored emit should not emit diagnostics");

    const std::filesystem::path driverPath = dir / "dpic_driver.cpp";
    {
        std::ofstream driver(driverPath);
        driver << "#include \"dpic_sim.hpp\"\n";
        driver << "#include <cstdint>\n";
        driver << "static std::uint32_t g_dpi_calls = 0;\n";
        driver << "static std::uint32_t g_dpi_last = 0;\n";
        driver << "extern \"C\" void dpi_capture(std::uint8_t value) { ++g_dpi_calls; g_dpi_last = value; }\n";
        driver << "#include \"dpic_sim.cpp\"\n";
        driver << "int main() {\n";
        driver << "    SSimTop sim;\n";
        driver << "    sim.set_reset(1); sim.step();\n";
        driver << "    sim.set_in(7); sim.step();\n";
        driver << "    if (sim.get_y() != 7) return 1;\n";
        driver << "    if (g_dpi_calls != 1 || g_dpi_last != 7) return 2;\n";
        driver << "    return 0;\n";
        driver << "}\n";
    }

    const std::string exePath = (dir / "dpic_driver").string();
    const std::string compileCmd =
        "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -o " + exePath + " " + driverPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "dpic side-effect driver should compile");
    expect(std::system(exePath.c_str()) == 0, "dpic side-effect driver should run successfully");
}

void testDpicReturnAndOutputBehavior()
{
    Design design = buildDpicReturnOutputDesign();
    runGsim(design, "dpic_ret_out_top");

    const auto dir = artifactRoot() / "dpic_return_output";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("dpic_ret_out_sim");
    options.topOverrides = {"dpic_ret_out_top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "return/output dpic emit should succeed");
    expect(!diags.hasError(), "return/output dpic emit should not emit diagnostics");

    const std::filesystem::path driverPath = dir / "dpic_ret_out_driver.cpp";
    {
        std::ofstream driver(driverPath);
        driver << "#include \"dpic_ret_out_sim.hpp\"\n";
        driver << "#include <cstdint>\n";
        driver << "extern \"C\" std::uint8_t dpi_bump_and_copy(std::uint8_t value, std::uint8_t *mirror) {\n";
        driver << "    *mirror = static_cast<std::uint8_t>(value + 2);\n";
        driver << "    return static_cast<std::uint8_t>(value + 1);\n";
        driver << "}\n";
        driver << "#include \"dpic_ret_out_sim.cpp\"\n";
        driver << "int main() {\n";
        driver << "    SSimTop sim;\n";
        driver << "    sim.set_reset(1); sim.step();\n";
        driver << "    sim.set_in(9); sim.step();\n";
        driver << "    if (sim.get_y_ret() != 10) return 1;\n";
        driver << "    if (sim.get_y_mirror() != 11) return 2;\n";
        driver << "    return 0;\n";
        driver << "}\n";
    }

    const std::string exePath = (dir / "dpic_ret_out_driver").string();
    const std::string compileCmd =
        "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -o " + exePath + " " + driverPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "dpic return/output driver should compile");
    expect(std::system(exePath.c_str()) == 0, "dpic return/output driver should run successfully");
}

void testDpicStringInputBehavior()
{
    Design design = buildDpicStringInputDesign();
    runGsim(design, "dpic_string_top");

    const auto dir = artifactRoot() / "dpic_string_input";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("dpic_string_sim");
    options.topOverrides = {"dpic_string_top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "string-input dpic emit should succeed");
    expect(!diags.hasError(), "string-input dpic emit should not emit diagnostics");

    const std::filesystem::path driverPath = dir / "dpic_string_driver.cpp";
    {
        std::ofstream driver(driverPath);
        driver << "#include \"dpic_string_sim.hpp\"\n";
        driver << "#include <cstdint>\n";
        driver << "#include <cstring>\n";
        driver << "static const char *g_filename = nullptr;\n";
        driver << "static std::int64_t g_line = 0;\n";
        driver << "extern \"C\" void dpi_capture_path(const char *filename, std::int64_t line) { g_filename = filename; g_line = line; }\n";
        driver << "#include \"dpic_string_sim.cpp\"\n";
        driver << "int main() {\n";
        driver << "    SSimTop sim;\n";
        driver << "    sim.set_reset(1); sim.step();\n";
        driver << "    sim.step();\n";
        driver << "    if (sim.get_y() != 42) return 1;\n";
        driver << "    if (g_filename == nullptr || std::strcmp(g_filename, \"unit/test/assert.sv\") != 0) return 2;\n";
        driver << "    if (g_line != 123) return 3;\n";
        driver << "    return 0;\n";
        driver << "}\n";
    }

    const std::string exePath = (dir / "dpic_string_driver").string();
    const std::string compileCmd =
        "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -o " + exePath + " " + driverPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "dpic string-input driver should compile");
    expect(std::system(exePath.c_str()) == 0, "dpic string-input driver should run successfully");
}

void testUnknownConstantBehavior()
{
    Design design = buildUnknownConstantDesign();
    runGsim(design, "unknown_const_top");

    const auto dir = artifactRoot() / "unknown_constant";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("unknown_const_sim");
    options.topOverrides = {"unknown_const_top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "unknown-constant emit should succeed");
    expect(!diags.hasError(), "unknown-constant emit should not emit diagnostics");

    const std::string source = readFile(dir / "unknown_const_sim.cpp");
    expect(contains(source, "0x0ULL"),
           "unknown-state constants should lower to a valid 2-state zero literal");

    const std::filesystem::path driverPath = dir / "unknown_const_driver.cpp";
    {
        std::ofstream driver(driverPath);
        driver << "#include \"unknown_const_sim.hpp\"\n";
        driver << "#include \"unknown_const_sim.cpp\"\n";
        driver << "int main() {\n";
        driver << "    SSimTop sim;\n";
        driver << "    if (sim.get_y() != 0) return 1;\n";
        driver << "    sim.step();\n";
        driver << "    return sim.get_y() == 0 ? 0 : 2;\n";
        driver << "}\n";
    }

    const std::string exePath = (dir / "unknown_const_driver").string();
    const std::string compileCmd =
        "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -o " + exePath + " " + driverPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "unknown-constant driver should compile");
    expect(std::system(exePath.c_str()) == 0, "unknown-constant driver should run successfully");
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
        testMetadataShardManifestAndCleanup();
        testGraphOnlyAndMultiHopTargetSelectionConsistency();
        testCrossRootInstancePathsStayDistinct();
        testBehavioralCompileAndRun();
        testPortOrderDecl();
        testPortOrderAlpha();
        testPortOrderCustom();
        testPortOrderInvalidName();
        testPortOrderDuplicateName();
        testVersionMismatchRejection();
        testHypergraphVersionMismatchRejection();
        testRegisterLatencyBehavior();
        testReplicateSignExtendBehavior();
        testLargeCombinationalChainUsesMaterializedTemporaries();
        testBehaviorShardsManifestAndCompile();
        testMemoryLoweringBehavior();
        testDpicSideEffectCallBehavior();
        testDpicReturnAndOutputBehavior();
        testDpicStringInputBehavior();
        testUnknownConstantBehavior();
    }
    catch (const std::exception &ex)
    {
        return fail(ex.what());
    }
    return 0;
}
