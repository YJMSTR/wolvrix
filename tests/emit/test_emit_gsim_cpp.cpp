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

std::size_t maxLineLength(const std::filesystem::path &path)
{
    std::ifstream stream(path);
    if (!stream.is_open())
    {
        return 0;
    }
    std::size_t maxLen = 0;
    std::string line;
    while (std::getline(stream, line))
    {
        maxLen = std::max(maxLen, line.size());
    }
    return maxLen;
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

Design buildNamedResetPassthroughDesign()
{
    Design design;
    auto &graph = design.createGraph("reset_top");
    design.markAsTop("reset_top");

    const auto resetIn = makeValue(graph, "reset", 1, false);
    const auto outY = makeValue(graph, "y", 1, false);
    graph.bindInputPort("reset", resetIn);
    graph.bindOutputPort("y", outY);

    const auto assign = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_y"));
    graph.addOperand(assign, resetIn);
    graph.addResult(assign, outY);

    return design;
}

Design buildNamedResetRegisterCycleDesign()
{
    Design design;
    auto &graph = design.createGraph("reset_reg_top");
    design.markAsTop("reset_reg_top");

    const auto resetIn = makeValue(graph, "reset", 1, false);
    graph.bindInputPort("reset", resetIn);

    const auto outY = makeValue(graph, "y", 8, false);
    graph.bindOutputPort("y", outY);

    const auto regOp = graph.createOperation(OperationKind::kRegister, graph.internSymbol("state"));
    graph.setAttr(regOp, "width", static_cast<int64_t>(8));
    graph.setAttr(regOp, "isSigned", false);
    graph.setAttr(regOp, "initValue", std::string("8'h00"));

    const auto readVal = makeValue(graph, "state_read", 8, false);
    const auto readOp = graph.createOperation(OperationKind::kRegisterReadPort, graph.internSymbol("state_rp"));
    graph.setAttr(readOp, "regSymbol", std::string("state"));
    graph.addResult(readOp, readVal);

    const auto assignY = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_y"));
    graph.addOperand(assignY, readVal);
    graph.addResult(assignY, outY);

    const auto zero = makeConstant(graph, "zero", "zero_c", 8, "8'h00");
    const auto one = makeConstant(graph, "one", "one_c", 8, "8'h01");
    const auto regNext = makeValue(graph, "state_next", 8, false);
    const auto add = graph.createOperation(OperationKind::kAdd, graph.internSymbol("state_inc"));
    graph.addOperand(add, readVal);
    graph.addOperand(add, one);
    graph.addResult(add, regNext);

    const auto writeVal = makeValue(graph, "state_write", 8, false);
    const auto mux = graph.createOperation(OperationKind::kMux, graph.internSymbol("state_sel"));
    graph.addOperand(mux, resetIn);
    graph.addOperand(mux, zero);
    graph.addOperand(mux, regNext);
    graph.addResult(mux, writeVal);

    const auto writeEnable = makeConstant(graph, "write_enable", "write_enable_c", 1, "1'b1");
    const auto mask = makeConstant(graph, "mask", "mask_c", 8, "8'hff");
    const auto clk = makeConstant(graph, "clk_const", "clk_const_c", 1, "1'b1");
    makeRegisterWrite(graph, "state_wp", writeEnable, writeVal, mask, clk, "state");

    const auto import = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("dpi_capture_state"));
    graph.setAttr(import, "argsDirection", std::vector<std::string>{"input"});
    graph.setAttr(import, "argsWidth", std::vector<int64_t>{8});
    graph.setAttr(import, "argsName", std::vector<std::string>{"value"});
    graph.setAttr(import, "argsSigned", std::vector<bool>{false});
    graph.setAttr(import, "argsType", std::vector<std::string>{"logic"});
    graph.setAttr(import, "hasReturn", false);
    graph.setAttr(import, "returnWidth", static_cast<int64_t>(0));
    graph.setAttr(import, "returnSigned", false);
    graph.setAttr(import, "returnType", std::string("void"));

    const auto call = graph.createOperation(OperationKind::kDpicCall, graph.internSymbol("dpi_capture_state_call"));
    graph.setAttr(call, "targetImportSymbol", std::string("dpi_capture_state"));
    graph.setAttr(call, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(call, "inArgName", std::vector<std::string>{"value"});
    graph.setAttr(call, "outArgName", std::vector<std::string>{});
    graph.setAttr(call, "inoutArgName", std::vector<std::string>{});
    graph.setAttr(call, "hasReturn", false);
    graph.addOperand(call, writeEnable);
    graph.addOperand(call, readVal);
    graph.addOperand(call, clk);

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

Design buildSelfCompareEqDesign()
{
    Design design;
    auto &graph = design.createGraph("self_eq_top");
    design.markAsTop("self_eq_top");

    const auto in = makeValue(graph, "a", 8, false);
    graph.bindInputPort("a", in);

    const auto eqValue = makeValue(graph, "eq_value", 1, false);
    const auto out = makeValue(graph, "y", 1, false);
    graph.bindOutputPort("y", out);

    const auto eq = graph.createOperation(OperationKind::kEq, graph.internSymbol("self_eq"));
    graph.addOperand(eq, in);
    graph.addOperand(eq, in);
    graph.addResult(eq, eqValue);

    const auto assign = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_y"));
    graph.addOperand(assign, eqValue);
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

Design buildWideBehaviorConcatDesign(std::size_t width)
{
    Design design;
    auto &graph = design.createGraph("wide_behavior_top");
    design.markAsTop("wide_behavior_top");

    std::vector<ValueId> inputs;
    inputs.reserve(width);
    for (std::size_t i = 0; i < width; ++i)
    {
        const auto value = makeValue(graph, "in_" + std::to_string(i), 1, false);
        graph.bindInputPort("in_" + std::to_string(i), value);
        inputs.push_back(value);
    }

    const auto concatValue = makeValue(graph, "concat_value", static_cast<int32_t>(width), false);
    const auto concat = graph.createOperation(OperationKind::kConcat, graph.internSymbol("wide_concat"));
    for (const auto value : inputs)
    {
        graph.addOperand(concat, value);
    }
    graph.addResult(concat, concatValue);

    const auto out = makeValue(graph, "y", static_cast<int32_t>(width), false);
    graph.bindOutputPort("y", out);
    const auto assign = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_wide_concat_out"));
    graph.addOperand(assign, concatValue);
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

Design buildDivideByZeroBehaviorDesign()
{
    Design design;
    auto &graph = design.createGraph("div_top");
    design.markAsTop("div_top");

    const auto lhs = makeValue(graph, "lhs", 8, false);
    const auto rhs = makeValue(graph, "rhs", 8, false);
    graph.bindInputPort("lhs", lhs);
    graph.bindInputPort("rhs", rhs);

    const auto divValue = makeValue(graph, "div_value", 8, false);
    const auto modValue = makeValue(graph, "mod_value", 8, false);
    const auto outQ = makeValue(graph, "q", 8, false);
    const auto outR = makeValue(graph, "r", 8, false);
    graph.bindOutputPort("q", outQ);
    graph.bindOutputPort("r", outR);

    const auto divOp = graph.createOperation(OperationKind::kDiv, graph.internSymbol("div"));
    graph.addOperand(divOp, lhs);
    graph.addOperand(divOp, rhs);
    graph.addResult(divOp, divValue);

    const auto modOp = graph.createOperation(OperationKind::kMod, graph.internSymbol("mod"));
    graph.addOperand(modOp, lhs);
    graph.addOperand(modOp, rhs);
    graph.addResult(modOp, modValue);

    const auto assignQ = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_q"));
    graph.addOperand(assignQ, divValue);
    graph.addResult(assignQ, outQ);

    const auto assignR = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_r"));
    graph.addOperand(assignR, modValue);
    graph.addResult(assignR, outR);

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

Design buildDpicSignedIntOutputDesign()
{
    Design design;
    auto &graph = design.createGraph("dpic_signed_out_top");
    design.markAsTop("dpic_signed_out_top");

    const auto clk = makeValue(graph, "clk", 1, false);
    graph.bindInputPort("clk", clk);

    const auto out = makeValue(graph, "y", 32, false);
    graph.bindOutputPort("y", out);

    const auto cond = makeConstant(graph, "dpi_cond", "dpi_signed_cond_const", 1, "1'b1");
    const auto import = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("dpi_fill_int"));
    graph.setAttr(import, "argsDirection", std::vector<std::string>{"output"});
    graph.setAttr(import, "argsWidth", std::vector<int64_t>{32});
    graph.setAttr(import, "argsName", std::vector<std::string>{"value"});
    graph.setAttr(import, "argsSigned", std::vector<bool>{true});
    graph.setAttr(import, "argsType", std::vector<std::string>{"int"});
    graph.setAttr(import, "hasReturn", false);
    graph.setAttr(import, "returnWidth", static_cast<int64_t>(0));
    graph.setAttr(import, "returnSigned", false);
    graph.setAttr(import, "returnType", std::string("void"));

    const auto dpiValue = makeValue(graph, "dpi_value", 32, false);
    const auto call = graph.createOperation(OperationKind::kDpicCall, graph.internSymbol("dpi_signed_out_call"));
    graph.setAttr(call, "targetImportSymbol", std::string("dpi_fill_int"));
    graph.setAttr(call, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(call, "inArgName", std::vector<std::string>{});
    graph.setAttr(call, "outArgName", std::vector<std::string>{"value"});
    graph.setAttr(call, "inoutArgName", std::vector<std::string>{});
    graph.setAttr(call, "hasReturn", false);
    graph.addOperand(call, cond);
    graph.addOperand(call, clk);
    graph.addResult(call, dpiValue);

    const auto assign = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_y"));
    graph.addOperand(assign, dpiValue);
    graph.addResult(assign, out);

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

Design buildWideDynamicShiftDesign()
{
    Design design;
    auto &graph = design.createGraph("wide_shift_top");
    design.markAsTop("wide_shift_top");

    const auto data = makeValue(graph, "data", 128, false);
    const auto amount = makeValue(graph, "amount", 128, false);
    const auto outShl = makeValue(graph, "y_shl", 128, false);
    const auto outLshr = makeValue(graph, "y_lshr", 128, false);
    const auto outAshr = makeValue(graph, "y_ashr", 128, false);
    graph.bindInputPort("data", data);
    graph.bindInputPort("amount", amount);
    graph.bindOutputPort("y_shl", outShl);
    graph.bindOutputPort("y_lshr", outLshr);
    graph.bindOutputPort("y_ashr", outAshr);

    const auto shlValue = makeValue(graph, "shl_value", 128, false);
    const auto lshrValue = makeValue(graph, "lshr_value", 128, false);
    const auto ashrValue = makeValue(graph, "ashr_value", 128, false);

    const auto shl = graph.createOperation(OperationKind::kShl, graph.internSymbol("wide_shl"));
    graph.addOperand(shl, data);
    graph.addOperand(shl, amount);
    graph.addResult(shl, shlValue);

    const auto lshr = graph.createOperation(OperationKind::kLShr, graph.internSymbol("wide_lshr"));
    graph.addOperand(lshr, data);
    graph.addOperand(lshr, amount);
    graph.addResult(lshr, lshrValue);

    const auto ashr = graph.createOperation(OperationKind::kAShr, graph.internSymbol("wide_ashr"));
    graph.addOperand(ashr, data);
    graph.addOperand(ashr, amount);
    graph.addResult(ashr, ashrValue);

    const auto assignShl = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_y_shl"));
    graph.addOperand(assignShl, shlValue);
    graph.addResult(assignShl, outShl);

    const auto assignLshr = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_y_lshr"));
    graph.addOperand(assignLshr, lshrValue);
    graph.addResult(assignLshr, outLshr);

    const auto assignAshr = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_y_ashr"));
    graph.addOperand(assignAshr, ashrValue);
    graph.addResult(assignAshr, outAshr);

    return design;
}

Design buildWideSliceDesign()
{
    Design design;
    auto &graph = design.createGraph("wide_slice_top");
    design.markAsTop("wide_slice_top");

    const auto data = makeValue(graph, "data", 136, false);
    const auto index = makeValue(graph, "index", 8, false);
    const auto outDynamic = makeValue(graph, "y_dynamic", 64, false);
    const auto outStatic = makeValue(graph, "y_static", 64, false);
    graph.bindInputPort("data", data);
    graph.bindInputPort("index", index);
    graph.bindOutputPort("y_dynamic", outDynamic);
    graph.bindOutputPort("y_static", outStatic);

    const auto dynamicValue = makeValue(graph, "dynamic_value", 64, false);
    const auto dynamicSlice = graph.createOperation(OperationKind::kSliceDynamic, graph.internSymbol("wide_slice_dynamic"));
    graph.addOperand(dynamicSlice, data);
    graph.addOperand(dynamicSlice, index);
    graph.addResult(dynamicSlice, dynamicValue);
    graph.setAttr(dynamicSlice, "sliceWidth", static_cast<int64_t>(64));

    const auto staticValue = makeValue(graph, "static_value", 64, false);
    const auto staticSlice = graph.createOperation(OperationKind::kSliceStatic, graph.internSymbol("wide_slice_static"));
    graph.addOperand(staticSlice, data);
    graph.addResult(staticSlice, staticValue);
    graph.setAttr(staticSlice, "sliceStart", static_cast<int64_t>(8));
    graph.setAttr(staticSlice, "sliceEnd", static_cast<int64_t>(71));

    const auto assignDynamic = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_y_dynamic"));
    graph.addOperand(assignDynamic, dynamicValue);
    graph.addResult(assignDynamic, outDynamic);

    const auto assignStatic = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_y_static"));
    graph.addOperand(assignStatic, staticValue);
    graph.addResult(assignStatic, outStatic);

    return design;
}

Design buildNarrowBehaviorAshrDesign(std::size_t prefixDepth)
{
    Design design;
    auto &graph = design.createGraph("narrow_ashr_top");
    design.markAsTop("narrow_ashr_top");

    const auto data = makeValue(graph, "data", 32, false);
    const auto amount = makeValue(graph, "amount", 5, false);
    graph.bindInputPort("data", data);
    graph.bindInputPort("amount", amount);

    ValueId current = data;
    for (std::size_t i = 0; i < prefixDepth; ++i)
    {
        const auto one = makeConstant(graph,
                                      "ashr_one_" + std::to_string(i),
                                      "ashr_one_const_" + std::to_string(i),
                                      32,
                                      "32'h00000001");
        const auto next = makeValue(graph, "ashr_prefix_" + std::to_string(i), 32, false);
        const auto add = graph.createOperation(OperationKind::kAdd, graph.internSymbol("ashr_prefix_add_" + std::to_string(i)));
        graph.addOperand(add, current);
        graph.addOperand(add, one);
        graph.addResult(add, next);
        current = next;
    }

    const auto shifted = makeValue(graph, "shifted", 32, false);
    const auto ashr = graph.createOperation(OperationKind::kAShr, graph.internSymbol("narrow_ashr"));
    graph.addOperand(ashr, current);
    graph.addOperand(ashr, amount);
    graph.addResult(ashr, shifted);

    const auto out = makeValue(graph, "y", 32, false);
    graph.bindOutputPort("y", out);
    const auto assign = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_narrow_ashr_out"));
    graph.addOperand(assign, shifted);
    graph.addResult(assign, out);

    return design;
}

Design buildRegisterInitValueDesign()
{
    Design design;
    auto &graph = design.createGraph("reg_init_top");
    design.markAsTop("reg_init_top");

    const auto outY = makeValue(graph, "y", 8, false);
    graph.bindOutputPort("y", outY);

    const auto regOp = graph.createOperation(OperationKind::kRegister, graph.internSymbol("state"));
    graph.setAttr(regOp, "width", static_cast<int64_t>(8));
    graph.setAttr(regOp, "isSigned", false);
    graph.setAttr(regOp, "initValue", std::string("8'h2a"));

    const auto readVal = makeValue(graph, "state_read", 8, false);
    const auto readOp = graph.createOperation(OperationKind::kRegisterReadPort, graph.internSymbol("state_rp"));
    graph.setAttr(readOp, "regSymbol", std::string("state"));
    graph.addResult(readOp, readVal);

    const auto assignY = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_y"));
    graph.addOperand(assignY, readVal);
    graph.addResult(assignY, outY);

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
    expect(contains(source, "metadata.schedule_activity_order.clear();"), "source should serialize schedule activity ordering");
    expect(contains(source, "metadata.schedule_activity_order.insert(metadata.schedule_activity_order.end(), {"),
           "source should serialize schedule activity ordering with chunked inserts");
    expect(contains(source, "metadata.hypergraph_edge_sinks.clear();"), "source should serialize hypergraph sink metadata");
    expect(contains(source, "metadata.hypergraph_edge_sinks["),
           "source should materialize hypergraph sink entries without monolithic initializers");
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
    options.attributes["metadata_shard_max_bytes"] = "1024";

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
    expect(!contains(source, "metadata.schedule_activity_order.insert(metadata.schedule_activity_order.end(), {"),
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

void testMetadataShardsKeepAssignmentsWhole()
{
    Design design = buildLinearAddChainDesign(256);
    runGsim(design, "chain_top");

    const auto dir = artifactRoot() / "metadata_shards_large";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("chain_metadata_split");
    options.topOverrides = {"chain_top"};
    options.attributes["metadata_shard_max_bytes"] = "4096";

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "large metadata sharded emit should succeed");
    expect(!diags.hasError(), "large metadata sharded emit should not emit diagnostics");

    const auto manifestLines = readLines(dir / "chain_metadata_split.manifest");
    expect(!manifestLines.empty(), "large metadata sharded emit should write a manifest");
    expect(!findMetadataShards(dir, "chain_metadata_split").empty(),
           "large metadata fixture should produce metadata shards");

    const std::filesystem::path wrapperPath = dir / "chain_metadata_compile.cpp";
    {
        std::ofstream wrapper(wrapperPath);
        wrapper << "#include \"chain_metadata_split.hpp\"\n";
        for (const auto &name : manifestLines)
        {
            wrapper << "#include \"" << name << "\"\n";
        }
        wrapper << "int main() { auto metadata = wolvrix::gsim::make_chain_top_metadata(); return metadata.op_count < 0; }\n";
    }

    const std::string compileCmd =
        "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -fsyntax-only " + wrapperPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0,
           "metadata shards should keep each metadata assignment whole enough for strict compilation");
}

void testMetadataShardsRespectByteBudgetForLargeMetadata()
{
    constexpr std::size_t metadataShardMaxBytes = 4096;
    constexpr std::uintmax_t shardFileSlackBytes = 2048;
    constexpr std::size_t maxMetadataStatementLineBytes = 4096;

    Design design = buildLinearAddChainDesign(4096);
    runGsim(design, "chain_top");

    const auto dir = artifactRoot() / "metadata_shards_budget";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("chain_metadata_budget");
    options.topOverrides = {"chain_top"};
    options.attributes["metadata_shard_max_bytes"] = std::to_string(metadataShardMaxBytes);

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "large metadata budget emit should succeed");
    expect(!diags.hasError(), "large metadata budget emit should not emit diagnostics");

    const auto shards = findMetadataShards(dir, "chain_metadata_budget");
    expect(!shards.empty(), "large metadata budget fixture should produce metadata shards");
    for (const auto &shard : shards)
    {
        const auto bytes = std::filesystem::file_size(shard);
        expect(bytes <= metadataShardMaxBytes + shardFileSlackBytes,
               "metadata shard should stay near the configured byte budget even with large metadata maps");

        std::ifstream shardStream(shard);
        std::string line;
        std::size_t longestLineBytes = 0;
        while (std::getline(shardStream, line))
        {
            longestLineBytes = std::max(longestLineBytes, line.size());
        }
        expect(longestLineBytes <= maxMetadataStatementLineBytes,
               "large metadata budget emit should avoid pathological single-line metadata statements");
    }

    const auto manifestLines = readLines(dir / "chain_metadata_budget.manifest");
    const std::filesystem::path wrapperPath = dir / "chain_metadata_budget_compile.cpp";
    {
        std::ofstream wrapper(wrapperPath);
        wrapper << "#include \"chain_metadata_budget.hpp\"\n";
        for (const auto &name : manifestLines)
        {
            wrapper << "#include \"" << name << "\"\n";
        }
        wrapper << "int main() { auto metadata = wolvrix::gsim::make_chain_top_metadata(); return metadata.op_count < 0; }\n";
    }

    const std::string compileCmd =
        "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -fsyntax-only " + wrapperPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0,
           "large metadata budget source set should still compile with strict flags");
}

void testMetadataStatementsStaySmallUnderLargeShardBudget()
{
    constexpr std::size_t metadataShardMaxBytes = 1024 * 1024;
    constexpr std::size_t maxMetadataStatementLineBytes = 4096;

    Design design = buildLinearAddChainDesign(4096);
    runGsim(design, "chain_top");

    const auto dir = artifactRoot() / "metadata_statement_budget";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("chain_metadata_stmt_budget");
    options.topOverrides = {"chain_top"};
    options.attributes["metadata_shard_max_bytes"] = std::to_string(metadataShardMaxBytes);

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "large shard budget emit should succeed");
    expect(!diags.hasError(), "large shard budget emit should not emit diagnostics");

    const auto shards = findMetadataShards(dir, "chain_metadata_stmt_budget");
    expect(!shards.empty(), "large shard budget fixture should still produce metadata shards");
    bool sawArrayLoopLowering = false;
    for (const auto &shard : shards)
    {
        const std::string shardSource = readFile(shard);
        expect(!contains(shardSource, "metadata.classifications.insert({{"),
               "large shard budget emit should avoid bulk map initializer inserts for classifications metadata");
        expect(!contains(shardSource, ".insert(metadata.predecessors["),
               "large shard budget emit should avoid predecessor vector insert initializers");
        sawArrayLoopLowering = sawArrayLoopLowering || contains(shardSource, "static constexpr std::int64_t values[] = {");

        std::ifstream shardStream(shard);
        std::string line;
        std::size_t longestLineBytes = 0;
        while (std::getline(shardStream, line))
        {
            longestLineBytes = std::max(longestLineBytes, line.size());
        }
        expect(longestLineBytes <= maxMetadataStatementLineBytes,
               "large shard budget emit should keep metadata statements bounded to avoid pathological compile hotspots");
    }
    expect(sawArrayLoopLowering,
           "large shard budget emit should lower at least one vector metadata shard through compact array loops");
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

void testResetPortNamedResetUsesInputSetter()
{
    Design design = buildNamedResetPassthroughDesign();
    runGsim(design, "reset_top");

    const auto dir = artifactRoot() / "named_reset_passthrough";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("named_reset_sim");
    options.topOverrides = {"reset_top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "named reset passthrough emit should succeed");
    expect(!diags.hasError(), "named reset passthrough emit should not emit diagnostics");

    const std::string header = readFile(dir / "named_reset_sim.hpp");
    expect(contains(header, "void set_reset("), "named reset passthrough should still expose set_reset");

    const std::filesystem::path driverPath = dir / "named_reset_driver.cpp";
    {
        std::ofstream driver(driverPath);
        driver << "#include \"named_reset_sim.hpp\"\n";
        driver << "#include \"named_reset_sim.cpp\"\n";
        driver << "#include <cstdio>\n";
        driver << "int main() {\n";
        driver << "    SSimTop sim;\n";
        driver << "    unsigned rst = 0;\n";
        driver << "    sim.step();\n";
        driver << "    rst = 1;\n";
        driver << "    sim.set_reset(rst);\n";
        driver << "    sim.step();\n";
        driver << "    if (sim.get_y() != 1) { std::printf(\"FAIL reset-high y=%u\\n\", static_cast<unsigned>(sim.get_y())); return 1; }\n";
        driver << "    rst = 0;\n";
        driver << "    sim.set_reset(rst);\n";
        driver << "    sim.step();\n";
        driver << "    if (sim.get_y() != 0) { std::printf(\"FAIL reset-low y=%u\\n\", static_cast<unsigned>(sim.get_y())); return 1; }\n";
        driver << "    return 0;\n";
        driver << "}\n";
    }

    const std::string exePath = (dir / "named_reset_driver").string();
    const std::string compileCmd = "g++ -std=c++17 -Wall -Wextra -Werror -I " +
        dir.string() + " -o " + exePath + " " + driverPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "named reset driver should compile");
    expect(std::system(exePath.c_str()) == 0, "named reset driver should observe the reset input");
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
        driver << "    // A step models one clocked cycle, so outputs should reflect the updated register state.\n";
        driver << "    if (sim.get_y() != 42) { printf(\"FAIL: step1 y=%d expected 42\\n\", sim.get_y()); return 1; }\n";
        driver << "    sim.set_a(7); sim.step();\n";
        driver << "    if (sim.get_y() != 7) { printf(\"FAIL: step2 y=%d expected 7\\n\", sim.get_y()); return 1; }\n";
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

void testRegisterDerivedOutputBehavior()
{
    Design design;
    auto &graph = design.createGraph("reg_derived_top");
    design.markAsTop("reg_derived_top");

    const auto inA = makeValue(graph, "a", 8, false);
    const auto inClk = makeValue(graph, "clk", 1, false);
    graph.bindInputPort("a", inA);
    graph.bindInputPort("clk", inClk);

    const auto outY = makeValue(graph, "y", 8, false);
    graph.bindOutputPort("y", outY);

    const auto regOut = makeValue(graph, "reg_state_out", 8, false);
    const auto regOp = graph.createOperation(OperationKind::kRegister, graph.internSymbol("state"));
    graph.addResult(regOp, regOut);

    const auto readVal = makeValue(graph, "state_read", 8, false);
    const auto readOp = graph.createOperation(OperationKind::kRegisterReadPort, graph.internSymbol("state_rp"));
    graph.setAttr(readOp, "regSymbol", std::string("state"));
    graph.addResult(readOp, readVal);

    const auto oneConst = makeConstant(graph, "one_const", "one_const_c", 8, "8'd1");
    const auto sumVal = makeValue(graph, "y_plus_one", 8, false);
    const auto addOp = graph.createOperation(OperationKind::kAdd, graph.internSymbol("add_y"));
    graph.addOperand(addOp, readVal);
    graph.addOperand(addOp, oneConst);
    graph.addResult(addOp, sumVal);

    const auto assignY = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_y"));
    graph.addOperand(assignY, sumVal);
    graph.addResult(assignY, outY);

    const auto wen = makeConstant(graph, "wen", "wen_c", 1, "1'b1");
    const auto mask = makeConstant(graph, "mask", "mask_c", 8, "8'hff");
    makeRegisterWrite(graph, "state_wp", wen, inA, mask, inClk, "state");

    runGsim(design, "reg_derived_top");

    const auto dir = artifactRoot() / "reg_derived_output";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("reg_derived_sim");
    options.topOverrides = {"reg_derived_top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "register-derived output emit should succeed");

    const std::filesystem::path driverPath = dir / "reg_derived_driver.cpp";
    {
        std::ofstream driver(driverPath);
        driver << "#include \"reg_derived_sim.hpp\"\n";
        driver << "#include \"reg_derived_sim.cpp\"\n";
        driver << "#include <cstdio>\n";
        driver << "int main() {\n";
        driver << "    SSimTop sim;\n";
        driver << "    sim.set_reset(1); sim.step();\n";
        driver << "    if (sim.get_y() != 1) { std::printf(\"FAIL reset y=%u expected 1\\n\", static_cast<unsigned>(sim.get_y())); return 1; }\n";
        driver << "    sim.set_a(41); sim.step();\n";
        driver << "    if (sim.get_y() != 42) { std::printf(\"FAIL step1 y=%u expected 42\\n\", static_cast<unsigned>(sim.get_y())); return 2; }\n";
        driver << "    sim.set_a(7); sim.step();\n";
        driver << "    if (sim.get_y() != 8) { std::printf(\"FAIL step2 y=%u expected 8\\n\", static_cast<unsigned>(sim.get_y())); return 3; }\n";
        driver << "    return 0;\n";
        driver << "}\n";
    }

    const std::string exePath = (dir / "reg_derived_driver").string();
    const std::string compileCmd =
        "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -o " + exePath + " " + driverPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "register-derived output driver should compile");
    expect(std::system(exePath.c_str()) == 0, "register-derived output driver should pass");
}

void testRegisterChainKeepsPreStepState()
{
    Design design;
    auto &graph = design.createGraph("reg_chain_top");
    design.markAsTop("reg_chain_top");

    const auto inD = makeValue(graph, "d", 1, false);
    const auto inClk = makeValue(graph, "clk", 1, false);
    graph.bindInputPort("d", inD);
    graph.bindInputPort("clk", inClk);

    const auto outQ = makeValue(graph, "q", 1, false);
    graph.bindOutputPort("q", outQ);

    const auto reg1 = graph.createOperation(OperationKind::kRegister, graph.internSymbol("inst1_q"));
    const auto reg1Out = makeValue(graph, "inst1_q_out", 1, false);
    graph.addResult(reg1, reg1Out);
    const auto reg2 = graph.createOperation(OperationKind::kRegister, graph.internSymbol("inst2_q"));
    const auto reg2Out = makeValue(graph, "inst2_q_out", 1, false);
    graph.addResult(reg2, reg2Out);
    const auto reg3 = graph.createOperation(OperationKind::kRegister, graph.internSymbol("inst3_q"));
    const auto reg3Out = makeValue(graph, "inst3_q_out", 1, false);
    graph.addResult(reg3, reg3Out);

    const auto rp1 = graph.createOperation(OperationKind::kRegisterReadPort, graph.internSymbol("inst1_rp"));
    graph.setAttr(rp1, "regSymbol", std::string("inst1_q"));
    const auto read1 = makeValue(graph, "inst1_read", 1, false);
    graph.addResult(rp1, read1);
    const auto rp2 = graph.createOperation(OperationKind::kRegisterReadPort, graph.internSymbol("inst2_rp"));
    graph.setAttr(rp2, "regSymbol", std::string("inst2_q"));
    const auto read2 = makeValue(graph, "inst2_read", 1, false);
    graph.addResult(rp2, read2);
    const auto rp3 = graph.createOperation(OperationKind::kRegisterReadPort, graph.internSymbol("inst3_rp"));
    graph.setAttr(rp3, "regSymbol", std::string("inst3_q"));
    const auto read3 = makeValue(graph, "inst3_read", 1, false);
    graph.addResult(rp3, read3);

    const auto assignQ = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_q"));
    graph.addOperand(assignQ, read3);
    graph.addResult(assignQ, outQ);

    const auto wen = makeConstant(graph, "wen", "wen_c", 1, "1'b1");
    const auto mask = makeConstant(graph, "mask", "mask_c", 1, "1'b1");
    makeRegisterWrite(graph, "inst1_wp", wen, inD, mask, inClk, "inst1_q");
    makeRegisterWrite(graph, "inst2_wp", wen, read1, mask, inClk, "inst2_q");
    makeRegisterWrite(graph, "inst3_wp", wen, read2, mask, inClk, "inst3_q");

    runGsim(design, "reg_chain_top");

    const auto dir = artifactRoot() / "reg_chain_snapshot";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("reg_chain_sim");
    options.topOverrides = {"reg_chain_top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "register-chain emit should succeed");

    const std::filesystem::path driverPath = dir / "reg_chain_driver.cpp";
    {
        std::ofstream driver(driverPath);
        driver << "#include \"reg_chain_sim.hpp\"\n";
        driver << "#include \"reg_chain_sim.cpp\"\n";
        driver << "#include <cstdio>\n";
        driver << "int main() {\n";
        driver << "    SSimTop sim;\n";
        driver << "    sim.set_d(1); sim.step();\n";
        driver << "    if (sim.get_q() != 0) { std::printf(\"FAIL step1 q=%u expected 0\\n\", static_cast<unsigned>(sim.get_q())); return 1; }\n";
        driver << "    sim.set_d(0); sim.step();\n";
        driver << "    if (sim.get_q() != 0) { std::printf(\"FAIL step2 q=%u expected 0\\n\", static_cast<unsigned>(sim.get_q())); return 2; }\n";
        driver << "    sim.set_d(1); sim.step();\n";
        driver << "    if (sim.get_q() != 1) { std::printf(\"FAIL step3 q=%u expected 1\\n\", static_cast<unsigned>(sim.get_q())); return 3; }\n";
        driver << "    return 0;\n";
        driver << "}\n";
    }

    const std::string exePath = (dir / "reg_chain_driver").string();
    const std::string compileCmd =
        "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -o " + exePath + " " + driverPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "register-chain driver should compile");
    expect(std::system(exePath.c_str()) == 0, "register-chain driver should preserve pre-step state across writes");
}

void testNamedResetInputWithClockDoesNotForceInitState()
{
    Design design;
    auto &graph = design.createGraph("named_reset_clock_top");
    design.markAsTop("named_reset_clock_top");

    const auto clk = makeValue(graph, "clk", 1, false);
    const auto resetIn = makeValue(graph, "reset", 1, false);
    const auto inD = makeValue(graph, "d", 8, false);
    graph.bindInputPort("clk", clk);
    graph.bindInputPort("reset", resetIn);
    graph.bindInputPort("d", inD);

    const auto outQ = makeValue(graph, "q", 8, false);
    graph.bindOutputPort("q", outQ);

    const auto regOp = graph.createOperation(OperationKind::kRegister, graph.internSymbol("state"));
    graph.setAttr(regOp, "initValue", std::string("8'h00"));
    const auto regOut = makeValue(graph, "state_out", 8, false);
    graph.addResult(regOp, regOut);

    const auto readOp = graph.createOperation(OperationKind::kRegisterReadPort, graph.internSymbol("state_rp"));
    graph.setAttr(readOp, "regSymbol", std::string("state"));
    const auto readVal = makeValue(graph, "state_read", 8, false);
    graph.addResult(readOp, readVal);

    const auto resetConst = makeConstant(graph, "reset_const", "reset_const_c", 8, "8'h34");
    const auto writeVal = makeValue(graph, "state_write", 8, false);
    const auto mux = graph.createOperation(OperationKind::kMux, graph.internSymbol("state_sel"));
    graph.addOperand(mux, resetIn);
    graph.addOperand(mux, resetConst);
    graph.addOperand(mux, inD);
    graph.addResult(mux, writeVal);

    const auto assignQ = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_q"));
    graph.addOperand(assignQ, readVal);
    graph.addResult(assignQ, outQ);

    const auto wen = makeConstant(graph, "wen", "wen_c", 1, "1'b1");
    const auto mask = makeConstant(graph, "mask", "mask_c", 8, "8'hff");
    makeRegisterWrite(graph, "state_wp", wen, writeVal, mask, clk, "state");

    runGsim(design, "named_reset_clock_top");

    const auto dir = artifactRoot() / "named_reset_clock";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("named_reset_clock_sim");
    options.topOverrides = {"named_reset_clock_top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "named-reset-with-clock emit should succeed");

    const std::filesystem::path driverPath = dir / "named_reset_clock_driver.cpp";
    {
        std::ofstream driver(driverPath);
        driver << "#include \"named_reset_clock_sim.hpp\"\n";
        driver << "#include \"named_reset_clock_sim.cpp\"\n";
        driver << "#include <cstdio>\n";
        driver << "int main() {\n";
        driver << "    SSimTop sim;\n";
        driver << "    sim.set_reset(1); sim.set_d(0); sim.step();\n";
        driver << "    if (sim.get_q() != 0x34u) { std::printf(\"FAIL reset q=%u expected 52\\n\", static_cast<unsigned>(sim.get_q())); return 1; }\n";
        driver << "    sim.set_reset(0); sim.set_d(18); sim.step();\n";
        driver << "    if (sim.get_q() != 18u) { std::printf(\"FAIL data q=%u expected 18\\n\", static_cast<unsigned>(sim.get_q())); return 2; }\n";
        driver << "    return 0;\n";
        driver << "}\n";
    }

    const std::string exePath = (dir / "named_reset_clock_driver").string();
    const std::string compileCmd =
        "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -o " + exePath + " " + driverPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "named-reset-with-clock driver should compile");
    expect(std::system(exePath.c_str()) == 0, "named reset should follow explicit graph logic instead of forcing init state");
}

void testNegedgeRegisterWriteHonorsClockLevel()
{
    Design design;
    auto &graph = design.createGraph("negedge_reg_top");
    design.markAsTop("negedge_reg_top");

    const auto clk = makeValue(graph, "clk", 1, false);
    const auto inD = makeValue(graph, "d", 8, false);
    graph.bindInputPort("clk", clk);
    graph.bindInputPort("d", inD);

    const auto outQ = makeValue(graph, "q", 8, false);
    graph.bindOutputPort("q", outQ);

    const auto regOp = graph.createOperation(OperationKind::kRegister, graph.internSymbol("state"));
    graph.setAttr(regOp, "initValue", std::string("8'h00"));
    const auto regOut = makeValue(graph, "state_out", 8, false);
    graph.addResult(regOp, regOut);

    const auto readOp = graph.createOperation(OperationKind::kRegisterReadPort, graph.internSymbol("state_rp"));
    graph.setAttr(readOp, "regSymbol", std::string("state"));
    const auto readVal = makeValue(graph, "state_read", 8, false);
    graph.addResult(readOp, readVal);

    const auto assignQ = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_q"));
    graph.addOperand(assignQ, readVal);
    graph.addResult(assignQ, outQ);

    const auto wen = makeConstant(graph, "wen", "wen_c", 1, "1'b1");
    const auto mask = makeConstant(graph, "mask", "mask_c", 8, "8'hff");
    const auto writeOp = graph.createOperation(OperationKind::kRegisterWritePort, graph.internSymbol("state_wp"));
    graph.addOperand(writeOp, wen);
    graph.addOperand(writeOp, inD);
    graph.addOperand(writeOp, mask);
    graph.addOperand(writeOp, clk);
    graph.setAttr(writeOp, "regSymbol", std::string("state"));
    graph.setAttr(writeOp, "clockSymbol", std::string("clk"));
    graph.setAttr(writeOp, "eventEdge", std::vector<std::string>{"negedge"});

    runGsim(design, "negedge_reg_top");

    const auto dir = artifactRoot() / "negedge_reg";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("negedge_reg_sim");
    options.topOverrides = {"negedge_reg_top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "negedge register emit should succeed");

    const std::filesystem::path driverPath = dir / "negedge_reg_driver.cpp";
    {
        std::ofstream driver(driverPath);
        driver << "#include \"negedge_reg_sim.hpp\"\n";
        driver << "#include \"negedge_reg_sim.cpp\"\n";
        driver << "#include <cstdio>\n";
        driver << "int main() {\n";
        driver << "    SSimTop sim;\n";
        driver << "    sim.set_clk(1); sim.set_d(0x12); sim.step();\n";
        driver << "    if (sim.get_q() != 0) { std::printf(\"FAIL posedge q=%u expected 0\\n\", static_cast<unsigned>(sim.get_q())); return 1; }\n";
        driver << "    sim.set_clk(0); sim.set_d(0x12); sim.step();\n";
        driver << "    if (sim.get_q() != 0x12u) { std::printf(\"FAIL negedge q=%u expected 18\\n\", static_cast<unsigned>(sim.get_q())); return 2; }\n";
        driver << "    return 0;\n";
        driver << "}\n";
    }

    const std::string exePath = (dir / "negedge_reg_driver").string();
    const std::string compileCmd =
        "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -o " + exePath + " " + driverPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "negedge register driver should compile");
    expect(std::system(exePath.c_str()) == 0, "negedge register should only update when clk is low");
}

void testMixedEdgeRegisterWriteUsesEachEventOperand()
{
    Design design;
    auto &graph = design.createGraph("mixed_edge_reg_top");
    design.markAsTop("mixed_edge_reg_top");

    const auto clk = makeValue(graph, "clk", 1, false);
    const auto rstn = makeValue(graph, "rst_n", 1, false);
    const auto inD = makeValue(graph, "d", 8, false);
    graph.bindInputPort("clk", clk);
    graph.bindInputPort("rst_n", rstn);
    graph.bindInputPort("d", inD);

    const auto outQ = makeValue(graph, "q", 8, false);
    graph.bindOutputPort("q", outQ);

    const auto regOp = graph.createOperation(OperationKind::kRegister, graph.internSymbol("state"));
    graph.setAttr(regOp, "initValue", std::string("8'h00"));
    const auto regOut = makeValue(graph, "state_out", 8, false);
    graph.addResult(regOp, regOut);

    const auto readOp = graph.createOperation(OperationKind::kRegisterReadPort, graph.internSymbol("state_rp"));
    graph.setAttr(readOp, "regSymbol", std::string("state"));
    const auto readVal = makeValue(graph, "state_read", 8, false);
    graph.addResult(readOp, readVal);

    const auto zero = makeConstant(graph, "zero", "zero_c", 8, "8'h00");
    const auto notRstn = makeValue(graph, "not_rst_n", 1, false);
    const auto notOp = graph.createOperation(OperationKind::kNot, graph.internSymbol("not_rst_n_op"));
    graph.addOperand(notOp, rstn);
    graph.addResult(notOp, notRstn);

    const auto writeVal = makeValue(graph, "state_write", 8, false);
    const auto mux = graph.createOperation(OperationKind::kMux, graph.internSymbol("state_sel"));
    graph.addOperand(mux, notRstn);
    graph.addOperand(mux, zero);
    graph.addOperand(mux, inD);
    graph.addResult(mux, writeVal);

    const auto assignQ = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_q"));
    graph.addOperand(assignQ, readVal);
    graph.addResult(assignQ, outQ);

    const auto wen = makeConstant(graph, "wen", "wen_c", 1, "1'b1");
    const auto mask = makeConstant(graph, "mask", "mask_c", 8, "8'hff");
    const auto writeOp = graph.createOperation(OperationKind::kRegisterWritePort, graph.internSymbol("state_wp"));
    graph.addOperand(writeOp, wen);
    graph.addOperand(writeOp, writeVal);
    graph.addOperand(writeOp, mask);
    graph.addOperand(writeOp, clk);
    graph.addOperand(writeOp, rstn);
    graph.setAttr(writeOp, "regSymbol", std::string("state"));
    graph.setAttr(writeOp, "clockSymbol", std::string("clk"));
    graph.setAttr(writeOp, "eventEdge", std::vector<std::string>{"posedge", "negedge"});

    runGsim(design, "mixed_edge_reg_top");

    const auto dir = artifactRoot() / "mixed_edge_reg";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("mixed_edge_reg_sim");
    options.topOverrides = {"mixed_edge_reg_top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "mixed-edge register emit should succeed");

    const std::filesystem::path driverPath = dir / "mixed_edge_reg_driver.cpp";
    {
        std::ofstream driver(driverPath);
        driver << "#include \"mixed_edge_reg_sim.hpp\"\n";
        driver << "#include \"mixed_edge_reg_sim.cpp\"\n";
        driver << "#include <cstdio>\n";
        driver << "int main() {\n";
        driver << "    SSimTop sim;\n";
        driver << "    sim.set_rst_n(1); sim.set_clk(1); sim.set_d(0x2a); sim.step();\n";
        driver << "    if (sim.get_q() != 0x2au) { std::printf(\"FAIL load q=%u expected 42\\n\", static_cast<unsigned>(sim.get_q())); return 1; }\n";
        driver << "    sim.set_rst_n(0); sim.set_clk(1); sim.set_d(0x55); sim.step();\n";
        driver << "    if (sim.get_q() != 0x00u) { std::printf(\"FAIL reset q=%u expected 0\\n\", static_cast<unsigned>(sim.get_q())); return 2; }\n";
        driver << "    return 0;\n";
        driver << "}\n";
    }

    const std::string exePath = (dir / "mixed_edge_reg_driver").string();
    const std::string compileCmd =
        "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -o " + exePath + " " + driverPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "mixed-edge register driver should compile");
    expect(std::system(exePath.c_str()) == 0, "mixed-edge register should use each event operand when deciding which edge fires");
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

void testRegisterInitValueBehavior()
{
    Design design = buildRegisterInitValueDesign();
    runGsim(design, "reg_init_top");

    const auto dir = artifactRoot() / "reg_init_value";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("reg_init_sim");
    options.topOverrides = {"reg_init_top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "register initValue emit should succeed");
    expect(!diags.hasError(), "register initValue emit should not emit diagnostics");

    const std::string source = readFile(dir / "reg_init_sim.cpp");
    expect(contains(source, "reg_state = 0x2aULL;"),
           "generated reset should preserve non-zero register initValue");

    const std::filesystem::path driverPath = dir / "reg_init_driver.cpp";
    {
        std::ofstream driver(driverPath);
        driver << "#include \"reg_init_sim.hpp\"\n";
        driver << "#include \"reg_init_sim.cpp\"\n";
        driver << "#include <cstdint>\n";
        driver << "int main() {\n";
        driver << "    SSimTop sim;\n";
        driver << "    sim.set_reset(1); sim.step();\n";
        driver << "    sim.set_reset(0); sim.step();\n";
        driver << "    return sim.get_y() == static_cast<std::uint8_t>(42) ? 0 : 1;\n";
        driver << "}\n";
    }

    const std::string exePath = (dir / "reg_init_driver").string();
    const std::string compileCmd =
        "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -o " + exePath + " " + driverPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "register initValue driver should compile");
    expect(std::system(exePath.c_str()) == 0, "register initValue driver should preserve non-zero initValue");
}

void testBootstrapResetFirstStepBehavior()
{
    Design design = buildNamedResetPassthroughDesign();
    runGsim(design, "reset_top");

    const auto dir = artifactRoot() / "bootstrap_reset_cycle";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("bootstrap_reset_sim");
    options.topOverrides = {"reset_top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "bootstrap reset regression emit should succeed");
    expect(!diags.hasError(), "bootstrap reset regression should not emit diagnostics");

    const std::filesystem::path driverPath = dir / "bootstrap_reset_driver.cpp";
    {
        std::ofstream driver(driverPath);
        driver << "#include \"bootstrap_reset_sim.hpp\"\n";
        driver << "#include \"bootstrap_reset_sim.cpp\"\n";
        driver << "#include <cstdio>\n";
        driver << "int main() {\n";
        driver << "    SSimTop sim;\n";
        driver << "    sim.set_reset(1);\n";
        driver << "    sim.step();\n";
        driver << "    if (sim.get_y() != 1) {\n";
        driver << "        std::printf(\"FAIL: first reset step y=%u expected=1\\n\", static_cast<unsigned>(sim.get_y()));\n";
        driver << "        return 1;\n";
        driver << "    }\n";
        driver << "    return 0;\n";
        driver << "}\n";
    }

    const std::string exePath = (dir / "bootstrap_reset_driver").string();
    const std::string compileCmd =
        "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -o " + exePath + " " + driverPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "bootstrap reset driver should compile");
    expect(std::system(exePath.c_str()) == 0,
           "first step after reset() should execute reset-controlled sequential logic");
}

void testNamedResetClockedRegisterAdvancesOnReleaseStep()
{
    Design design = buildNamedResetRegisterCycleDesign();
    runGsim(design, "reset_reg_top");

    const auto dir = artifactRoot() / "named_reset_register_cycle";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("named_reset_reg_sim");
    options.topOverrides = {"reset_reg_top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "named reset register emit should succeed");
    expect(!diags.hasError(), "named reset register emit should not emit diagnostics");

    const std::filesystem::path driverPath = dir / "named_reset_reg_driver.cpp";
    {
        std::ofstream driver(driverPath);
        driver << "#include \"named_reset_reg_sim.hpp\"\n";
        driver << "#include <cstdint>\n";
        driver << "static std::uint32_t g_dpi_calls = 0;\n";
        driver << "static std::uint32_t g_dpi_last = 0xffffffffU;\n";
        driver << "extern \"C\" void dpi_capture_state(std::uint8_t value) { ++g_dpi_calls; g_dpi_last = value; }\n";
        driver << "#include \"named_reset_reg_sim.cpp\"\n";
        driver << "int main() {\n";
        driver << "    SSimTop sim;\n";
        driver << "    sim.set_reset(1); sim.step();\n";
        driver << "    if (g_dpi_calls != 1 || g_dpi_last != 0) return 1;\n";
        driver << "    sim.set_reset(0); sim.step();\n";
        driver << "    if (g_dpi_calls != 2 || g_dpi_last != 1) return 2;\n";
        driver << "    return 0;\n";
        driver << "}\n";
    }

    const std::string exePath = (dir / "named_reset_reg_driver").string();
    const std::string compileCmd =
        "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -o " + exePath + " " + driverPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "named reset register driver should compile");
    expect(std::system(exePath.c_str()) == 0,
           "release step should expose the updated clocked state to posedge side effects");
}

void testNamedResetClockedRegisterReassertsToResetState()
{
    Design design = buildNamedResetRegisterCycleDesign();
    runGsim(design, "reset_reg_top");

    const auto dir = artifactRoot() / "named_reset_register_reassert";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("named_reset_reg_reassert_sim");
    options.topOverrides = {"reset_reg_top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "named reset register reassert emit should succeed");
    expect(!diags.hasError(), "named reset register reassert emit should not emit diagnostics");

    const std::filesystem::path driverPath = dir / "named_reset_reg_reassert_driver.cpp";
    {
        std::ofstream driver(driverPath);
        driver << "#include \"named_reset_reg_reassert_sim.hpp\"\n";
        driver << "#include <cstdint>\n";
        driver << "#include <cstdio>\n";
        driver << "static std::uint32_t g_dpi_calls = 0;\n";
        driver << "static std::uint32_t g_dpi_last = 0xffffffffU;\n";
        driver << "extern \"C\" void dpi_capture_state(std::uint8_t value) { ++g_dpi_calls; g_dpi_last = value; }\n";
        driver << "#include \"named_reset_reg_reassert_sim.cpp\"\n";
        driver << "int main() {\n";
        driver << "    SSimTop sim;\n";
        driver << "    sim.set_reset(1); sim.step();\n";
        driver << "    if (g_dpi_calls != 1 || g_dpi_last != 0 || sim.get_y() != 0) {\n";
        driver << "        std::printf(\"FAIL: first reset calls=%u last=%u y=%u\\n\", g_dpi_calls, g_dpi_last, static_cast<unsigned>(sim.get_y()));\n";
        driver << "        return 1;\n";
        driver << "    }\n";
        driver << "    sim.set_reset(0); sim.step();\n";
        driver << "    if (g_dpi_calls != 2 || g_dpi_last != 1 || sim.get_y() != 1) {\n";
        driver << "        std::printf(\"FAIL: release calls=%u last=%u y=%u\\n\", g_dpi_calls, g_dpi_last, static_cast<unsigned>(sim.get_y()));\n";
        driver << "        return 2;\n";
        driver << "    }\n";
        driver << "    sim.set_reset(1); sim.step();\n";
        driver << "    if (g_dpi_calls != 3 || g_dpi_last != 0 || sim.get_y() != 0) {\n";
        driver << "        std::printf(\"FAIL: reassert calls=%u last=%u y=%u\\n\", g_dpi_calls, g_dpi_last, static_cast<unsigned>(sim.get_y()));\n";
        driver << "        return 3;\n";
        driver << "    }\n";
        driver << "    return 0;\n";
        driver << "}\n";
    }

    const std::string exePath = (dir / "named_reset_reg_reassert_driver").string();
    const std::string compileCmd =
        "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -o " + exePath + " " + driverPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "named reset register reassert driver should compile");
    expect(std::system(exePath.c_str()) == 0,
           "reasserted reset should return clocked state and posedge side effects to the reset value");
}

void testSelfCompareDoesNotEmitClangTautologyWarning()
{
    Design design = buildSelfCompareEqDesign();
    runGsim(design, "self_eq_top");

    const auto dir = artifactRoot() / "self_compare_warning";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("self_compare_sim");
    options.topOverrides = {"self_eq_top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "self-compare warning emit should succeed");
    expect(!diags.hasError(), "self-compare warning emit should not emit diagnostics");

    const std::filesystem::path wrapperPath = dir / "self_compare_compile.cpp";
    {
        std::ofstream wrapper(wrapperPath);
        wrapper << "#include \"self_compare_sim.hpp\"\n";
        wrapper << "#include \"self_compare_sim.cpp\"\n";
        wrapper << "int main() { SSimTop sim; sim.set_a(7); sim.step(); return sim.get_y() == 1 ? 0 : 1; }\n";
    }

    const std::string compileCmd =
        "clang++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -fsyntax-only " + wrapperPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0,
           "self-compare lowering should compile without clang tautology warnings");
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
    options.attributes["behavior_shard_max_bytes"] = "1200";

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "behavior-sharded emit should succeed");
    expect(!diags.hasError(), "behavior-sharded emit should not emit diagnostics");

    const auto behaviorShards = findBehaviorShards(dir, "chain_split");
    expect(!behaviorShards.empty(), "tiny behavior shard threshold should produce step shards");
    for (const auto &shard : behaviorShards)
    {
        expect(static_cast<std::size_t>(std::filesystem::file_size(shard)) <= 1200,
               "behavior shard should stay within the configured byte budget");
    }

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

void testBehaviorShardsRejectUnshardableStatement()
{
    Design design = buildLinearAddChainDesign(8);
    runGsim(design, "chain_top");

    const auto dir = artifactRoot() / "behavior_shards_reject_tiny";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("chain_tiny_cap");
    options.topOverrides = {"chain_top"};
    options.attributes["behavior_shard_max_bytes"] = "8";

    const EmitResult result = emitter.emit(design, options);
    expect(!result.success, "emit should fail when a single statement cannot fit within the shard budget");
    expect(diags.hasError(), "unshardable behavior statement should produce diagnostics");
    expectDiagnosticsContain(diags, "behavior_shard_max_bytes");
}

void testWideBehaviorShardsKeepTypedTemps()
{
    Design design = buildWideBehaviorConcatDesign(128);
    runGsim(design, "wide_behavior_top");

    const auto dir = artifactRoot() / "wide_behavior_shards";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("wide_behavior_split");
    options.topOverrides = {"wide_behavior_top"};
    options.attributes["behavior_shard_max_bytes"] = "512";

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "wide behavior-sharded emit should succeed");
    expect(!diags.hasError(), "wide behavior-sharded emit should not emit diagnostics");

    const auto behaviorShards = findBehaviorShards(dir, "wide_behavior_split");
    expect(!behaviorShards.empty(), "wide behavior design should emit behavior shards");

    const std::string header = readFile(dir / "wide_behavior_split.hpp");
    expect(!contains(header, "std::vector<std::uint64_t> step_tmp_"),
           "behavior shards should not store typed temporaries in a uint64_t vector");
    expect(!contains(header, "wolvrix::gsim::Bits<128> step_tmp_"),
           "behavior shards should not emit one persistent member per wide temporary");
    expect(contains(header, "std::vector<wolvrix::gsim::Bits<128>> step_tmp_group_"),
           "wide behavior shards should preserve wide typed temporaries in grouped storage");

    const std::string source = readFile(dir / "wide_behavior_split.cpp");
    expect(contains(source, "step_tmp_group_"),
           "wide behavior shards should size grouped persistent temporary storage in the source");

    std::size_t observedMaxLine = 0;
    for (const auto &shard : behaviorShards)
    {
        observedMaxLine = std::max(observedMaxLine, maxLineLength(shard));
    }
    expect(observedMaxLine < 4096,
           "wide behavior shard lowering should avoid giant single-line expressions");

    const auto manifestLines = readLines(dir / "wide_behavior_split.manifest");
    expect(!manifestLines.empty(), "wide behavior-sharded emit should still write a manifest");

    const std::filesystem::path wrapperPath = dir / "wide_behavior_compile.cpp";
    {
        std::ofstream wrapper(wrapperPath);
        wrapper << "#include \"wide_behavior_split.hpp\"\n";
        for (const auto &name : manifestLines)
        {
            wrapper << "#include \"" << name << "\"\n";
        }
        wrapper << "int main() { SSimTop sim; return 0; }\n";
    }

    const std::string compileCmd =
        "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -fsyntax-only " + wrapperPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "wide behavior shard driver should compile");
}

void testWideBehaviorShardsSplitCtorStorageInit()
{
    Design design = buildWideBehaviorConcatDesign(128);
    runGsim(design, "wide_behavior_top");

    const auto dir = artifactRoot() / "wide_behavior_ctor_init_shards";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("wide_behavior_ctor_split");
    options.topOverrides = {"wide_behavior_top"};
    options.attributes["behavior_shard_max_bytes"] = "512";

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "wide behavior ctor-split emit should succeed");
    expect(!diags.hasError(), "wide behavior ctor-split emit should not emit diagnostics");

    const std::string header = readFile(dir / "wide_behavior_ctor_split.hpp");
    const std::string source = readFile(dir / "wide_behavior_ctor_split.cpp");
    expect(contains(header, "void init_ctor_storage_shard_0();"),
           "wide behavior ctor-split header should declare constructor storage init shards");
    expect(contains(source, "void SSimTop::init_ctor_storage_shard_0()"),
           "wide behavior ctor-split source should define constructor storage init shards");
    expect(contains(source, "    init_ctor_storage_shard_0();"),
           "wide behavior ctor-split constructor should call the first storage init shard");
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
    expect(contains(header, "std::vector<std::uint8_t> mem_mem0_;"),
           "memory lowering should leave vector-backed storage default-constructed in the header");
    expect(!contains(header, "std::vector<std::uint8_t> mem_mem0_ = std::vector<std::uint8_t>(4, 0);"),
           "memory lowering should not rely on in-class vector default initializers");
    expect(contains(source, "mem_mem0_.resize(4);"),
           "memory lowering should allocate vector-backed storage in the constructor");
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

void testDivideByZeroBehavior()
{
    Design design = buildDivideByZeroBehaviorDesign();
    runGsim(design, "div_top");

    const auto dir = artifactRoot() / "divide_by_zero_behavior";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("div_sim");
    options.topOverrides = {"div_top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "divide-by-zero lowering emit should succeed");
    expect(!diags.hasError(), "divide-by-zero lowering emit should not emit diagnostics");

    const std::filesystem::path driverPath = dir / "div_driver.cpp";
    {
        std::ofstream driver(driverPath);
        driver << "#include \"div_sim.hpp\"\n";
        driver << "#include \"div_sim.cpp\"\n";
        driver << "#include <cstdio>\n";
        driver << "int main() {\n";
        driver << "    SSimTop sim;\n";
        driver << "    sim.set_reset(1); sim.step();\n";
        driver << "    sim.set_reset(0);\n";
        driver << "    sim.set_lhs(9); sim.set_rhs(0); sim.step();\n";
        driver << "    if (sim.get_q() != 0 || sim.get_r() != 0) {\n";
        driver << "        std::printf(\"FAIL zero-div %u %u\\n\", static_cast<unsigned>(sim.get_q()), static_cast<unsigned>(sim.get_r()));\n";
        driver << "        return 1;\n";
        driver << "    }\n";
        driver << "    sim.set_rhs(4); sim.step();\n";
        driver << "    if (sim.get_q() != 2 || sim.get_r() != 1) {\n";
        driver << "        std::printf(\"FAIL normal-div %u %u\\n\", static_cast<unsigned>(sim.get_q()), static_cast<unsigned>(sim.get_r()));\n";
        driver << "        return 1;\n";
        driver << "    }\n";
        driver << "    return 0;\n";
        driver << "}\n";
    }

    const std::string exePath = (dir / "div_driver").string();
    const std::string compileCmd =
        "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -o " + exePath + " " + driverPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "divide-by-zero driver should compile");
    expect(std::system(exePath.c_str()) == 0, "divide-by-zero driver should run successfully");
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

    const std::string headerText = readFile(dir / "dpic_sim.hpp");
    const std::string sourceText = readFile(dir / "dpic_sim.cpp");
    expect(headerText.find("dpi_capture(") == std::string::npos,
           "dpic side-effect header should not expose DPI forward declarations");
    expect(sourceText.find("extern \"C\"") != std::string::npos,
           "dpic side-effect source should keep local DPI forward declarations");
    expect(sourceText.find("dpi_capture(") != std::string::npos,
           "dpic side-effect source should declare the imported DPI symbol");

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
        driver << "    if (g_dpi_calls != 1 || g_dpi_last != 0) return 1;\n";
        driver << "    sim.set_reset(0);\n";
        driver << "    sim.set_in(7); sim.step();\n";
        driver << "    if (sim.get_y() != 7) return 2;\n";
        driver << "    if (g_dpi_calls != 2 || g_dpi_last != 7) return 3;\n";
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

void testDpicSignedIntOutputBehavior()
{
    Design design = buildDpicSignedIntOutputDesign();
    runGsim(design, "dpic_signed_out_top");

    const auto dir = artifactRoot() / "dpic_signed_out";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("dpic_signed_out_sim");
    options.topOverrides = {"dpic_signed_out_top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "signed-int-output dpic emit should succeed");
    expect(!diags.hasError(), "signed-int-output dpic emit should not emit diagnostics");

    const std::filesystem::path driverPath = dir / "dpic_signed_out_driver.cpp";
    {
        std::ofstream driver(driverPath);
        driver << "#include \"dpic_signed_out_sim.hpp\"\n";
        driver << "#include <cstdint>\n";
        driver << "extern \"C\" void dpi_fill_int(int *value) { *value = -5; }\n";
        driver << "#include \"dpic_signed_out_sim.cpp\"\n";
        driver << "int main() {\n";
        driver << "    SSimTop sim;\n";
        driver << "    sim.set_reset(1); sim.step();\n";
        driver << "    sim.step();\n";
        driver << "    if (sim.get_y() != static_cast<std::uint32_t>(0xfffffffbU)) return 1;\n";
        driver << "    return 0;\n";
        driver << "}\n";
    }

    const std::string exePath = (dir / "dpic_signed_out_driver").string();
    const std::string compileCmd =
        "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -o " + exePath + " " + driverPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "dpic signed-int-output driver should compile");
    expect(std::system(exePath.c_str()) == 0, "dpic signed-int-output driver should run successfully");
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

void testWideDynamicShiftCompile()
{
    Design design = buildWideDynamicShiftDesign();
    runGsim(design, "wide_shift_top");

    const auto dir = artifactRoot() / "wide_dynamic_shift";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("wide_shift_sim");
    options.topOverrides = {"wide_shift_top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "wide dynamic shift emit should succeed");
    expect(!diags.hasError(), "wide dynamic shift emit should not emit diagnostics");

    const std::filesystem::path wrapperPath = dir / "wide_shift_compile.cpp";
    {
        std::ofstream wrapper(wrapperPath);
        wrapper << "#include \"wide_shift_sim.hpp\"\n";
        wrapper << "#include \"wide_shift_sim.cpp\"\n";
        wrapper << "int main() { SSimTop sim; return 0; }\n";
    }

    const std::string compileCmd =
        "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -fsyntax-only " + wrapperPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "wide dynamic shift driver should compile");
}

void testWideSliceCompile()
{
    Design design = buildWideSliceDesign();
    runGsim(design, "wide_slice_top");

    const auto dir = artifactRoot() / "wide_slice";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("wide_slice_sim");
    options.topOverrides = {"wide_slice_top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "wide slice emit should succeed");
    expect(!diags.hasError(), "wide slice emit should not emit diagnostics");

    const std::filesystem::path wrapperPath = dir / "wide_slice_compile.cpp";
    {
        std::ofstream wrapper(wrapperPath);
        wrapper << "#include \"wide_slice_sim.hpp\"\n";
        wrapper << "#include \"wide_slice_sim.cpp\"\n";
        wrapper << "int main() { SSimTop sim; return 0; }\n";
    }

    const std::string compileCmd =
        "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -fsyntax-only " + wrapperPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "wide slice driver should compile");
}

void testNarrowBehaviorAshrCompile()
{
    Design design = buildNarrowBehaviorAshrDesign(4096);
    runGsim(design, "narrow_ashr_top");

    const auto dir = artifactRoot() / "narrow_behavior_ashr";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("narrow_ashr_split");
    options.topOverrides = {"narrow_ashr_top"};
    options.attributes["behavior_shard_max_bytes"] = "65536";

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "narrow behavior ashr emit should succeed");
    expect(!diags.hasError(), "narrow behavior ashr emit should not emit diagnostics");

    const std::string header = readFile(dir / "narrow_ashr_split.hpp");
    expect(contains(header, "std::vector<std::uint32_t> step_tmp_group_"),
           "narrow behavior ashr should use grouped uint32 temporary storage when sharded");

    const auto manifestLines = readLines(dir / "narrow_ashr_split.manifest");
    expect(!manifestLines.empty(), "narrow behavior ashr should emit a managed source manifest");

    const std::filesystem::path wrapperPath = dir / "narrow_ashr_compile.cpp";
    {
        std::ofstream wrapper(wrapperPath);
        wrapper << "#include \"narrow_ashr_split.hpp\"\n";
        for (const auto &name : manifestLines)
        {
            wrapper << "#include \"" << name << "\"\n";
        }
        wrapper << "int main() { SSimTop sim; return 0; }\n";
    }

    const std::string compileCmd =
        "g++ -std=c++17 -Wall -Wextra -Werror -I " + dir.string() +
        " -fsyntax-only " + wrapperPath.string() + " 2>&1";
    expect(std::system(compileCmd.c_str()) == 0, "narrow behavior ashr source set should compile");
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
        testResetPortNamedResetUsesInputSetter();
        testPortOrderDecl();
        testPortOrderAlpha();
        testPortOrderCustom();
        testPortOrderInvalidName();
        testPortOrderDuplicateName();
        testVersionMismatchRejection();
        testHypergraphVersionMismatchRejection();
        testMetadataShardsKeepAssignmentsWhole();
        testMetadataShardsRespectByteBudgetForLargeMetadata();
        testMetadataStatementsStaySmallUnderLargeShardBudget();
        testRegisterLatencyBehavior();
        testRegisterDerivedOutputBehavior();
        testRegisterChainKeepsPreStepState();
        testNamedResetInputWithClockDoesNotForceInitState();
        testNegedgeRegisterWriteHonorsClockLevel();
        testMixedEdgeRegisterWriteUsesEachEventOperand();
        testReplicateSignExtendBehavior();
        testRegisterInitValueBehavior();
        testBootstrapResetFirstStepBehavior();
        testNamedResetClockedRegisterAdvancesOnReleaseStep();
        testNamedResetClockedRegisterReassertsToResetState();
        testSelfCompareDoesNotEmitClangTautologyWarning();
        testLargeCombinationalChainUsesMaterializedTemporaries();
        testBehaviorShardsManifestAndCompile();
        testBehaviorShardsRejectUnshardableStatement();
        testMemoryLoweringBehavior();
        testDivideByZeroBehavior();
        testDpicSideEffectCallBehavior();
        testDpicReturnAndOutputBehavior();
        testDpicSignedIntOutputBehavior();
        testDpicStringInputBehavior();
        testUnknownConstantBehavior();
        testWideDynamicShiftCompile();
        testWideSliceCompile();
        testNarrowBehaviorAshrCompile();
        testWideBehaviorShardsKeepTypedTemps();
        testWideBehaviorShardsSplitCtorStorageInit();
    }
    catch (const std::exception &ex)
    {
        return fail(ex.what());
    }
    return 0;
}
