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

std::size_t countOccurrences(std::string_view text, std::string_view needle)
{
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string_view::npos)
    {
        ++count;
        pos += needle.size();
    }
    return count;
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

OperationId makeLatch(Graph &graph,
                      const std::string &opName,
                      int32_t width,
                      const std::string &latchSymbol)
{
    const auto op = graph.createOperation(OperationKind::kLatch, graph.internSymbol(opName));
    graph.setAttr(op, "latchSymbol", latchSymbol);
    graph.setAttr(op, "width", static_cast<int64_t>(width));
    graph.setAttr(op, "isSigned", false);
    return op;
}

ValueId makeLatchRead(Graph &graph,
                      const std::string &valueName,
                      const std::string &opName,
                      int32_t width,
                      const std::string &latchSymbol)
{
    const auto value = graph.createValue(graph.internSymbol(valueName), width, false);
    const auto op = graph.createOperation(OperationKind::kLatchReadPort, graph.internSymbol(opName));
    graph.addResult(op, value);
    graph.setAttr(op, "latchSymbol", latchSymbol);
    return value;
}

OperationId makeMemory(Graph &graph,
                       int32_t width,
                       int64_t row,
                       const std::string &memSymbol)
{
    const auto op = graph.createOperation(OperationKind::kMemory, graph.internSymbol(memSymbol));
    graph.setAttr(op, "width", static_cast<int64_t>(width));
    graph.setAttr(op, "row", row);
    graph.setAttr(op, "isSigned", false);
    graph.setAttr(op, "initKind", std::vector<std::string>{"literal"});
    graph.setAttr(op, "initFile", std::vector<std::string>{""});
    graph.setAttr(op, "initValue", std::vector<std::string>{"8'hA5"});
    graph.setAttr(op, "initStart", std::vector<int64_t>{2});
    graph.setAttr(op, "initLen", std::vector<int64_t>{1});
    return op;
}

ValueId makeMemoryRead(Graph &graph,
                       const std::string &valueName,
                       const std::string &opName,
                       int32_t width,
                       ValueId addr,
                       const std::string &memSymbol)
{
    const auto value = graph.createValue(graph.internSymbol(valueName), width, false);
    const auto op = graph.createOperation(OperationKind::kMemoryReadPort, graph.internSymbol(opName));
    graph.addOperand(op, addr);
    graph.addResult(op, value);
    graph.setAttr(op, "memSymbol", memSymbol);
    return value;
}

OperationId makeMemoryWrite(Graph &graph,
                            const std::string &opName,
                            ValueId updateCond,
                            ValueId addr,
                            ValueId data,
                            ValueId maskValue,
                            ValueId clk,
                            const std::string &memSymbol)
{
    const auto op = graph.createOperation(OperationKind::kMemoryWritePort,
                                          graph.internSymbol(opName));
    graph.addOperand(op, updateCond);
    graph.addOperand(op, addr);
    graph.addOperand(op, data);
    graph.addOperand(op, maskValue);
    graph.addOperand(op, clk);
    graph.setAttr(op, "memSymbol", memSymbol);
    graph.setAttr(op, "eventEdge", std::vector<std::string>{"posedge"});
    return op;
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

OperationId makeRegisterWriteWithEdge(Graph &graph,
                                      const std::string &opName,
                                      ValueId updateCond,
                                      ValueId nextValue,
                                      ValueId maskValue,
                                      ValueId clk,
                                      const std::string &regSymbol,
                                      const std::string &edge)
{
    const auto op = graph.createOperation(OperationKind::kRegisterWritePort,
                                          graph.internSymbol(opName));
    graph.addOperand(op, updateCond);
    graph.addOperand(op, nextValue);
    graph.addOperand(op, maskValue);
    graph.addOperand(op, clk);
    graph.setAttr(op, "regSymbol", regSymbol);
    graph.setAttr(op, "clockSymbol", std::string("clk"));
    graph.setAttr(op, "eventEdge", std::vector<std::string>{edge});
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

Design buildDifftestTopPortDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto exitIn = makeValue(graph, "exit_in", 64, false);
    const auto stepIn = makeValue(graph, "step_in", 64, false);
    const auto exitOut = makeValue(graph, "difftest_exit", 64, false);
    const auto stepOut = makeValue(graph, "difftest_step", 64, false);
    graph.bindInputPort("exit_in", exitIn);
    graph.bindInputPort("step_in", stepIn);
    graph.bindOutputPort("difftest_exit", exitOut);
    graph.bindOutputPort("difftest_step", stepOut);

    const auto assignExit = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_difftest_exit"));
    graph.addOperand(assignExit, exitIn);
    graph.addResult(assignExit, exitOut);

    const auto assignStep = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_difftest_step"));
    graph.addOperand(assignStep, stepIn);
    graph.addResult(assignStep, stepOut);

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

Design buildResetPortForwardingDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto reset = makeValue(graph, "reset", 1, false);
    const auto outY = makeValue(graph, "y", 1, false);
    graph.bindInputPort("reset", reset);
    graph.bindOutputPort("y", outY);

    const auto assign = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_reset_to_y"));
    graph.addOperand(assign, reset);
    graph.addResult(assign, outY);
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


Design buildDpicCallDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto inA = makeValue(graph, "a", 8, false);
    const auto clk = makeValue(graph, "clk", 1, false);
    graph.bindInputPort("a", inA);
    graph.bindInputPort("clk", clk);

    const auto outY = makeValue(graph, "y", 8, false);
    graph.bindOutputPort("y", outY);
    const auto assign = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_y"));
    graph.addOperand(assign, inA);
    graph.addResult(assign, outY);

    const auto dpiImport = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("dpi_capture"));
    graph.setAttr(dpiImport, "argsDirection", std::vector<std::string>{"input"});
    graph.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{8});
    graph.setAttr(dpiImport, "argsName", std::vector<std::string>{"value"});
    graph.setAttr(dpiImport, "argsSigned", std::vector<bool>{false});
    graph.setAttr(dpiImport, "argsType", std::vector<std::string>{"byte"});
    graph.setAttr(dpiImport, "hasReturn", false);
    graph.setAttr(dpiImport, "returnType", std::string("void"));

    const auto one = makeConstant(graph, "one", "one_const", 1, "1'b1");
    const auto dpiCall = graph.createOperation(OperationKind::kDpicCall, graph.internSymbol("call_capture"));
    graph.addOperand(dpiCall, one);
    graph.addOperand(dpiCall, inA);
    graph.addOperand(dpiCall, clk);
    graph.setAttr(dpiCall, "targetImportSymbol", std::string("dpi_capture"));
    graph.setAttr(dpiCall, "inArgName", std::vector<std::string>{"value"});
    graph.setAttr(dpiCall, "outArgName", std::vector<std::string>{});
    graph.setAttr(dpiCall, "hasReturn", false);
    graph.setAttr(dpiCall, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(dpiCall, "clkPolarity", std::string("posedge"));

    return design;
}

Design buildNoOutputShardedDpicConditionDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    auto enCurrent = makeValue(graph, "en", 1, false);
    const auto clk = makeValue(graph, "clk", 1, false);
    graph.bindInputPort("en", enCurrent);
    graph.bindInputPort("clk", clk);

    for (int i = 0; i < 140; ++i)
    {
        const auto next = makeValue(graph, "en_chain_" + std::to_string(i), 1, false);
        const auto op = graph.createOperation(OperationKind::kNot,
                                              graph.internSymbol("en_chain_not_" + std::to_string(i)));
        graph.addOperand(op, enCurrent);
        graph.addResult(op, next);
        enCurrent = next;
    }

    const auto dpiImport = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("dpi_capture"));
    graph.setAttr(dpiImport, "argsDirection", std::vector<std::string>{"input"});
    graph.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{1});
    graph.setAttr(dpiImport, "argsName", std::vector<std::string>{"value"});
    graph.setAttr(dpiImport, "argsSigned", std::vector<bool>{false});
    graph.setAttr(dpiImport, "argsType", std::vector<std::string>{"bit"});
    graph.setAttr(dpiImport, "hasReturn", false);
    graph.setAttr(dpiImport, "returnType", std::string("void"));

    const auto dpiCall = graph.createOperation(OperationKind::kDpicCall, graph.internSymbol("call_capture"));
    graph.addOperand(dpiCall, enCurrent);
    graph.addOperand(dpiCall, enCurrent);
    graph.addOperand(dpiCall, clk);
    graph.setAttr(dpiCall, "targetImportSymbol", std::string("dpi_capture"));
    graph.setAttr(dpiCall, "inArgName", std::vector<std::string>{"value"});
    graph.setAttr(dpiCall, "outArgName", std::vector<std::string>{});
    graph.setAttr(dpiCall, "hasReturn", false);
    graph.setAttr(dpiCall, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(dpiCall, "clkPolarity", std::string("posedge"));

    return design;
}

Design buildNoOutputDpicConditionWithProducerClosureDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto en = makeValue(graph, "en", 1, false);
    const auto clk = makeValue(graph, "clk", 1, false);
    graph.bindInputPort("en", en);
    graph.bindInputPort("clk", clk);

    const auto constSeed = makeConstant(graph, "const_seed", "const_seed_op", 1, "1'b1");
    auto constCurrent = constSeed;
    for (int i = 0; i < 176; ++i)
    {
        const auto next = makeValue(graph, "producer_tmp_" + std::to_string(i), 1, false);
        const auto op = graph.createOperation(OperationKind::kNot,
                                              graph.internSymbol("producer_not_" + std::to_string(i)));
        graph.addOperand(op, constCurrent);
        graph.addResult(op, next);
        constCurrent = next;
    }

    const auto cond = makeValue(graph, "capture_cond", 1, false);
    const auto andOp = graph.createOperation(OperationKind::kLogicAnd, graph.internSymbol("capture_cond_and"));
    graph.addOperand(andOp, en);
    graph.addOperand(andOp, constCurrent);
    graph.addResult(andOp, cond);

    const auto dpiImport = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("dpi_capture"));
    graph.setAttr(dpiImport, "argsDirection", std::vector<std::string>{"input"});
    graph.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{1});
    graph.setAttr(dpiImport, "argsName", std::vector<std::string>{"value"});
    graph.setAttr(dpiImport, "argsSigned", std::vector<bool>{false});
    graph.setAttr(dpiImport, "argsType", std::vector<std::string>{"bit"});
    graph.setAttr(dpiImport, "hasReturn", false);
    graph.setAttr(dpiImport, "returnType", std::string("void"));

    const auto dpiCall = graph.createOperation(OperationKind::kDpicCall, graph.internSymbol("call_capture"));
    graph.addOperand(dpiCall, cond);
    graph.addOperand(dpiCall, cond);
    graph.addOperand(dpiCall, clk);
    graph.setAttr(dpiCall, "targetImportSymbol", std::string("dpi_capture"));
    graph.setAttr(dpiCall, "inArgName", std::vector<std::string>{"value"});
    graph.setAttr(dpiCall, "outArgName", std::vector<std::string>{});
    graph.setAttr(dpiCall, "hasReturn", false);
    graph.setAttr(dpiCall, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(dpiCall, "clkPolarity", std::string("posedge"));

    return design;
}

Design buildNoOutputLatchGatedDpicDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto en = makeValue(graph, "en", 1, false);
    const auto d = makeValue(graph, "d", 1, false);
    const auto clk = makeValue(graph, "clk", 1, false);
    graph.bindInputPort("en", en);
    graph.bindInputPort("d", d);
    graph.bindInputPort("clk", clk);

    (void)makeLatch(graph, "gate_latch_decl", 1, "gate_latch");
    const auto gate = makeLatchRead(graph, "gate_latch_q", "gate_latch_read", 1, "gate_latch");

    const auto mask = makeConstant(graph, "mask", "mask_const", 1, "1'b1");
    const auto latchWrite = graph.createOperation(OperationKind::kLatchWritePort, graph.internSymbol("gate_latch_write"));
    graph.addOperand(latchWrite, en);
    graph.addOperand(latchWrite, d);
    graph.addOperand(latchWrite, mask);
    graph.setAttr(latchWrite, "latchSymbol", std::string("gate_latch"));

    for (int i = 0; i < 140; ++i)
    {
        (void)makeConstant(graph, "latch_padding_const_" + std::to_string(i),
                           "latch_padding_const_op_" + std::to_string(i), 1, "1'b0");
    }

    const auto dpiImport = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("dpi_capture"));
    graph.setAttr(dpiImport, "argsDirection", std::vector<std::string>{"input"});
    graph.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{1});
    graph.setAttr(dpiImport, "argsName", std::vector<std::string>{"value"});
    graph.setAttr(dpiImport, "argsSigned", std::vector<bool>{false});
    graph.setAttr(dpiImport, "argsType", std::vector<std::string>{"bit"});
    graph.setAttr(dpiImport, "hasReturn", false);
    graph.setAttr(dpiImport, "returnType", std::string("void"));

    const auto dpiCall = graph.createOperation(OperationKind::kDpicCall, graph.internSymbol("call_capture_latch"));
    graph.addOperand(dpiCall, gate);
    graph.addOperand(dpiCall, gate);
    graph.addOperand(dpiCall, clk);
    graph.setAttr(dpiCall, "targetImportSymbol", std::string("dpi_capture"));
    graph.setAttr(dpiCall, "inArgName", std::vector<std::string>{"value"});
    graph.setAttr(dpiCall, "outArgName", std::vector<std::string>{});
    graph.setAttr(dpiCall, "hasReturn", false);
    graph.setAttr(dpiCall, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(dpiCall, "clkPolarity", std::string("posedge"));

    return design;
}

Design buildNoDiffDifftestDpicDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto clk = makeValue(graph, "clk", 1, false);
    graph.bindInputPort("clk", clk);

    const auto one = makeConstant(graph, "one", "one_const", 1, "1'b1");

    const auto dpiImport = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("v_difftest_TestEvent"));
    graph.setAttr(dpiImport, "argsDirection", std::vector<std::string>{"input"});
    graph.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{1});
    graph.setAttr(dpiImport, "argsName", std::vector<std::string>{"value"});
    graph.setAttr(dpiImport, "argsSigned", std::vector<bool>{false});
    graph.setAttr(dpiImport, "argsType", std::vector<std::string>{"bit"});
    graph.setAttr(dpiImport, "hasReturn", false);
    graph.setAttr(dpiImport, "returnType", std::string("void"));

    const auto dpiCall = graph.createOperation(OperationKind::kDpicCall, graph.internSymbol("call_difftest_event"));
    graph.addOperand(dpiCall, one);
    graph.addOperand(dpiCall, one);
    graph.addOperand(dpiCall, clk);
    graph.setAttr(dpiCall, "targetImportSymbol", std::string("v_difftest_TestEvent"));
    graph.setAttr(dpiCall, "inArgName", std::vector<std::string>{"value"});
    graph.setAttr(dpiCall, "outArgName", std::vector<std::string>{});
    graph.setAttr(dpiCall, "hasReturn", false);
    graph.setAttr(dpiCall, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(dpiCall, "clkPolarity", std::string("posedge"));

    for (int i = 0; i < 140; ++i)
    {
        (void)makeConstant(graph, "difftest_padding_const_" + std::to_string(i),
                           "difftest_padding_const_op_" + std::to_string(i), 1, "1'b0");
    }

    return design;
}

Design buildNoDiffValueDifftestDpicDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto clk = makeValue(graph, "clk", 1, false);
    graph.bindInputPort("clk", clk);

    const auto one = makeConstant(graph, "value_one", "value_one_const", 1, "1'b1");

    const auto returnImport = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("v_difftest_ReturnValue"));
    graph.setAttr(returnImport, "argsDirection", std::vector<std::string>{"input"});
    graph.setAttr(returnImport, "argsWidth", std::vector<int64_t>{1});
    graph.setAttr(returnImport, "argsName", std::vector<std::string>{"value"});
    graph.setAttr(returnImport, "argsSigned", std::vector<bool>{false});
    graph.setAttr(returnImport, "argsType", std::vector<std::string>{"bit"});
    graph.setAttr(returnImport, "hasReturn", true);
    graph.setAttr(returnImport, "returnType", std::string("byte"));

    const auto returnValue = makeValue(graph, "return_value", 8, false);
    const auto returnCall = graph.createOperation(OperationKind::kDpicCall, graph.internSymbol("call_return_value"));
    graph.addOperand(returnCall, one);
    graph.addOperand(returnCall, one);
    graph.addOperand(returnCall, clk);
    graph.addResult(returnCall, returnValue);
    graph.setAttr(returnCall, "targetImportSymbol", std::string("v_difftest_ReturnValue"));
    graph.setAttr(returnCall, "inArgName", std::vector<std::string>{"value"});
    graph.setAttr(returnCall, "outArgName", std::vector<std::string>{});
    graph.setAttr(returnCall, "hasReturn", true);
    graph.setAttr(returnCall, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(returnCall, "clkPolarity", std::string("posedge"));
    graph.bindOutputPort("ret", returnValue);

    const auto outputImport = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("v_difftest_OutputValue"));
    graph.setAttr(outputImport, "argsDirection", std::vector<std::string>{"output"});
    graph.setAttr(outputImport, "argsWidth", std::vector<int64_t>{8});
    graph.setAttr(outputImport, "argsName", std::vector<std::string>{"value"});
    graph.setAttr(outputImport, "argsSigned", std::vector<bool>{false});
    graph.setAttr(outputImport, "argsType", std::vector<std::string>{"byte"});
    graph.setAttr(outputImport, "hasReturn", false);
    graph.setAttr(outputImport, "returnType", std::string("void"));

    const auto outputValue = makeValue(graph, "output_value", 8, false);
    const auto outputCall = graph.createOperation(OperationKind::kDpicCall, graph.internSymbol("call_output_value"));
    graph.addOperand(outputCall, one);
    graph.addOperand(outputCall, clk);
    graph.addResult(outputCall, outputValue);
    graph.setAttr(outputCall, "targetImportSymbol", std::string("v_difftest_OutputValue"));
    graph.setAttr(outputCall, "inArgName", std::vector<std::string>{});
    graph.setAttr(outputCall, "outArgName", std::vector<std::string>{"value"});
    graph.setAttr(outputCall, "hasReturn", false);
    graph.setAttr(outputCall, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(outputCall, "clkPolarity", std::string("posedge"));
    graph.bindOutputPort("out", outputValue);

    for (int i = 0; i < 140; ++i)
    {
        (void)makeConstant(graph, "value_difftest_padding_const_" + std::to_string(i),
                           "value_difftest_padding_const_op_" + std::to_string(i), 1, "1'b0");
    }

    return design;
}

Design buildNoDiffMultiResultDifftestDpicDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto clk = makeValue(graph, "clk", 1, false);
    graph.bindInputPort("clk", clk);

    const auto one = makeConstant(graph, "multi_one", "multi_one_const", 1, "1'b1");

    const auto dpiImport = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("v_difftest_MultiValue"));
    graph.setAttr(dpiImport, "argsDirection", std::vector<std::string>{"input", "output"});
    graph.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{1, 8});
    graph.setAttr(dpiImport, "argsName", std::vector<std::string>{"in", "out"});
    graph.setAttr(dpiImport, "argsSigned", std::vector<bool>{false, false});
    graph.setAttr(dpiImport, "argsType", std::vector<std::string>{"bit", "byte"});
    graph.setAttr(dpiImport, "hasReturn", true);
    graph.setAttr(dpiImport, "returnType", std::string("byte"));

    const auto retValue = makeValue(graph, "multi_ret", 8, false);
    const auto outValue = makeValue(graph, "multi_out", 8, false);
    const auto dpiCall = graph.createOperation(OperationKind::kDpicCall, graph.internSymbol("call_multi_value"));
    graph.addOperand(dpiCall, one);
    graph.addOperand(dpiCall, one);
    graph.addOperand(dpiCall, clk);
    graph.addResult(dpiCall, retValue);
    graph.addResult(dpiCall, outValue);
    graph.setAttr(dpiCall, "targetImportSymbol", std::string("v_difftest_MultiValue"));
    graph.setAttr(dpiCall, "inArgName", std::vector<std::string>{"in"});
    graph.setAttr(dpiCall, "outArgName", std::vector<std::string>{"out"});
    graph.setAttr(dpiCall, "hasReturn", true);
    graph.setAttr(dpiCall, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(dpiCall, "clkPolarity", std::string("posedge"));
    graph.bindOutputPort("ret", retValue);
    graph.bindOutputPort("out", outValue);

    for (int i = 0; i < 140; ++i)
    {
        (void)makeConstant(graph, "multi_difftest_padding_const_" + std::to_string(i),
                           "multi_difftest_padding_const_op_" + std::to_string(i), 1, "1'b0");
    }

    return design;
}

Design buildDpicReturnReadDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto addr = makeValue(graph, "addr", 64, false);
    const auto clk = makeValue(graph, "clk", 1, false);
    graph.bindInputPort("addr", addr);
    graph.bindInputPort("clk", clk);

    (void)makeRegister(graph, "data_storage", "data_reg", 64, "data");
    const auto dataRead = makeRegisterRead(graph, "data_read", "data_read_op", 64, "data");
    graph.bindOutputPort("data", dataRead);

    const auto dpiImport = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("difftest_ram_read"));
    graph.setAttr(dpiImport, "argsDirection", std::vector<std::string>{"input"});
    graph.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{64});
    graph.setAttr(dpiImport, "argsName", std::vector<std::string>{"rIdx"});
    graph.setAttr(dpiImport, "argsSigned", std::vector<bool>{false});
    graph.setAttr(dpiImport, "argsType", std::vector<std::string>{"longint"});
    graph.setAttr(dpiImport, "hasReturn", true);
    graph.setAttr(dpiImport, "returnWidth", static_cast<int64_t>(64));
    graph.setAttr(dpiImport, "returnSigned", false);
    graph.setAttr(dpiImport, "returnType", std::string("longint"));

    const auto one = makeConstant(graph, "one", "one_const", 1, "1'b1");
    const auto readResult = makeValue(graph, "read_result", 64, false);
    const auto dpiCall = graph.createOperation(OperationKind::kDpicCall, graph.internSymbol("call_ram_read"));
    graph.addOperand(dpiCall, one);
    graph.addOperand(dpiCall, addr);
    graph.addOperand(dpiCall, clk);
    graph.addResult(dpiCall, readResult);
    graph.setAttr(dpiCall, "targetImportSymbol", std::string("difftest_ram_read"));
    graph.setAttr(dpiCall, "inArgName", std::vector<std::string>{"rIdx"});
    graph.setAttr(dpiCall, "outArgName", std::vector<std::string>{});
    graph.setAttr(dpiCall, "hasReturn", true);
    graph.setAttr(dpiCall, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(dpiCall, "clkPolarity", std::string("posedge"));

    const auto mask = makeConstant(graph, "mask", "mask_const", 64, "64'hffffffffffffffff");
    makeRegisterWrite(graph, "data_write", one, readResult, mask, clk, "data");

    return design;
}

Design buildDpicOutputReadDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto addr = makeValue(graph, "addr", 32, false);
    const auto clk = makeValue(graph, "clk", 1, false);
    graph.bindInputPort("addr", addr);
    graph.bindInputPort("clk", clk);

    (void)makeRegister(graph, "data_storage", "data_reg", 64, "data");
    const auto dataRead = makeRegisterRead(graph, "data_read", "data_read_op", 64, "data");
    graph.bindOutputPort("data", dataRead);

    const auto dpiImport = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("flash_read"));
    graph.setAttr(dpiImport, "argsDirection", std::vector<std::string>{"input", "output"});
    graph.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{32, 64});
    graph.setAttr(dpiImport, "argsName", std::vector<std::string>{"addr", "data"});
    graph.setAttr(dpiImport, "argsSigned", std::vector<bool>{false, false});
    graph.setAttr(dpiImport, "argsType", std::vector<std::string>{"int", "longint"});
    graph.setAttr(dpiImport, "hasReturn", false);
    graph.setAttr(dpiImport, "returnType", std::string("void"));

    const auto one = makeConstant(graph, "one", "one_const", 1, "1'b1");
    const auto readResult = makeValue(graph, "read_result", 64, false);
    const auto dpiCall = graph.createOperation(OperationKind::kDpicCall, graph.internSymbol("call_flash_read"));
    graph.addOperand(dpiCall, one);
    graph.addOperand(dpiCall, addr);
    graph.addOperand(dpiCall, clk);
    graph.addResult(dpiCall, readResult);
    graph.setAttr(dpiCall, "targetImportSymbol", std::string("flash_read"));
    graph.setAttr(dpiCall, "inArgName", std::vector<std::string>{"addr"});
    graph.setAttr(dpiCall, "outArgName", std::vector<std::string>{"data"});
    graph.setAttr(dpiCall, "hasReturn", false);
    graph.setAttr(dpiCall, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(dpiCall, "clkPolarity", std::string("posedge"));

    const auto mask = makeConstant(graph, "mask", "mask_const", 64, "64'hffffffffffffffff");
    makeRegisterWrite(graph, "data_write", one, readResult, mask, clk, "data");

    return design;
}

Design buildDpicJtagTickMultiResultDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto clk = makeValue(graph, "clk", 1, false);
    const auto en = makeValue(graph, "en", 1, false);
    const auto tickIn = makeValue(graph, "tick_in", 1, false);
    graph.bindInputPort("clk", clk);
    graph.bindInputPort("en", en);
    graph.bindInputPort("tick_in", tickIn);

    (void)makeRegister(graph, "result_storage", "result_reg", 32, "result");
    (void)makeRegister(graph, "tck_storage", "tck_reg", 1, "tck");
    (void)makeRegister(graph, "tms_storage", "tms_reg", 1, "tms");
    (void)makeRegister(graph, "tdi_storage", "tdi_reg", 1, "tdi");
    (void)makeRegister(graph, "trstn_storage", "trstn_reg", 1, "trstn");

    graph.bindOutputPort("result", makeRegisterRead(graph, "result_read", "result_read_op", 32, "result"));
    graph.bindOutputPort("tck", makeRegisterRead(graph, "tck_read", "tck_read_op", 1, "tck"));
    graph.bindOutputPort("tms", makeRegisterRead(graph, "tms_read", "tms_read_op", 1, "tms"));
    graph.bindOutputPort("tdi", makeRegisterRead(graph, "tdi_read", "tdi_read_op", 1, "tdi"));
    graph.bindOutputPort("trstn", makeRegisterRead(graph, "trstn_read", "trstn_read_op", 1, "trstn"));

    const auto dpiImport = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("jtag_tick"));
    graph.setAttr(dpiImport, "argsDirection",
                  std::vector<std::string>{"output", "output", "output", "output", "input"});
    graph.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{1, 1, 1, 1, 1});
    graph.setAttr(dpiImport, "argsName", std::vector<std::string>{"tck", "tms", "tdi", "trstn", "tdo"});
    graph.setAttr(dpiImport, "argsSigned", std::vector<bool>{false, false, false, false, false});
    graph.setAttr(dpiImport, "argsType", std::vector<std::string>{"bit", "bit", "bit", "bit", "bit"});
    graph.setAttr(dpiImport, "hasReturn", true);
    graph.setAttr(dpiImport, "returnWidth", static_cast<int64_t>(32));
    graph.setAttr(dpiImport, "returnSigned", true);
    graph.setAttr(dpiImport, "returnType", std::string("int"));

    const auto ret = makeValue(graph, "jtag_ret", 32, false);
    const auto tck = makeValue(graph, "jtag_tck", 1, false);
    const auto tms = makeValue(graph, "jtag_tms", 1, false);
    const auto tdi = makeValue(graph, "jtag_tdi", 1, false);
    const auto trstn = makeValue(graph, "jtag_trstn", 1, false);

    const auto dpiCall = graph.createOperation(OperationKind::kDpicCall, graph.internSymbol("call_jtag_tick"));
    graph.addOperand(dpiCall, en);
    graph.addOperand(dpiCall, tickIn);
    graph.addOperand(dpiCall, clk);
    graph.addResult(dpiCall, ret);
    graph.addResult(dpiCall, tck);
    graph.addResult(dpiCall, tms);
    graph.addResult(dpiCall, tdi);
    graph.addResult(dpiCall, trstn);
    graph.setAttr(dpiCall, "targetImportSymbol", std::string("jtag_tick"));
    graph.setAttr(dpiCall, "inArgName", std::vector<std::string>{"tdo"});
    graph.setAttr(dpiCall, "outArgName", std::vector<std::string>{"tck", "tms", "tdi", "trstn"});
    graph.setAttr(dpiCall, "hasReturn", true);
    graph.setAttr(dpiCall, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(dpiCall, "clkPolarity", std::string("posedge"));

    const auto mask32 = makeConstant(graph, "mask32", "mask32_const", 32, "32'hffffffff");
    const auto mask1 = makeConstant(graph, "mask1", "mask1_const", 1, "1'b1");
    makeRegisterWrite(graph, "result_write", en, ret, mask32, clk, "result");
    makeRegisterWrite(graph, "tck_write", en, tck, mask1, clk, "tck");
    makeRegisterWrite(graph, "tms_write", en, tms, mask1, clk, "tms");
    makeRegisterWrite(graph, "tdi_write", en, tdi, mask1, clk, "tdi");
    makeRegisterWrite(graph, "trstn_write", en, trstn, mask1, clk, "trstn");

    return design;
}

Design buildDpicJtagTickPreOnlyOutputDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto clk = makeValue(graph, "clk", 1, false);
    const auto en = makeValue(graph, "en", 1, false);
    const auto tickIn = makeValue(graph, "tick_in", 1, false);
    graph.bindInputPort("clk", clk);
    graph.bindInputPort("en", en);
    graph.bindInputPort("tick_in", tickIn);

    const auto dpiImport = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("jtag_tick"));
    graph.setAttr(dpiImport, "argsDirection",
                  std::vector<std::string>{"output", "output", "output", "output", "input"});
    graph.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{1, 1, 1, 1, 1});
    graph.setAttr(dpiImport, "argsName", std::vector<std::string>{"tck", "tms", "tdi", "trstn", "tdo"});
    graph.setAttr(dpiImport, "argsSigned", std::vector<bool>{false, false, false, false, false});
    graph.setAttr(dpiImport, "argsType", std::vector<std::string>{"bit", "bit", "bit", "bit", "bit"});
    graph.setAttr(dpiImport, "hasReturn", true);
    graph.setAttr(dpiImport, "returnWidth", static_cast<int64_t>(32));
    graph.setAttr(dpiImport, "returnSigned", true);
    graph.setAttr(dpiImport, "returnType", std::string("int"));

    const auto ret = makeValue(graph, "jtag_ret", 32, false);
    const auto tck = makeValue(graph, "jtag_tck", 1, false);
    const auto tms = makeValue(graph, "jtag_tms", 1, false);
    const auto tdi = makeValue(graph, "jtag_tdi", 1, false);
    const auto trstn = makeValue(graph, "jtag_trstn", 1, false);

    const auto dpiCall = graph.createOperation(OperationKind::kDpicCall, graph.internSymbol("call_jtag_tick"));
    graph.addOperand(dpiCall, en);
    graph.addOperand(dpiCall, tickIn);
    graph.addOperand(dpiCall, clk);
    graph.addResult(dpiCall, ret);
    graph.addResult(dpiCall, tck);
    graph.addResult(dpiCall, tms);
    graph.addResult(dpiCall, tdi);
    graph.addResult(dpiCall, trstn);
    graph.setAttr(dpiCall, "targetImportSymbol", std::string("jtag_tick"));
    graph.setAttr(dpiCall, "inArgName", std::vector<std::string>{"tdo"});
    graph.setAttr(dpiCall, "outArgName", std::vector<std::string>{"tck", "tms", "tdi", "trstn"});
    graph.setAttr(dpiCall, "hasReturn", true);
    graph.setAttr(dpiCall, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(dpiCall, "clkPolarity", std::string("posedge"));

    graph.bindOutputPort("result", ret);
    graph.bindOutputPort("tck", tck);
    graph.bindOutputPort("tms", tms);
    graph.bindOutputPort("tdi", tdi);
    graph.bindOutputPort("trstn", trstn);

    return design;
}

Design buildDpicPostSequentialSettleDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto clk = makeValue(graph, "clk", 1, false);
    graph.bindInputPort("clk", clk);

    (void)makeRegister(graph, "pc_storage", "pc_reg", 8, "pc");
    const auto pcRead = makeRegisterRead(graph, "pc_read", "pc_read_op", 8, "pc");

    auto pcForDpi = pcRead;
    for (int i = 0; i < 160; ++i) {
        const auto nextPcValue = makeValue(graph, "pc_chain_" + std::to_string(i), 8, false);
        const auto assign = graph.createOperation(
            OperationKind::kAssign, graph.internSymbol("pc_chain_assign_" + std::to_string(i)));
        graph.addOperand(assign, pcForDpi);
        graph.addResult(assign, nextPcValue);
        pcForDpi = nextPcValue;
    }
    graph.bindOutputPort("pc", pcForDpi);

    const auto one = makeConstant(graph, "one", "one_const", 1, "1'b1");
    const auto nextPc = makeConstant(graph, "next_pc", "next_pc_const", 8, "8'h2a");
    const auto mask = makeConstant(graph, "mask", "mask_const", 8, "8'hff");
    makeRegisterWrite(graph, "pc_write", one, nextPc, mask, clk, "pc");

    const auto dpiImport = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("dpi_capture"));
    graph.setAttr(dpiImport, "argsDirection", std::vector<std::string>{"input"});
    graph.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{8});
    graph.setAttr(dpiImport, "argsName", std::vector<std::string>{"value"});
    graph.setAttr(dpiImport, "argsSigned", std::vector<bool>{false});
    graph.setAttr(dpiImport, "argsType", std::vector<std::string>{"byte"});
    graph.setAttr(dpiImport, "hasReturn", false);
    graph.setAttr(dpiImport, "returnType", std::string("void"));

    const auto dpiCall = graph.createOperation(OperationKind::kDpicCall, graph.internSymbol("call_capture_pc"));
    graph.addOperand(dpiCall, one);
    graph.addOperand(dpiCall, pcForDpi);
    graph.addOperand(dpiCall, clk);
    graph.setAttr(dpiCall, "targetImportSymbol", std::string("dpi_capture"));
    graph.setAttr(dpiCall, "inArgName", std::vector<std::string>{"value"});
    graph.setAttr(dpiCall, "outArgName", std::vector<std::string>{});
    graph.setAttr(dpiCall, "hasReturn", false);
    graph.setAttr(dpiCall, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(dpiCall, "clkPolarity", std::string("posedge"));

    return design;
}


Design buildDerivedClockPostCommitReplayDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto clk = makeValue(graph, "clk", 1, false);
    graph.bindInputPort("clk", clk);

    (void)makeRegister(graph, "gate_storage", "gate_reg", 1, "gate");
    const auto gateRead = makeRegisterRead(graph, "gate_read", "gate_read_op", 1, "gate");
    (void)makeRegister(graph, "data_storage", "data_reg", 8, "data");
    const auto dataRead = makeRegisterRead(graph, "data_read", "data_read_op", 8, "data");
    graph.bindOutputPort("data", dataRead);

    const auto one = makeConstant(graph, "one", "one_const", 1, "1'b1");
    const auto mask1 = makeConstant(graph, "mask1", "mask1_const", 1, "1'b1");
    const auto dataValue = makeConstant(graph, "data_value", "data_value_const", 8, "8'h5a");
    const auto mask8 = makeConstant(graph, "mask8", "mask8_const", 8, "8'hff");

    makeRegisterWrite(graph, "gate_write", one, one, mask1, clk, "gate");

    const auto gatedClk = makeValue(graph, "gated_clk", 1, false);
    const auto andOp = graph.createOperation(OperationKind::kLogicAnd, graph.internSymbol("gated_clk_and"));
    graph.addOperand(andOp, clk);
    graph.addOperand(andOp, gateRead);
    graph.addResult(andOp, gatedClk);

    const auto dataWrite = makeRegisterWrite(graph, "data_write", one, dataValue, mask8, gatedClk, "data");
    graph.setAttr(dataWrite, "clockSymbol", std::string("gated_clk"));

    // Keep this fixture above the sharding threshold so it covers the
    // activity-watermark path used by XiangShan-scale GSIM emission.
    for (int i = 0; i < 140; ++i) {
        (void)makeConstant(graph, "padding_const_" + std::to_string(i),
                           "padding_const_op_" + std::to_string(i), 1, "1'b0");
    }

    return design;
}

Design buildSettledDerivedInputClockDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto clk = makeValue(graph, "clk", 1, false);
    const auto en = makeValue(graph, "en", 1, false);
    graph.bindInputPort("clk", clk);
    graph.bindInputPort("en", en);

    (void)makeRegister(graph, "data_storage", "data_reg", 8, "data");
    const auto dataRead = makeRegisterRead(graph, "data_read", "data_read_op", 8, "data");
    graph.bindOutputPort("data", dataRead);

    const auto gatedClk = makeValue(graph, "gated_clk", 1, false);
    const auto andOp = graph.createOperation(OperationKind::kLogicAnd, graph.internSymbol("input_gated_clk_and"));
    graph.addOperand(andOp, clk);
    graph.addOperand(andOp, en);
    graph.addResult(andOp, gatedClk);

    const auto one = makeConstant(graph, "one", "one_const", 1, "1'b1");
    const auto dataValue = makeConstant(graph, "data_value", "data_value_const", 8, "8'h5a");
    const auto mask8 = makeConstant(graph, "mask8", "mask8_const", 8, "8'hff");
    const auto dataWrite = makeRegisterWrite(graph, "data_write", one, dataValue, mask8, gatedClk, "data");
    graph.setAttr(dataWrite, "clockSymbol", std::string("gated_clk"));

    for (int i = 0; i < 140; ++i) {
        (void)makeConstant(graph, "settled_clock_padding_const_" + std::to_string(i),
                           "settled_clock_padding_const_op_" + std::to_string(i), 1, "1'b0");
    }

    return design;
}

Design buildLatchReadNoOpDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    (void)makeLatch(graph, "state_latch_decl", 4, "state_latch");
    const auto latchRead = makeLatchRead(graph, "state_latch_q", "state_latch_read", 4, "state_latch");
    graph.bindOutputPort("y", latchRead);

    return design;
}

Design buildMemoryReadDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto addr = makeValue(graph, "addr", 3, false);
    graph.bindInputPort("addr", addr);

    (void)makeMemory(graph, 8, 4, "mem0");
    const auto data = makeMemoryRead(graph, "data", "mem0_read", 8, addr, "mem0");
    graph.bindOutputPort("data", data);

    return design;
}

Design buildMemoryWriteDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto clk = makeValue(graph, "clk", 1, false);
    const auto wen = makeValue(graph, "wen", 1, false);
    const auto addr = makeValue(graph, "addr", 2, false);
    const auto data = makeValue(graph, "data", 8, false);
    graph.bindInputPort("clk", clk);
    graph.bindInputPort("wen", wen);
    graph.bindInputPort("addr", addr);
    graph.bindInputPort("data", data);

    (void)makeMemory(graph, 8, 4, "mem0");
    const auto read = makeMemoryRead(graph, "read_data", "read_data_op", 8, addr, "mem0");
    graph.bindOutputPort("q", read);

    const auto mask = makeConstant(graph, "mask", "mask_const", 8, "8'hff");
    makeMemoryWrite(graph, "write_port", wen, addr, data, mask, clk, "mem0");

    return design;
}

Design buildLatchWriteDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto en = makeValue(graph, "en", 1, false);
    const auto d = makeValue(graph, "d", 4, false);
    graph.bindInputPort("en", en);
    graph.bindInputPort("d", d);

    (void)makeLatch(graph, "state_latch_decl", 4, "state_latch");
    const auto latchRead = makeLatchRead(graph, "state_latch_q", "state_latch_read", 4, "state_latch");
    graph.bindOutputPort("y", latchRead);

    const auto mask = makeConstant(graph, "mask", "mask_const", 4, "4'hF");
    const auto latchWrite = graph.createOperation(OperationKind::kLatchWritePort, graph.internSymbol("state_latch_write"));
    graph.addOperand(latchWrite, en);
    graph.addOperand(latchWrite, d);
    graph.addOperand(latchWrite, mask);
    graph.setAttr(latchWrite, "latchSymbol", std::string("state_latch"));

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

Design buildLogicBinaryDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto inA = makeValue(graph, "a", 1, false);
    const auto inB = makeValue(graph, "b", 1, false);
    graph.bindInputPort("a", inA);
    graph.bindInputPort("b", inB);

    const auto outAnd = makeValue(graph, "and_y", 1, false);
    const auto outOr = makeValue(graph, "or_y", 1, false);
    graph.bindOutputPort("and_y", outAnd);
    graph.bindOutputPort("or_y", outOr);

    const auto andOp = graph.createOperation(OperationKind::kLogicAnd, graph.internSymbol("logic_and_y"));
    graph.addOperand(andOp, inA);
    graph.addOperand(andOp, inB);
    graph.addResult(andOp, outAnd);

    const auto orOp = graph.createOperation(OperationKind::kLogicOr, graph.internSymbol("logic_or_y"));
    graph.addOperand(orOp, inA);
    graph.addOperand(orOp, inB);
    graph.addResult(orOp, outOr);

    return design;
}

Design buildCaseEqDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto inA = makeValue(graph, "a", 8, false);
    const auto inB = makeValue(graph, "b", 8, false);
    graph.bindInputPort("a", inA);
    graph.bindInputPort("b", inB);

    const auto outEq = makeValue(graph, "eq_y", 1, false);
    const auto outNe = makeValue(graph, "ne_y", 1, false);
    graph.bindOutputPort("eq_y", outEq);
    graph.bindOutputPort("ne_y", outNe);

    const auto eqOp = graph.createOperation(OperationKind::kCaseEq, graph.internSymbol("case_eq_y"));
    graph.addOperand(eqOp, inA);
    graph.addOperand(eqOp, inB);
    graph.addResult(eqOp, outEq);

    const auto neOp = graph.createOperation(OperationKind::kCaseNe, graph.internSymbol("case_ne_y"));
    graph.addOperand(neOp, inA);
    graph.addOperand(neOp, inB);
    graph.addResult(neOp, outNe);

    return design;
}

Design buildCompareDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto inA = makeValue(graph, "a", 8, false);
    const auto inB = makeValue(graph, "b", 8, false);
    graph.bindInputPort("a", inA);
    graph.bindInputPort("b", inB);

    const auto outLt = makeValue(graph, "lt_y", 1, false);
    const auto outLe = makeValue(graph, "le_y", 1, false);
    const auto outGt = makeValue(graph, "gt_y", 1, false);
    const auto outGe = makeValue(graph, "ge_y", 1, false);
    const auto outNe = makeValue(graph, "ne_y", 1, false);
    graph.bindOutputPort("lt_y", outLt);
    graph.bindOutputPort("le_y", outLe);
    graph.bindOutputPort("gt_y", outGt);
    graph.bindOutputPort("ge_y", outGe);
    graph.bindOutputPort("ne_y", outNe);

    const auto ltOp = graph.createOperation(OperationKind::kLt, graph.internSymbol("lt_y_op"));
    graph.addOperand(ltOp, inA);
    graph.addOperand(ltOp, inB);
    graph.addResult(ltOp, outLt);

    const auto gtOp = graph.createOperation(OperationKind::kGt, graph.internSymbol("gt_y_op"));
    graph.addOperand(gtOp, inA);
    graph.addOperand(gtOp, inB);
    graph.addResult(gtOp, outGt);

    const auto leOp = graph.createOperation(OperationKind::kLe, graph.internSymbol("le_y_op"));
    graph.addOperand(leOp, inA);
    graph.addOperand(leOp, inB);
    graph.addResult(leOp, outLe);

    const auto geOp = graph.createOperation(OperationKind::kGe, graph.internSymbol("ge_y_op"));
    graph.addOperand(geOp, inA);
    graph.addOperand(geOp, inB);
    graph.addResult(geOp, outGe);

    const auto neOp = graph.createOperation(OperationKind::kNe, graph.internSymbol("ne_y_op"));
    graph.addOperand(neOp, inA);
    graph.addOperand(neOp, inB);
    graph.addResult(neOp, outNe);

    return design;
}

Design buildXnorDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto inA = makeValue(graph, "a", 8, false);
    const auto inB = makeValue(graph, "b", 8, false);
    graph.bindInputPort("a", inA);
    graph.bindInputPort("b", inB);

    const auto outY = makeValue(graph, "y", 8, false);
    graph.bindOutputPort("y", outY);

    const auto xnorOp = graph.createOperation(OperationKind::kXnor, graph.internSymbol("xnor_y_op"));
    graph.addOperand(xnorOp, inA);
    graph.addOperand(xnorOp, inB);
    graph.addResult(xnorOp, outY);

    return design;
}

Design buildSliceStaticDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto inA = makeValue(graph, "a", 8, false);
    graph.bindInputPort("a", inA);

    const auto outY = makeValue(graph, "y", 4, false);
    graph.bindOutputPort("y", outY);

    const auto slice = graph.createOperation(OperationKind::kSliceStatic, graph.internSymbol("slice_hi_nibble"));
    graph.addOperand(slice, inA);
    graph.addResult(slice, outY);
    graph.setAttr(slice, "sliceStart", static_cast<int64_t>(4));
    graph.setAttr(slice, "sliceEnd", static_cast<int64_t>(7));

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

Design buildWideBitSliceDynamicDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto in = makeValue(graph, "in", 130, false);
    const auto index = makeValue(graph, "index", 8, false);
    graph.bindInputPort("in", in);
    graph.bindInputPort("index", index);

    const auto out = makeValue(graph, "out", 1, false);
    graph.bindOutputPort("out", out);

    const auto slice = graph.createOperation(OperationKind::kSliceDynamic, graph.internSymbol("wide_bit_slice"));
    graph.addOperand(slice, in);
    graph.addOperand(slice, index);
    graph.addResult(slice, out);
    graph.setAttr(slice, "sliceWidth", static_cast<int64_t>(1));

    return design;
}

Design buildWideNibbleSliceDynamicDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto in = makeValue(graph, "in", 130, false);
    const auto index = makeValue(graph, "index", 8, false);
    graph.bindInputPort("in", in);
    graph.bindInputPort("index", index);

    const auto out = makeValue(graph, "out", 4, false);
    graph.bindOutputPort("out", out);

    const auto slice = graph.createOperation(OperationKind::kSliceDynamic, graph.internSymbol("wide_nibble_slice"));
    graph.addOperand(slice, in);
    graph.addOperand(slice, index);
    graph.addResult(slice, out);
    graph.setAttr(slice, "sliceWidth", static_cast<int64_t>(4));

    return design;
}

Design buildWideVectorSliceDynamicDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto in = makeValue(graph, "in", 130, false);
    const auto index = makeValue(graph, "index", 8, false);
    graph.bindInputPort("in", in);
    graph.bindInputPort("index", index);

    const auto out = makeValue(graph, "out", 70, false);
    graph.bindOutputPort("out", out);

    const auto slice = graph.createOperation(OperationKind::kSliceDynamic, graph.internSymbol("wide_vector_slice"));
    graph.addOperand(slice, in);
    graph.addOperand(slice, index);
    graph.addResult(slice, out);
    graph.setAttr(slice, "sliceWidth", static_cast<int64_t>(70));

    return design;
}

Design buildWideStaticSliceDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto in = makeValue(graph, "in", 130, false);
    graph.bindInputPort("in", in);

    const auto out = makeValue(graph, "out", 70, false);
    graph.bindOutputPort("out", out);

    const auto slice = graph.createOperation(OperationKind::kSliceStatic, graph.internSymbol("wide_static_slice"));
    graph.addOperand(slice, in);
    graph.addResult(slice, out);
    graph.setAttr(slice, "sliceStart", static_cast<int64_t>(5));
    graph.setAttr(slice, "sliceEnd", static_cast<int64_t>(74));

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

Design buildClockAliasWithoutMaterializedExprDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto clk = makeValue(graph, "clk", 1, false);
    const auto d = makeValue(graph, "d", 1, false);
    graph.bindInputPort("clk", clk);
    graph.bindInputPort("d", d);

    (void)makeRegister(graph, "state_storage", "state_reg", 1, "state");
    const auto stateRead = makeRegisterRead(graph, "state_read", "state_read_op", 1, "state");
    graph.bindOutputPort("q", stateRead);

    const auto one = makeConstant(graph, "one", "one_const", 1, "1'b1");
    const auto mask = makeConstant(graph, "mask", "mask_const", 1, "1'b1");
    const auto missingClockCarrier = makeValue(graph, "clock_alias_wire", 1, false);

    const auto write = graph.createOperation(OperationKind::kRegisterWritePort, graph.internSymbol("state_write"));
    graph.addOperand(write, one);
    graph.addOperand(write, d);
    graph.addOperand(write, mask);
    graph.addOperand(write, missingClockCarrier);
    graph.setAttr(write, "regSymbol", std::string("state"));
    graph.setAttr(write, "clockSymbol", std::string("clock_alias"));
    graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});

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

Design buildWideConcatDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto msb = makeValue(graph, "msb", 1, false);
    const auto rest = makeValue(graph, "rest", 100, false);
    graph.bindInputPort("msb", msb);
    graph.bindInputPort("rest", rest);

    const auto out = makeValue(graph, "y", 101, false);
    graph.bindOutputPort("y", out);

    const auto concat = graph.createOperation(OperationKind::kConcat, graph.internSymbol("wide_concat_y"));
    graph.addOperand(concat, msb);
    graph.addOperand(concat, rest);
    graph.addResult(concat, out);

    return design;
}

Design buildWideBitwiseDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto a = makeValue(graph, "a", 100, false);
    const auto b = makeValue(graph, "b", 100, false);
    graph.bindInputPort("a", a);
    graph.bindInputPort("b", b);

    const auto outAnd = makeValue(graph, "and_y", 100, false);
    const auto outOr = makeValue(graph, "or_y", 100, false);
    const auto outXor = makeValue(graph, "xor_y", 100, false);
    const auto outNot = makeValue(graph, "not_y", 100, false);
    graph.bindOutputPort("and_y", outAnd);
    graph.bindOutputPort("or_y", outOr);
    graph.bindOutputPort("xor_y", outXor);
    graph.bindOutputPort("not_y", outNot);

    const auto andOp = graph.createOperation(OperationKind::kAnd, graph.internSymbol("wide_and_y"));
    graph.addOperand(andOp, a);
    graph.addOperand(andOp, b);
    graph.addResult(andOp, outAnd);

    const auto orOp = graph.createOperation(OperationKind::kOr, graph.internSymbol("wide_or_y"));
    graph.addOperand(orOp, a);
    graph.addOperand(orOp, b);
    graph.addResult(orOp, outOr);

    const auto xorOp = graph.createOperation(OperationKind::kXor, graph.internSymbol("wide_xor_y"));
    graph.addOperand(xorOp, a);
    graph.addOperand(xorOp, b);
    graph.addResult(xorOp, outXor);

    const auto notOp = graph.createOperation(OperationKind::kNot, graph.internSymbol("wide_not_y"));
    graph.addOperand(notOp, a);
    graph.addResult(notOp, outNot);

    // Keep this fixture above the sharding threshold so it covers the XiangShan
    // hot path that lowers wide bitwise operations directly into materialized
    // temp storage instead of returning heap-backed vectors by value.
    ValueId prev = a;
    for (int i = 0; i < 130; ++i)
    {
        const auto filler = makeValue(graph, "wide_bitwise_filler_" + std::to_string(i), 100, false);
        const auto fillerOp = graph.createOperation(OperationKind::kXor,
                                                    graph.internSymbol("wide_bitwise_filler_op_" + std::to_string(i)));
        graph.addOperand(fillerOp, prev);
        graph.addOperand(fillerOp, b);
        graph.addResult(fillerOp, filler);
        prev = filler;
    }

    return design;
}

Design buildWideAdderSplitDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto a = makeValue(graph, "a", 100, false);
    const auto b = makeValue(graph, "b", 100, false);
    const auto cin = makeValue(graph, "cin", 1, false);
    graph.bindInputPort("a", a);
    graph.bindInputPort("b", b);
    graph.bindInputPort("cin", cin);

    const auto aExt = makeValue(graph, "a_ext", 101, false);
    const auto bExt = makeValue(graph, "b_ext", 101, false);
    const auto cinExt = makeValue(graph, "cin_ext", 101, false);
    const auto sumWide = makeValue(graph, "sum_wide", 101, false);
    const auto sum = makeValue(graph, "sum", 100, false);
    const auto cout = makeValue(graph, "cout", 1, false);
    graph.bindOutputPort("sum", sum);
    graph.bindOutputPort("cout", cout);

    const auto aConcat = graph.createOperation(OperationKind::kConcat, graph.internSymbol("a_ext_op"));
    const auto zeroA = makeConstant(graph, "zero_a", "zero_a_const", 1, "1'b0");
    graph.addOperand(aConcat, zeroA);
    graph.addOperand(aConcat, a);
    graph.addResult(aConcat, aExt);

    const auto bConcat = graph.createOperation(OperationKind::kConcat, graph.internSymbol("b_ext_op"));
    const auto zeroB = makeConstant(graph, "zero_b", "zero_b_const", 1, "1'b0");
    graph.addOperand(bConcat, zeroB);
    graph.addOperand(bConcat, b);
    graph.addResult(bConcat, bExt);

    const auto cinConcat = graph.createOperation(OperationKind::kConcat, graph.internSymbol("cin_ext_op"));
    const auto zeroCin = makeConstant(graph, "zero_cin", "zero_cin_const", 100, "100'd0");
    graph.addOperand(cinConcat, zeroCin);
    graph.addOperand(cinConcat, cin);
    graph.addResult(cinConcat, cinExt);

    const auto add0 = graph.createOperation(OperationKind::kAdd, graph.internSymbol("sum_ab_op"));
    graph.addOperand(add0, aExt);
    graph.addOperand(add0, bExt);
    graph.addResult(add0, sumWide);

    const auto sumWithCin = makeValue(graph, "sum_with_cin", 101, false);
    const auto add1 = graph.createOperation(OperationKind::kAdd, graph.internSymbol("sum_with_cin_op"));
    graph.addOperand(add1, sumWide);
    graph.addOperand(add1, cinExt);
    graph.addResult(add1, sumWithCin);

    const auto sliceSum = graph.createOperation(OperationKind::kSliceStatic, graph.internSymbol("sum_slice"));
    graph.addOperand(sliceSum, sumWithCin);
    graph.addResult(sliceSum, sum);
    graph.setAttr(sliceSum, "sliceStart", static_cast<int64_t>(0));
    graph.setAttr(sliceSum, "sliceEnd", static_cast<int64_t>(99));

    const auto sliceCout = graph.createOperation(OperationKind::kSliceStatic, graph.internSymbol("cout_slice"));
    graph.addOperand(sliceCout, sumWithCin);
    graph.addResult(sliceCout, cout);
    graph.setAttr(sliceCout, "sliceStart", static_cast<int64_t>(100));
    graph.setAttr(sliceCout, "sliceEnd", static_cast<int64_t>(100));

    return design;
}

Design buildReplicateDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto in = makeValue(graph, "in", 5, false);
    graph.bindInputPort("in", in);

    const auto out = makeValue(graph, "y", 25, false);
    graph.bindOutputPort("y", out);

    const auto rep = graph.createOperation(OperationKind::kReplicate, graph.internSymbol("replicate_y"));
    graph.addOperand(rep, in);
    graph.addResult(rep, out);
    graph.setAttr(rep, "rep", static_cast<int64_t>(5));

    return design;
}

Design buildWideReplicateDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto in = makeValue(graph, "in", 70, false);
    graph.bindInputPort("in", in);

    const auto out = makeValue(graph, "y", 140, false);
    graph.bindOutputPort("y", out);

    const auto rep = graph.createOperation(OperationKind::kReplicate, graph.internSymbol("wide_replicate_y"));
    graph.addOperand(rep, in);
    graph.addResult(rep, out);
    graph.setAttr(rep, "rep", static_cast<int64_t>(2));

    return design;
}

Design buildReduceDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto in = makeValue(graph, "in", 8, false);
    graph.bindInputPort("in", in);

    const auto outAnd = makeValue(graph, "and_y", 1, false);
    const auto outOr = makeValue(graph, "or_y", 1, false);
    const auto outXor = makeValue(graph, "xor_y", 1, false);
    graph.bindOutputPort("and_y", outAnd);
    graph.bindOutputPort("or_y", outOr);
    graph.bindOutputPort("xor_y", outXor);

    const auto andOp = graph.createOperation(OperationKind::kReduceAnd, graph.internSymbol("reduce_and_y"));
    graph.addOperand(andOp, in);
    graph.addResult(andOp, outAnd);

    const auto orOp = graph.createOperation(OperationKind::kReduceOr, graph.internSymbol("reduce_or_y"));
    graph.addOperand(orOp, in);
    graph.addResult(orOp, outOr);

    const auto xorOp = graph.createOperation(OperationKind::kReduceXor, graph.internSymbol("reduce_xor_y"));
    graph.addOperand(xorOp, in);
    graph.addResult(xorOp, outXor);

    return design;
}

Design buildWideReduceDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto in = makeValue(graph, "in", 130, false);
    graph.bindInputPort("in", in);

    const auto outAnd = makeValue(graph, "and_y", 1, false);
    const auto outOr = makeValue(graph, "or_y", 1, false);
    const auto outXor = makeValue(graph, "xor_y", 1, false);
    graph.bindOutputPort("and_y", outAnd);
    graph.bindOutputPort("or_y", outOr);
    graph.bindOutputPort("xor_y", outXor);

    const auto andOp = graph.createOperation(OperationKind::kReduceAnd, graph.internSymbol("wide_reduce_and_y"));
    graph.addOperand(andOp, in);
    graph.addResult(andOp, outAnd);

    const auto orOp = graph.createOperation(OperationKind::kReduceOr, graph.internSymbol("wide_reduce_or_y"));
    graph.addOperand(orOp, in);
    graph.addResult(orOp, outOr);

    const auto xorOp = graph.createOperation(OperationKind::kReduceXor, graph.internSymbol("wide_reduce_xor_y"));
    graph.addOperand(xorOp, in);
    graph.addResult(xorOp, outXor);

    return design;
}

Design buildWideMaskedRegisterDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto clk = makeValue(graph, "clk", 1, false);
    const auto d = makeValue(graph, "d", 130, false);
    const auto mask = makeValue(graph, "mask", 130, false);
    graph.bindInputPort("clk", clk);
    graph.bindInputPort("d", d);
    graph.bindInputPort("mask", mask);

    (void)makeRegister(graph, "state_storage", "state_reg", 130, "state");
    const auto stateRead = makeRegisterRead(graph, "state_read", "state_read_op", 130, "state");
    graph.bindOutputPort("q", stateRead);

    const auto one = makeConstant(graph, "one", "one_const", 1, "1'b1");
    makeRegisterWrite(graph, "state_write", one, d, mask, clk, "state");

    return design;
}

Design buildWideFullMaskRegisterDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto clk = makeValue(graph, "clk", 1, false);
    const auto d = makeValue(graph, "d", 130, false);
    graph.bindInputPort("clk", clk);
    graph.bindInputPort("d", d);

    (void)makeRegister(graph, "state_storage", "state_reg", 130, "state");
    const auto stateRead = makeRegisterRead(graph, "state_read", "state_read_op", 130, "state");
    graph.bindOutputPort("q", stateRead);

    const auto one = makeConstant(graph, "one", "one_const", 1, "1'b1");
    const auto fullMask =
        makeConstant(graph, "full_mask", "full_mask_const", 130, "130'h3FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF");
    makeRegisterWrite(graph, "state_write", one, d, fullMask, clk, "state");

    return design;
}

Design buildWideUnknownConstantDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto out = makeValue(graph, "y", 130, false);
    graph.bindOutputPort("y", out);
    const auto unknownConst =
        makeConstant(graph, "wide_unknown", "wide_unknown_const", 130, "130'h3x0000000000000000000000000000001");
    const auto assign = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_y"));
    graph.addOperand(assign, unknownConst);
    graph.addResult(assign, out);

    return design;
}

Design buildDualEdgeClockDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto clk = makeValue(graph, "clk", 1, false);
    const auto d = makeValue(graph, "d", 1, false);
    graph.bindInputPort("clk", clk);
    graph.bindInputPort("d", d);

    (void)makeRegister(graph, "pos_storage", "pos_reg", 1, "pos_state");
    (void)makeRegister(graph, "neg_storage", "neg_reg", 1, "neg_state");
    const auto posRead = makeRegisterRead(graph, "pos_read", "pos_read_op", 1, "pos_state");
    graph.bindOutputPort("q", posRead);

    const auto one = makeConstant(graph, "one", "one_const", 1, "1'b1");
    const auto mask = makeConstant(graph, "mask", "mask_const", 1, "1'b1");
    makeRegisterWriteWithEdge(graph, "pos_write", one, d, mask, clk, "pos_state", "posedge");
    makeRegisterWriteWithEdge(graph, "neg_write", one, d, mask, clk, "neg_state", "negedge");

    return design;
}

Design buildMediumShardedDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    auto current = makeValue(graph, "in", 1, false);
    graph.bindInputPort("in", current);

    for (int i = 0; i < 140; ++i)
    {
        const auto next = makeValue(graph, "tmp_" + std::to_string(i), 1, false);
        const auto op = graph.createOperation(OperationKind::kNot, graph.internSymbol("not_" + std::to_string(i)));
        graph.addOperand(op, current);
        graph.addResult(op, next);
        current = next;
    }

    graph.bindOutputPort("out", current);
    return design;
}

Design buildShardedNoCommitInputOutputDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    auto current = makeValue(graph, "a", 1, false);
    const auto clk = makeValue(graph, "clk", 1, false);
    graph.bindInputPort("a", current);
    graph.bindInputPort("clk", clk);

    for (int i = 0; i < 140; ++i)
    {
        const auto next = makeValue(graph, "dirty_tmp_" + std::to_string(i), 1, false);
        const auto op = graph.createOperation(OperationKind::kNot, graph.internSymbol("dirty_not_" + std::to_string(i)));
        graph.addOperand(op, current);
        graph.addResult(op, next);
        current = next;
    }
    graph.bindOutputPort("y", current);

    (void)makeRegister(graph, "state_storage", "state_reg", 1, "state");
    const auto zeroCond = makeConstant(graph, "zero_cond", "zero_cond_const", 1, "1'b0");
    const auto oneMask = makeConstant(graph, "one_mask", "one_mask_const", 1, "1'b1");
    makeRegisterWrite(graph, "state_write", zeroCond, current, oneMask, clk, "state");
    return design;
}

Design buildSelectiveReplayShardedDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    auto inputCurrent = makeValue(graph, "in", 1, false);
    graph.bindInputPort("in", inputCurrent);

    for (int i = 0; i < 16; ++i)
    {
        const auto next = makeValue(graph, "input_tmp_" + std::to_string(i), 1, false);
        const auto op = graph.createOperation(OperationKind::kNot, graph.internSymbol("input_not_" + std::to_string(i)));
        graph.addOperand(op, inputCurrent);
        graph.addResult(op, next);
        inputCurrent = next;
    }
    graph.bindOutputPort("input_out", inputCurrent);

    const auto constSeed = makeConstant(graph, "const_seed", "const_seed_op", 1, "1'b1");
    auto constCurrent = constSeed;
    for (int i = 0; i < 176; ++i)
    {
        const auto next = makeValue(graph, "const_tmp_" + std::to_string(i), 1, false);
        const auto op = graph.createOperation(OperationKind::kNot, graph.internSymbol("const_not_" + std::to_string(i)));
        graph.addOperand(op, constCurrent);
        graph.addResult(op, next);
        constCurrent = next;
    }
    graph.bindOutputPort("const_out", constCurrent);
    return design;
}


Design buildConvergentActiveReplayDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    auto aCurrent = makeValue(graph, "a", 1, false);
    auto bCurrent = makeValue(graph, "b", 1, false);
    graph.bindInputPort("a", aCurrent);
    graph.bindInputPort("b", bCurrent);

    // Emit the B-only chain first so a later A-only source activation has to
    // prove it can skip earlier independent shards before reaching the shared
    // convergent fanout.
    for (int i = 0; i < 60; ++i)
    {
        const auto next = makeValue(graph, "b_tmp_" + std::to_string(i), 1, false);
        const auto op = graph.createOperation(OperationKind::kNot, graph.internSymbol("b_not_" + std::to_string(i)));
        graph.addOperand(op, bCurrent);
        graph.addResult(op, next);
        bCurrent = next;
    }

    for (int i = 0; i < 60; ++i)
    {
        const auto next = makeValue(graph, "a_tmp_" + std::to_string(i), 1, false);
        const auto op = graph.createOperation(OperationKind::kNot, graph.internSymbol("a_not_" + std::to_string(i)));
        graph.addOperand(op, aCurrent);
        graph.addResult(op, next);
        aCurrent = next;
    }

    auto sharedCurrent = makeValue(graph, "shared_xor", 1, false);
    const auto xorOp = graph.createOperation(OperationKind::kXor, graph.internSymbol("shared_xor_op"));
    graph.addOperand(xorOp, aCurrent);
    graph.addOperand(xorOp, bCurrent);
    graph.addResult(xorOp, sharedCurrent);

    for (int i = 0; i < 12; ++i)
    {
        const auto next = makeValue(graph, "shared_tmp_" + std::to_string(i), 1, false);
        const auto op = graph.createOperation(OperationKind::kNot, graph.internSymbol("shared_not_" + std::to_string(i)));
        graph.addOperand(op, sharedCurrent);
        graph.addResult(op, next);
        sharedCurrent = next;
    }

    graph.bindOutputPort("y", sharedCurrent);
    return design;
}

Design buildShiftDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto in = makeValue(graph, "in", 8, false);
    const auto amount = makeValue(graph, "amount", 3, false);
    graph.bindInputPort("in", in);
    graph.bindInputPort("amount", amount);

    const auto outL = makeValue(graph, "shl_y", 8, false);
    const auto outR = makeValue(graph, "lshr_y", 8, false);
    graph.bindOutputPort("shl_y", outL);
    graph.bindOutputPort("lshr_y", outR);

    const auto shlOp = graph.createOperation(OperationKind::kShl, graph.internSymbol("shl_y_op"));
    graph.addOperand(shlOp, in);
    graph.addOperand(shlOp, amount);
    graph.addResult(shlOp, outL);

    const auto lshrOp = graph.createOperation(OperationKind::kLShr, graph.internSymbol("lshr_y_op"));
    graph.addOperand(lshrOp, in);
    graph.addOperand(lshrOp, amount);
    graph.addResult(lshrOp, outR);

    return design;
}

Design buildWideShiftDesign()
{
    Design design;
    auto &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto in = makeValue(graph, "in", 130, true);
    const auto amount = makeValue(graph, "amount", 7, false);
    graph.bindInputPort("in", in);
    graph.bindInputPort("amount", amount);

    const auto outL = makeValue(graph, "shl_y", 130, false);
    const auto outR = makeValue(graph, "lshr_y", 130, false);
    const auto outA = makeValue(graph, "ashr_y", 130, true);
    graph.bindOutputPort("shl_y", outL);
    graph.bindOutputPort("lshr_y", outR);
    graph.bindOutputPort("ashr_y", outA);

    const auto shlOp = graph.createOperation(OperationKind::kShl, graph.internSymbol("wide_shl_y_op"));
    graph.addOperand(shlOp, in);
    graph.addOperand(shlOp, amount);
    graph.addResult(shlOp, outL);

    const auto lshrOp = graph.createOperation(OperationKind::kLShr, graph.internSymbol("wide_lshr_y_op"));
    graph.addOperand(lshrOp, in);
    graph.addOperand(lshrOp, amount);
    graph.addResult(lshrOp, outR);

    const auto ashrOp = graph.createOperation(OperationKind::kAShr, graph.internSymbol("wide_ashr_y_op"));
    graph.addOperand(ashrOp, in);
    graph.addOperand(ashrOp, amount);
    graph.addResult(ashrOp, outA);

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

    // Keep an even number of inversions so q preserves stage3 while forcing
    // the state-read dependent path to span multiple behavior shards.  The
    // activity-watermark regression below can then prove changed state does
    // not invalidate the entire combinational schedule.
    auto qCurrent = stage3Read;
    for (int i = 0; i < 160; ++i)
    {
        const auto next = makeValue(graph, "stage3_q_tmp_" + std::to_string(i), 1, false);
        const auto op = graph.createOperation(OperationKind::kNot, graph.internSymbol("stage3_q_not_" + std::to_string(i)));
        graph.addOperand(op, qCurrent);
        graph.addResult(op, next);
        qCurrent = next;
    }
    graph.bindOutputPort("q", qCurrent);

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
                          const std::string &sourceText,
                          const std::string &extraCxxFlags = {})
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
    std::vector<std::filesystem::path> compileInputs{runnerPath};
    const auto manifestPath = dir / (baseName + ".manifest");
    if (std::filesystem::exists(manifestPath))
    {
        std::ifstream manifest(manifestPath);
        std::string rel;
        while (std::getline(manifest, rel))
        {
            if (rel.ends_with(".cpp"))
            {
                compileInputs.push_back(dir / rel);
            }
        }
    }
    else
    {
        compileInputs.push_back(dir / (baseName + ".cpp"));
    }
    std::string compileCmd = cxx + " -std=c++20";
    if (!extraCxxFlags.empty())
    {
        compileCmd += " " + extraCxxFlags;
    }
    compileCmd += " -I " + dir.string();
    for (const auto &input : compileInputs)
    {
        compileCmd += " " + input.string();
    }
    compileCmd += " -o " + binaryPath.string();
    if (std::system(compileCmd.c_str()) != 0)
    {
        throw std::runtime_error("failed to compile emitted runtime harness");
    }
    if (std::system(binaryPath.string().c_str()) != 0)
    {
        throw std::runtime_error("emitted runtime harness execution failed");
    }
}


void instrumentSchedCounters(const std::filesystem::path &dir,
                             const std::string &baseName,
                             std::size_t shardCount)
{
    for (std::size_t i = 0; i < shardCount; ++i)
    {
        const auto path = dir / (baseName + "_sched_" + std::to_string(i) + ".cpp");
        std::string text = readFile(path);
        expect(!text.empty(), "instrumented sched shard should exist");
        const std::string signature = "void SSimTop::sched_" + std::to_string(i) + "() {\n";
        const std::string replacement = "extern int wolvrix_test_sched_counts[];\n" + signature +
                                        "++wolvrix_test_sched_counts[" + std::to_string(i) + "];\n";
        const auto pos = text.find(signature);
        expect(pos != std::string::npos, "instrumented sched shard should contain its function signature");
        text.replace(pos, signature.size(), replacement);
        std::ofstream out(path);
        if (!out.is_open())
        {
            throw std::runtime_error("failed to rewrite instrumented sched shard");
        }
        out << text;
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
    expect(result.artifacts.size() >= 3, "EmitGsimCpp should report header, source, manifest, and any auxiliary implementation artifacts");

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

void testDifftestCompatibilityAccessorsUseTopLevelPorts()
{
    Design design = buildDifftestTopPortDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "difftest_ports";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("difftest_ports");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp should emit difftest port fixture");
    expect(!diags.hasError(), "EmitGsimCpp should not error on difftest port fixture");

    const std::string header = readFile(dir / "difftest_ports.hpp");
    expect(contains(header, "std::uint64_t get_difftest_exit() const { return output_difftest_exit_; }"),
           "top-level difftest_exit port accessor should read emitted output port storage");
    expect(contains(header, "std::uint64_t get_difftest_step() const { return output_difftest_step_; }"),
           "top-level difftest_step port accessor should read emitted output port storage");
    expect(contains(header, "get_difftest__DOT__exit() const { return get_difftest_exit(); }"),
           "downstream compatibility difftest exit accessor should forward to top-level output port");
    expect(contains(header, "get_difftest__DOT__step() const { return difftest_step_; }"),
           "downstream compatibility difftest step accessor should keep the no-diff progress counter fallback");
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
    const std::string source = readFile(dir / "runtime_top.cpp");
    expect(contains(header, "void settle()"), "runtime fixture should expose settle");
    expect(contains(header, "void commit_step()"), "runtime fixture should expose commit_step");
    expect(contains(header, "bool non_clock_inputs_dirty_ = true;"),
           "runtime fixture should track non-clock input dirtiness");
    expect(contains(source, "*state_ = SSimTopState();"),
           "runtime fixture should reset pooled state via SSimTopState value reset");
    expect(!contains(source, "state_->stateU8[0] = 0;"),
           "runtime fixture should not redundantly zero pooled register storage after SSimTopState reset");

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

void testResetCompatibilitySetterDrivesTopLevelResetPort()
{
    Design design = buildResetPortForwardingDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "reset_port_forwarding";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("reset_port_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp reset-port fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp reset-port fixture should not emit errors");

    const std::string header = readFile(dir / "reset_port_top.hpp");
    const std::string source = readFile(dir / "reset_port_top.cpp");
    expect(contains(header, "void set_reset(unsigned reset)"),
           "compatibility reset API should remain available");
    expect(!contains(header, "void set_reset(std::uint8_t value)"),
           "top-level reset input should be served by the compatibility reset API without an ambiguous overload");
    expect(contains(source, "void SSimTop::set_reset(unsigned reset) { const auto value = static_cast<std::uint8_t>(reset);"),
           "compatibility reset API should forward to the top-level reset port when present");

    const std::string runner = R"CPP(
#include "reset_port_top.hpp"

int main() {
    SSimTop sim;
    sim.set_reset(1);
    sim.settle();
    if (sim.get_y() != 1) {
        return 1;
    }
    sim.set_reset(0);
    sim.step();
    if (sim.get_y() != 0) {
        return 2;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "reset_port_top", runner);
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

    const std::string source = readFile(dir / "dpic_import_top.cpp");

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


void testDpicCallCompileAndRun()
{
    Design design = buildDpicCallDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "dpic_call_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("dpic_call_top");
    options.topOverrides = {"top"};
    options.attributes["dpic_trace"] = "1";

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp should lower input-only kDpicCall ops");
    expect(!diags.hasError(), "EmitGsimCpp should not report input-only kDpicCall ops as unsupported");

    const std::string source = readFile(dir / "dpic_call_top.cpp");
    expect(contains(source, "#include \"difftest-dpic.h\""), "dpic call source should include the generated DPI-C header");
    const std::string commitChunk = readFile(dir / "dpic_call_top_commit_chunk_posedge_clk_0.cpp");
    expect(contains(commitChunk, "dpi_capture(static_cast<std::uint8_t>"), "dpic call source should invoke the imported function");
    expect(contains(commitChunk, "[wolvrix-gsim-dpic]"), "dpic trace mode should emit per-site diagnostics");
    expect(contains(commitChunk, "first_cond"), "dpic trace mode should log the first observed condition");
    expect(contains(commitChunk, "miss="), "dpic trace mode should sample repeated false conditions");

    std::ofstream stub(dir / "difftest-dpic.h");
    if (!stub.is_open()) {
        throw std::runtime_error("failed to write dpic stub header");
    }
    stub << R"HPP(
#pragma once
#include <cstdint>
inline unsigned g_dpic_capture_calls = 0;
inline std::uint8_t g_dpic_capture_last = 0;
extern "C" inline void dpi_capture(std::uint8_t value) {
    ++g_dpic_capture_calls;
    g_dpic_capture_last = value;
}
)HPP";
    stub.close();

    const std::string runner = R"CPP(
#include "dpic_call_top.hpp"
#include "difftest-dpic.h"

int main() {
    SSimTop sim;
    sim.set_a(7);
    sim.set_clk(0);
    sim.step();
    if (g_dpic_capture_calls != 0) {
        return 1;
    }
    sim.set_clk(1);
    sim.step();
    if (g_dpic_capture_calls != 1 || g_dpic_capture_last != 7) {
        return 2;
    }
    if (sim.get_difftest__DOT__step() != 1) {
        return 3;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "dpic_call_top", runner);
}

void testNoOutputShardedDpicConditionReplaysDirtyInputs()
{
    Design design = buildNoOutputShardedDpicConditionDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "dpic_no_output_dirty_condition";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("dpic_no_output_top");
    options.topOverrides = {"top"};
    options.attributes["behavior_shard_max_bytes"] = "512";
    options.attributes["activity_shard_watermark"] = "1";

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp should lower no-output sharded DPIC condition fixture");
    expect(!diags.hasError(), "EmitGsimCpp should not report no-output DPIC condition fixture errors");

    const std::string source = readFile(dir / "dpic_no_output_top.cpp");
    expect(!contains(source, "if (sequential_edge_pending_) {\n            non_clock_inputs_dirty_ = false;"),
           "sequential edge with no output ports must not drop pending dirty input replay before DPIC conditions");
    expect(contains(source, "dirty_replayed_ = true;\n        replay_dirty_input_shards();"),
           "sequential edge should replay dirty input shards before sampling no-output DPIC conditions");
    expect(contains(source, "if (!post_commit_settled_ && (non_clock_inputs_dirty_ || dirty_replayed_))"),
           "sharded final settle should not run solely for input-only DPIC side-effect commits");
    const std::string chunk = readFile(dir / "dpic_no_output_top_commit_chunk_posedge_clk_0.cpp");
    expect(contains(chunk, "bool& dirty_on_commit_"),
           "input-only DPIC statement chunks should carry dirty-on-commit metadata");
    expect(!contains(chunk, "dirty_on_commit_ = true;"),
           "input-only DPIC calls should not force all shards dirty after every side-effect event");

    std::ofstream stub(dir / "difftest-dpic.h");
    if (!stub.is_open()) {
        throw std::runtime_error("failed to write dpic stub header");
    }
    stub << R"HPP(
#pragma once
#include <cstdint>
inline unsigned g_dpic_capture_calls = 0;
inline std::uint8_t g_dpic_capture_last = 0;
extern "C" inline void dpi_capture(std::uint8_t value) {
    ++g_dpic_capture_calls;
    g_dpic_capture_last = value;
}
)HPP";
    stub.close();

    const std::string runner = R"CPP(
#include "dpic_no_output_top.hpp"
#include "difftest-dpic.h"

int main() {
    SSimTop sim;
    sim.set_en(0);
    sim.set_clk(0);
    sim.step();
    if (g_dpic_capture_calls != 0) {
        return 1;
    }
    sim.set_en(1);
    sim.set_clk(1);
    sim.step();
    if (g_dpic_capture_calls != 1 || g_dpic_capture_last != 1) {
        return 2;
    }
    if (sim.get_difftest__DOT__step() != 1) {
        return 3;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "dpic_no_output_top", runner);
}

void testDpicDirtyReplayIncludesProducerClosure()
{
    Design design = buildNoOutputDpicConditionWithProducerClosureDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "dpic_dirty_replay_producer_closure";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("dpic_closure_top");
    options.topOverrides = {"top"};
    options.attributes["behavior_shard_max_bytes"] = "512";

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp should lower DPIC producer-closure fixture");
    expect(!diags.hasError(), "EmitGsimCpp should not report DPIC producer-closure fixture errors");

    const std::string source = readFile(dir / "dpic_closure_top.cpp");
    const std::string marker = "void SSimTop::replay_dirty_input_shards() {";
    const auto markerPos = source.find(marker);
    expect(markerPos != std::string::npos,
           "producer-closure fixture should emit replay_dirty_input_shards helper");
    const auto endPos = source.find("}\n\nvoid SSimTop::commit_step()", markerPos);
    expect(endPos != std::string::npos,
           "producer-closure fixture should place replay helper before commit_step");
    const std::string replayBody = source.substr(markerPos, endPos - markerPos);

    std::size_t replayCalls = 0;
    std::size_t searchPos = 0;
    while ((searchPos = replayBody.find("sched_", searchPos)) != std::string::npos)
    {
        ++replayCalls;
        searchPos += 6;
    }
    expect(replayCalls > 4,
           "dirty replay should include non-dirty producer shard closure before the dirty DPIC condition shard");

    std::ofstream stub(dir / "difftest-dpic.h");
    if (!stub.is_open()) {
        throw std::runtime_error("failed to write dpic stub header");
    }
    stub << R"HPP(
#pragma once
#include <cstdint>
inline unsigned g_dpic_capture_calls = 0;
inline std::uint8_t g_dpic_capture_last = 0;
extern "C" inline void dpi_capture(std::uint8_t value) {
    ++g_dpic_capture_calls;
    g_dpic_capture_last = value;
}
)HPP";
    stub.close();

    const std::string runner = R"CPP(
#include "dpic_closure_top.hpp"
#include "difftest-dpic.h"

int main() {
    SSimTop sim;
    sim.set_en(0);
    sim.set_clk(0);
    sim.step();
    if (g_dpic_capture_calls != 0) {
        return 1;
    }
    sim.set_en(1);
    sim.set_clk(1);
    sim.step();
    if (g_dpic_capture_calls != 1 || g_dpic_capture_last != 1) {
        return 2;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "dpic_closure_top", runner);
}

void testShardedDirtyReplaySettlesLatchBeforeDpicCondition()
{
    Design design = buildNoOutputLatchGatedDpicDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "dpic_latch_dirty_condition";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("dpic_latch_top");
    options.topOverrides = {"top"};
    options.attributes["behavior_shard_max_bytes"] = "512";
    options.attributes["activity_shard_watermark"] = "1";

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp should lower latch-gated DPIC fixture");
    expect(!diags.hasError(), "EmitGsimCpp should not report latch-gated DPIC fixture errors");

    const std::string source = readFile(dir / "dpic_latch_top.cpp");
    expect(contains(source, "if (non_clock_inputs_dirty_) {\n        settle();"),
           "latch-bearing sharded designs should settle dirty inputs before same-step DPIC edge sampling");

    std::ofstream stub(dir / "difftest-dpic.h");
    if (!stub.is_open()) {
        throw std::runtime_error("failed to write dpic stub header");
    }
    stub << R"HPP(
#pragma once
#include <cstdint>
inline unsigned g_dpic_capture_calls = 0;
inline std::uint8_t g_dpic_capture_last = 0;
extern "C" inline void dpi_capture(std::uint8_t value) {
    ++g_dpic_capture_calls;
    g_dpic_capture_last = value;
}
)HPP";
    stub.close();

    const std::string runner = R"CPP(
#include "dpic_latch_top.hpp"
#include "difftest-dpic.h"

int main() {
    SSimTop sim;
    sim.set_en(0);
    sim.set_d(0);
    sim.set_clk(0);
    sim.set_reset(0);
    sim.step();
    if (g_dpic_capture_calls != 0) {
        return 1;
    }
    sim.set_en(1);
    sim.set_d(1);
    sim.set_clk(1);
    sim.step();
    if (g_dpic_capture_calls != 1 || g_dpic_capture_last != 1) {
        return 2;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "dpic_latch_top", runner);
}

void testNoDiffGuardedDpicCallsCompileOut()
{
    Design design = buildNoDiffDifftestDpicDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "dpic_no_diff_guard";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("dpic_no_diff_top");
    options.topOverrides = {"top"};
    options.attributes["behavior_shard_max_bytes"] = "512";
    options.attributes["activity_shard_watermark"] = "1";

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp should lower no-diff guarded DPIC fixture");
    expect(!diags.hasError(), "EmitGsimCpp should not report no-diff guarded DPIC errors");

    const std::string source = readFile(dir / "dpic_no_diff_top.cpp");
    const std::string chunk = readFile(dir / "dpic_no_diff_top_commit_chunk_posedge_clk_0.cpp");
    expect(contains(source, "#ifndef CONFIG_NO_DIFFTEST\n        static constexpr std::uint32_t kDpicPreSettleShards") &&
               contains(source, "        settle();\n        post_commit_settled_ = true;\n#endif"),
           "pure difftest DPIC pre-settle should be compiled out in CONFIG_NO_DIFFTEST builds");
    expect(contains(chunk, "#ifndef CONFIG_NO_DIFFTEST") &&
               contains(chunk, "v_difftest_TestEvent") &&
               contains(chunk, "#endif"),
           "difftest DPIC side-effect chunks should compile out with CONFIG_NO_DIFFTEST");

    std::ofstream stub(dir / "difftest-dpic.h");
    if (!stub.is_open()) {
        throw std::runtime_error("failed to write empty dpic stub header");
    }
    stub << "#pragma once\n";
    stub.close();

    const std::string runner = R"CPP(
#include "dpic_no_diff_top.hpp"

int main() {
    SSimTop sim;
    sim.set_clk(0);
    sim.step();
    sim.set_clk(1);
    sim.step();
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "dpic_no_diff_top", runner, "-DCONFIG_NO_DIFFTEST");
}

void testNoDiffGuardedValueDpicCallsDefaultOut()
{
    Design design = buildNoDiffValueDifftestDpicDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "dpic_no_diff_value_guard";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("dpic_no_diff_value_top");
    options.topOverrides = {"top"};
    options.attributes["behavior_shard_max_bytes"] = "512";
    options.attributes["activity_shard_watermark"] = "1";

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp should lower no-diff value DPIC fixture");
    expect(!diags.hasError(), "EmitGsimCpp should not report no-diff value DPIC errors");

    std::ofstream stub(dir / "difftest-dpic.h");
    if (!stub.is_open()) {
        throw std::runtime_error("failed to write empty dpic stub header");
    }
    stub << "#pragma once\n";
    stub.close();

    const std::string source = readFile(dir / "dpic_no_diff_value_top.cpp");
    expect(contains(source, "#ifndef CONFIG_NO_DIFFTEST\nif (") &&
               contains(source, "v_difftest_ReturnValue") &&
               contains(source, "v_difftest_OutputValue"),
           "value-producing v_difftest DPIC expressions should guard calls in no-diff builds");

    const std::string runner = R"CPP(
#include "dpic_no_diff_value_top.hpp"

int main() {
    SSimTop sim;
    sim.set_clk(0);
    sim.step();
    sim.set_clk(1);
    sim.step();
    if (sim.get_ret() != 0 || sim.get_out() != 0) {
        return 1;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "dpic_no_diff_value_top", runner, "-DCONFIG_NO_DIFFTEST");

    std::ofstream realHeader(dir / "difftest-dpic.h");
    if (!realHeader.is_open()) {
        throw std::runtime_error("failed to write real dpic header");
    }
    realHeader << "#pragma once\n"
               << "#include <cstdint>\n"
               << "extern \"C\" std::uint8_t v_difftest_ReturnValue(std::uint8_t value);\n"
               << "extern \"C\" void v_difftest_OutputValue(std::uint8_t *value);\n";
    realHeader.close();

    bool shardIncludesDifftestHeader = false;
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
        const auto name = entry.path().filename().string();
        if (name.starts_with("dpic_no_diff_value_top_sched_") && name.ends_with(".cpp")) {
            const std::string shard = readFile(entry.path());
            if (contains(shard, "#include \"difftest-dpic.h\"")) {
                shardIncludesDifftestHeader = true;
                break;
            }
        }
    }
    expect(shardIncludesDifftestHeader,
           "sharded value-producing v_difftest DPIC expressions should include the real difftest header");

    const std::string normalRunner = R"CPP(
#include "dpic_no_diff_value_top.hpp"
#include <cstdint>

extern "C" std::uint8_t v_difftest_ReturnValue(std::uint8_t value) {
    return value ? static_cast<std::uint8_t>(7) : static_cast<std::uint8_t>(0);
}

extern "C" void v_difftest_OutputValue(std::uint8_t *value) {
    *value = 9;
}

int main() {
    SSimTop sim;
    sim.set_clk(0);
    sim.step();
    sim.set_clk(1);
    sim.step();
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "dpic_no_diff_value_top", normalRunner);
}

void testNoDiffGuardedMultiResultDpicCallsDefaultOut()
{
    Design design = buildNoDiffMultiResultDifftestDpicDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "dpic_no_diff_multi_guard";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("dpic_no_diff_multi_top");
    options.topOverrides = {"top"};
    options.attributes["behavior_shard_max_bytes"] = "512";
    options.attributes["activity_shard_watermark"] = "1";

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp should lower no-diff multi-result DPIC fixture");
    expect(!diags.hasError(), "EmitGsimCpp should not report no-diff multi-result DPIC errors");

    std::ofstream stub(dir / "difftest-dpic.h");
    if (!stub.is_open()) {
        throw std::runtime_error("failed to write empty dpic stub header");
    }
    stub << "#pragma once\n";
    stub.close();

    const std::string chunk = readFile(dir / "dpic_no_diff_multi_top_commit_chunk_posedge_clk_0.cpp");
    expect(contains(chunk, "#ifndef CONFIG_NO_DIFFTEST") &&
               contains(chunk, "v_difftest_MultiValue") &&
               contains(chunk, "return result;"),
           "multi-result v_difftest DPIC pre-statement should guard the call and keep default result values");

    const std::string runner = R"CPP(
#include "dpic_no_diff_multi_top.hpp"

int main() {
    SSimTop sim;
    sim.set_clk(0);
    sim.step();
    sim.set_clk(1);
    sim.step();
    if (sim.get_ret() != 0 || sim.get_out() != 0) {
        return 1;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "dpic_no_diff_multi_top", runner, "-DCONFIG_NO_DIFFTEST");
}

void testDpicReturnValueFeedsSequentialWrite()
{
    Design design = buildDpicReturnReadDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "dpic_return_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("dpic_return_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp should lower return-valued kDpicCall ops");
    expect(!diags.hasError(), "EmitGsimCpp should not report return-valued kDpicCall ops as unsupported");

    const std::string source = readFile(dir / "dpic_return_top.cpp");
    expect(contains(source, "#include \"difftest-dpic.h\""),
           "return-valued dpic source should include the generated DPI-C header");
    const std::string commitChunk = readFile(dir / "dpic_return_top_commit_chunk_posedge_clk_0.cpp");
    expect(contains(commitChunk, "difftest_ram_read(static_cast<std::uint64_t>"),
           "return-valued DPIC call should be inlined into the edge update that consumes it");
    expect(!contains(commitChunk, "kDpicCall-output"),
           "return-valued DPIC call should not be rejected as an output-arg call");

    std::ofstream stub(dir / "difftest-dpic.h");
    if (!stub.is_open()) {
        throw std::runtime_error("failed to write dpic stub header");
    }
    stub << R"HPP(
#pragma once
#include <cstdint>
inline unsigned g_dpic_ram_read_calls = 0;
extern "C" inline std::uint64_t difftest_ram_read(std::uint64_t rIdx) {
    ++g_dpic_ram_read_calls;
    return rIdx ^ UINT64_C(0x123456789abcdef0);
}
)HPP";
    stub.close();

    const std::string runner = R"CPP(
#include "dpic_return_top.hpp"
#include "difftest-dpic.h"

int main() {
    SSimTop sim;
    sim.set_addr(UINT64_C(0x10));
    sim.set_clk(0);
    sim.step();
    if (g_dpic_ram_read_calls != 0) {
        return 1;
    }
    sim.set_clk(1);
    sim.step();
    if (g_dpic_ram_read_calls != 1) {
        return 2;
    }
    if (sim.get_data() != (UINT64_C(0x10) ^ UINT64_C(0x123456789abcdef0))) {
        return 3;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "dpic_return_top", runner);
}

void testDpicOutputArgFeedsSequentialWrite()
{
    Design design = buildDpicOutputReadDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "dpic_output_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("dpic_output_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp should lower output-arg kDpicCall ops");
    expect(!diags.hasError(), "EmitGsimCpp should not report output-arg kDpicCall ops as unsupported");

    const std::string source = readFile(dir / "dpic_output_top.cpp");
    expect(contains(source, "#include \"difftest-dpic.h\""),
           "output-arg dpic source should include the generated DPI-C header");
    const std::string commitChunk = readFile(dir / "dpic_output_top_commit_chunk_posedge_clk_0.cpp");
    expect(contains(commitChunk, "flash_read(static_cast<std::uint32_t>"),
           "output-arg DPIC call should preserve the imported function call");
    expect(contains(commitChunk, "&dpic_out_"),
           "output-arg DPIC call should pass writable pointer storage");
    expect(!contains(commitChunk, "kDpicCall-noninput"),
           "output-arg DPIC call should not be rejected as non-input");

    std::ofstream stub(dir / "difftest-dpic.h");
    if (!stub.is_open()) {
        throw std::runtime_error("failed to write dpic stub header");
    }
    stub << R"HPP(
#pragma once
#include <cstdint>
inline unsigned g_flash_read_calls = 0;
extern "C" inline void flash_read(std::uint32_t addr, std::uint64_t *data) {
    ++g_flash_read_calls;
    *data = static_cast<std::uint64_t>(addr) | UINT64_C(0xdead000000000000);
}
)HPP";
    stub.close();

    const std::string runner = R"CPP(
#include "dpic_output_top.hpp"
#include "difftest-dpic.h"

int main() {
    SSimTop sim;
    sim.set_addr(0x34);
    sim.set_clk(0);
    sim.step();
    if (g_flash_read_calls != 0) {
        return 1;
    }
    sim.set_clk(1);
    sim.step();
    if (g_flash_read_calls != 1) {
        return 2;
    }
    if (sim.get_data() != (UINT64_C(0xdead000000000000) | UINT64_C(0x34))) {
        return 3;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "dpic_output_top", runner);
}

void testDpicJtagTickMultiResultCallOnceAndConditionGated()
{
    Design design = buildDpicJtagTickMultiResultDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "dpic_jtag_tick_multi_result_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("dpic_jtag_tick_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp should lower return-plus-output-arg jtag_tick kDpicCall ops");
    expect(!diags.hasError(), "EmitGsimCpp should not report return-plus-output-arg jtag_tick as unsupported");

    const std::string source = readFile(dir / "dpic_jtag_tick_top.cpp");
    expect(contains(source, "#include \"difftest-dpic.h\""),
           "jtag_tick source should include the generated DPI-C header");
    const std::string header = readFile(dir / "dpic_jtag_tick_top_internal.hpp");
    expect(contains(header, "extern \"C\" int jtag_tick(std::uint8_t *, std::uint8_t *, std::uint8_t *, std::uint8_t *, std::uint8_t);"),
           "jtag_tick prototype should preserve the int return and output pointer ABI");
    const std::string preChunk = readFile(dir / "dpic_jtag_tick_top_commit_chunk_posedge_clk_0.cpp");
    expect(contains(preChunk, "jtag_tick(&result.out0, &result.out1, &result.out2, &result.out3, static_cast<std::uint8_t>"),
           "jtag_tick call should pass four output pointers before the input argument");
    expect(countOccurrences(preChunk, "jtag_tick(") == 1,
           "one same-edge multi-result kDpicCall should emit exactly one jtag_tick invocation");
    expect(contains(preChunk, "if (input_en_)"),
           "jtag_tick should be condition-gated before the imported call");
    expect(!contains(preChunk, "kDpicCall-output"),
           "return-plus-output jtag_tick should not be rejected as output-only unsupported");

    std::ofstream stub(dir / "difftest-dpic.h");
    if (!stub.is_open()) {
        throw std::runtime_error("failed to write dpic stub header");
    }
    stub << R"HPP(
#pragma once
#include <cstdint>

inline unsigned g_jtag_tick_calls = 0;
inline std::uint8_t g_jtag_tick_last_input = 0;

extern "C" inline int jtag_tick(std::uint8_t *tck,
                                std::uint8_t *tms,
                                std::uint8_t *tdi,
                                std::uint8_t *trstn,
                                std::uint8_t tdo) {
    ++g_jtag_tick_calls;
    g_jtag_tick_last_input = tdo;
    *tck = static_cast<std::uint8_t>(g_jtag_tick_calls & 1U);
    *tms = static_cast<std::uint8_t>((g_jtag_tick_calls >> 1U) & 1U);
    *tdi = static_cast<std::uint8_t>(tdo & 1U);
    *trstn = static_cast<std::uint8_t>(1U);
    return static_cast<int>(UINT32_C(0xCAFE0000) |
                            (static_cast<std::uint32_t>(g_jtag_tick_calls) << 8U) |
                            static_cast<std::uint32_t>(tdo));
}
)HPP";
    stub.close();

    const std::string runner = R"CPP(
#include "dpic_jtag_tick_top.hpp"
#include "difftest-dpic.h"

int main() {
    SSimTop sim;
    sim.set_en(0);
    sim.set_tick_in(1);
    sim.set_clk(0);
    sim.step();

    sim.set_clk(1);
    sim.step();
    if (g_jtag_tick_calls != 0) {
        return 1;
    }
    if (sim.get_result() != 0 || sim.get_tck() != 0 || sim.get_tms() != 0 ||
        sim.get_tdi() != 0 || sim.get_trstn() != 0) {
        return 2;
    }

    sim.set_clk(0);
    sim.step();
    if (g_jtag_tick_calls != 0) {
        return 3;
    }

    sim.set_en(1);
    sim.set_tick_in(1);
    sim.set_clk(1);
    sim.step();
    if (g_jtag_tick_calls != 1 || g_jtag_tick_last_input != 1) {
        return 4;
    }
    if (sim.get_result() != (UINT32_C(0xCAFE0000) | UINT32_C(0x100) | UINT32_C(1))) {
        return 5;
    }
    if (sim.get_tck() != 1 || sim.get_tms() != 0 || sim.get_tdi() != 1 || sim.get_trstn() != 1) {
        return 6;
    }

    sim.step();
    if (g_jtag_tick_calls != 1) {
        return 7;
    }

    sim.set_clk(0);
    sim.step();
    sim.set_tick_in(0);
    sim.set_clk(1);
    sim.step();
    if (g_jtag_tick_calls != 2 || g_jtag_tick_last_input != 0) {
        return 8;
    }
    if (sim.get_result() != (UINT32_C(0xCAFE0000) | UINT32_C(0x200))) {
        return 9;
    }
    if (sim.get_tck() != 0 || sim.get_tms() != 1 || sim.get_tdi() != 0 || sim.get_trstn() != 1) {
        return 10;
    }

    return 0;
}
)CPP";

    compileAndRunHarness(dir, "dpic_jtag_tick_top", runner);
}

void testDpicJtagTickPreOnlySequentialClockDeclared()
{
    Design design = buildDpicJtagTickPreOnlyOutputDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "dpic_jtag_tick_pre_only_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("dpic_jtag_tick_pre_only_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp should lower pre-only return-plus-output-arg jtag_tick ops");
    expect(!diags.hasError(), "EmitGsimCpp should not reject pre-only return-plus-output-arg jtag_tick ops");

    const std::string publicHeader = readFile(dir / "dpic_jtag_tick_pre_only_top.hpp");
    expect(contains(publicHeader, "bool prev_clk_ = false;"),
           "pre-only sequential DPI domains should declare their previous-clock state");
    const std::string internalHeader = readFile(dir / "dpic_jtag_tick_pre_only_top_internal.hpp");
    expect(contains(internalHeader, "extern \"C\" int jtag_tick(std::uint8_t *, std::uint8_t *, std::uint8_t *, std::uint8_t *, std::uint8_t);"),
           "pre-only jtag_tick prototype should preserve the int return and output pointer ABI");

    const std::string preChunk = readFile(dir / "dpic_jtag_tick_pre_only_top_commit_chunk_posedge_clk_0.cpp");
    expect(countOccurrences(preChunk, "jtag_tick(") == 1,
           "pre-only return-plus-output kDpicCall should emit exactly one jtag_tick invocation");
    expect(contains(preChunk, "if (input_en_)"),
           "pre-only jtag_tick should remain condition-gated");

    std::ofstream stub(dir / "difftest-dpic.h");
    if (!stub.is_open()) {
        throw std::runtime_error("failed to write dpic stub header");
    }
    stub << R"HPP(
#pragma once
#include <cstdint>

inline unsigned g_jtag_tick_calls = 0;

extern "C" inline int jtag_tick(std::uint8_t *tck,
                                std::uint8_t *tms,
                                std::uint8_t *tdi,
                                std::uint8_t *trstn,
                                std::uint8_t tdo) {
    ++g_jtag_tick_calls;
    *tck = static_cast<std::uint8_t>(1U);
    *tms = static_cast<std::uint8_t>(tdo & 1U);
    *tdi = static_cast<std::uint8_t>((tdo ^ 1U) & 1U);
    *trstn = static_cast<std::uint8_t>(1U);
    return static_cast<int>(UINT32_C(0xFACE0000) | static_cast<std::uint32_t>(tdo));
}
)HPP";
    stub.close();

    const std::string runner = R"CPP(
#include "dpic_jtag_tick_pre_only_top.hpp"
#include "difftest-dpic.h"

int main() {
    SSimTop sim;
    sim.set_en(1);
    sim.set_tick_in(1);
    sim.set_clk(0);
    sim.step();
    if (g_jtag_tick_calls != 0) {
        return 1;
    }

    sim.set_clk(1);
    sim.step();
    if (g_jtag_tick_calls != 1) {
        return 2;
    }
    if (sim.get_result() != (UINT32_C(0xFACE0000) | UINT32_C(1))) {
        return 3;
    }
    if (sim.get_tck() != 1 || sim.get_tms() != 1 || sim.get_tdi() != 0 || sim.get_trstn() != 1) {
        return 4;
    }

    sim.step();
    if (g_jtag_tick_calls != 1) {
        return 5;
    }

    return 0;
}
)CPP";

    compileAndRunHarness(dir, "dpic_jtag_tick_pre_only_top", runner);
}

void testDerivedClockEdgesSeePriorDomainCommits()
{
    Design design = buildDerivedClockPostCommitReplayDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "derived_clock_post_commit_replay";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("derived_clock_top");
    options.topOverrides = {"top"};
    options.attributes["behavior_shard_max_bytes"] = "4096";
    options.attributes["activity_shard_watermark"] = "1";

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp derived-clock fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp derived-clock fixture should not emit errors");

    const std::string source = readFile(dir / "derived_clock_top.cpp");
    expect(contains(source, "dirty_replayed_ = false;"),
           "committed register domains should invalidate dirty replay before later derived-clock edge checks");
    expect(contains(source, "if (non_clock_inputs_dirty_ && !dirty_replayed_) {\n        dirty_replayed_ = true;\n        replay_dirty_input_shards();\n    }\n    if ((!prev_gated_clk_"),
           "derived-clock edge checks should replay dirty shards before evaluating the edge expression");

    const std::string runner = R"CPP(
#include "derived_clock_top.hpp"

int main() {
    SSimTop sim;
    sim.set_clk(0);
    sim.step();
    if (sim.get_data() != 0) {
        return 1;
    }
    sim.set_clk(1);
    sim.step();
    if (sim.get_data() != 0x5a) {
        return 2;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "derived_clock_top", runner);
}

void testSettlePreservesDerivedClockInputEdges()
{
    Design design = buildSettledDerivedInputClockDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "settled_derived_clock_input_edge";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("settled_clock_top");
    options.topOverrides = {"top"};
    options.attributes["behavior_shard_max_bytes"] = "512";
    options.attributes["activity_shard_watermark"] = "1";

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp settled derived-clock fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp settled derived-clock fixture should not emit errors");

    const std::string source = readFile(dir / "settled_clock_top.cpp");
    expect(contains(source, "void SSimTop::settle() {\n    if (clock_inputs_dirty_) {\n        replay_dirty_input_shards();\n        clock_inputs_dirty_ = false;"),
           "settle should replay clock-dependent dirty shards before clearing clock_inputs_dirty_");
    expect(contains(source, "const bool had_non_clock_inputs_dirty_ = non_clock_inputs_dirty_;\n        dirty_replayed_ = true;\n        replay_dirty_input_shards();\n        clock_inputs_dirty_ = false;\n        non_clock_inputs_dirty_ = had_non_clock_inputs_dirty_;"),
           "clock-input replay should not consume pending non-clock dirty state");

    const std::string runner = R"CPP(
#include "settled_clock_top.hpp"

int main() {
    SSimTop sim;
    sim.set_clk(0);
    sim.set_en(1);
    sim.commit_step();
    if (sim.get_data() != 0) {
        return 1;
    }
    sim.set_clk(1);
    sim.settle();
    sim.commit_step();
    if (sim.get_data() != 0x5a) {
        return 2;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "settled_clock_top", runner);
}

void testDpicSamplesPostSequentialSettleState()
{
    Design design = buildDpicPostSequentialSettleDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "dpic_post_seq_settle_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("dpic_post_seq_top");
    options.topOverrides = {"top"};
    options.attributes["behavior_shard_max_bytes"] = "512";
    options.attributes["activity_shard_watermark"] = "1";

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp DPIC post-sequential fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp DPIC post-sequential fixture should not emit errors");

    const std::string source = readFile(dir / "dpic_post_seq_top.cpp");
    expect(contains(source, "domain_reg_committed_"), "commit_step should separate register chunks from DPIC chunks");
    expect(contains(source, "post_commit_settled_"), "commit_step should avoid redundant final settle after pre-DPIC settle");
    expect(contains(source, "kDpicPreSettleShards"), "DPIC post-sequential fixture should emit a pre-settle shard set");
    expect(contains(source, "kDpicPreSettleShards[] = {0U, 16U}"),
           "DPIC post-sequential fixture should seed the upstream producer shard without expanding to every shard");
    expect(!contains(source, "#ifndef CONFIG_NO_DIFFTEST\n        static constexpr std::uint32_t kDpicPreSettleShards"),
           "runtime DPIC calls should keep pre-DPIC settle even in no-diff builds");
    expect(contains(source, "if (domain_reg_committed_) {\n            committed_ = true;\n            any_domain_reg_committed_ = true;\n            non_clock_inputs_dirty_ = true;\n            dirty_replayed_ = false;"),
           "chunked register commits should invalidate dirty replay and activate post-commit settle work");
    const std::string chunk = readFile(dir / "dpic_post_seq_top_commit_chunk_posedge_clk_1.cpp");
    expect(!contains(chunk, "dirty_on_commit_ = true;"),
           "input-only post-sequential DPIC chunks should not dirty all internal shards after sampling");

    std::ofstream stub(dir / "difftest-dpic.h");
    if (!stub.is_open()) {
        throw std::runtime_error("failed to write dpic stub header");
    }
    stub << R"HPP(
#pragma once
#include <cstdint>
inline unsigned g_dpic_capture_calls = 0;
inline std::uint8_t g_dpic_capture_last = 0;
extern "C" inline void dpi_capture(std::uint8_t value) {
    ++g_dpic_capture_calls;
    g_dpic_capture_last = value;
}
)HPP";
    stub.close();

    const std::string runner = R"CPP(
#include "dpic_post_seq_top.hpp"
#include "difftest-dpic.h"

int main() {
    SSimTop sim;
    sim.set_clk(0);
    sim.step();
    if (g_dpic_capture_calls != 0) {
        return 1;
    }
    sim.set_clk(1);
    sim.step();
    if (g_dpic_capture_calls != 1 || g_dpic_capture_last != 0x2a) {
        return 2;
    }
    if (sim.get_pc() != 0x2a) {
        return 3;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "dpic_post_seq_top", runner);
}

void testLatchReadNoOpCompileAndRun()
{
    Design design = buildLatchReadNoOpDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "latch_read_noop_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("latch_read_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp should lower standalone latch declarations and latch reads");
    expect(!diags.hasError(), "EmitGsimCpp should not report latch declaration/read ops as unsupported");

    const std::string header = readFile(dir / "latch_read_top.hpp");
    const std::string internalHeader = readFile(dir / "latch_read_top_internal.hpp");
    const std::string source = readFile(dir / "latch_read_top.cpp");
    expect(contains(internalHeader, "std::vector<std::uint8_t> stateU8") ||
               contains(source, "state_->stateU8["),
           "latch-read fixture should materialize latch storage in the emitted internal state");

    const std::string runner = R"CPP(
#include "latch_read_top.hpp"

int main() {
    SSimTop sim;
    sim.step();
    if (sim.get_y() != 0) {
        return 1;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "latch_read_top", runner);
}

void testMemoryReadCompileAndRun()
{
    Design design = buildMemoryReadDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "memory_read_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("memory_read_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp should lower memory declaration/read fixtures");
    expect(!diags.hasError(), "EmitGsimCpp should not report memory declaration/read fixtures as unsupported");

    const std::string header = readFile(dir / "memory_read_top.hpp");
    const std::string internalHeader = readFile(dir / "memory_read_top_internal.hpp");
    const std::string source = readFile(dir / "memory_read_top.cpp");
    expect(contains(internalHeader, "std::vector<std::uint8_t> mem_mem0_"),
           "memory-read fixture should materialize byte-addressable memory storage in the emitted internal state");

    const std::string runner = R"CPP(
#include "memory_read_top.hpp"
#include <cstdint>

int main() {
    SSimTop sim;
    sim.set_addr(0);
    sim.step();
    if (sim.get_data() != 0) {
        return 1;
    }
    sim.set_addr(2);
    sim.step();
    if (sim.get_data() != 0xA5) {
        return 2;
    }
    sim.set_addr(6);
    sim.step();
    if (sim.get_data() != 0) {
        return 3;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "memory_read_top", runner);
}

void testMemoryWriteCompileAndRun()
{
    Design design = buildMemoryWriteDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "memory_write_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("memory_write_top");
    options.topOverrides = {"top"};
    options.attributes["behavior_shard_max_bytes"] = "512";
    options.attributes["activity_shard_watermark"] = "1";

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp memory-write fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp memory-write fixture should not emit errors");

    const std::string source = readFile(dir / "memory_write_top.cpp");
    const std::string chunk = readFile(dir / "memory_write_top_commit_chunk_posedge_clk_0.cpp");
    expect(contains(chunk, "dirty_on_commit_ = true; committed_ = true;"),
           "memory-write statement chunks should still invalidate dirty replay after internal state mutation");

    const std::string runner = R"CPP(
#include "memory_write_top.hpp"
#include <cstdint>

int main() {
    SSimTop sim;
    sim.set_clk(0);
    sim.set_wen(0);
    sim.set_addr(2);
    sim.step();
    if (sim.get_q() != 0xA5) {
        return 1;
    }
    sim.set_wen(1);
    sim.set_data(0x3C);
    sim.step();
    sim.set_clk(1);
    sim.step();
    if (sim.get_q() != 0x3C) {
        return 2;
    }
    sim.set_clk(0);
    sim.set_wen(0);
    sim.step();
    if (sim.get_q() != 0x3C) {
        return 3;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "memory_write_top", runner);
}

void testLatchWriteCompileAndRun()
{
    Design design = buildLatchWriteDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "latch_write_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("latch_write_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp latch-write fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp latch-write fixture should not emit errors");

    const std::string source = readFile(dir / "latch_write_top.cpp");

    const std::string runner = R"CPP(
#include "latch_write_top.hpp"

int main() {
    SSimTop sim;
    sim.set_reset(1);
    sim.set_en(1);
    sim.set_d(0xF);
    sim.commit_step();
    if (sim.get_y() != 0) {
        return 1;
    }
    sim.set_reset(0);
    sim.set_en(0);
    sim.set_d(0x3);
    sim.step();
    if (sim.get_y() != 0) {
        return 2;
    }
    sim.set_en(1);
    sim.set_d(0xA);
    sim.step();
    if (sim.get_y() != 0xA) {
        return 3;
    }
    sim.set_d(0x5);
    sim.step();
    if (sim.get_y() != 0x5) {
        return 4;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "latch_write_top", runner);
}

void testLogicBinaryCompileAndRun()
{
    Design design = buildLogicBinaryDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "logic_binary_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("logic_binary_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp logic-and/or fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp logic-and/or fixture should not emit errors");

    const std::string source = readFile(dir / "logic_binary_top.cpp");

    const std::string runner = R"CPP(
#include "logic_binary_top.hpp"

int main() {
    SSimTop sim;
    sim.set_a(0);
    sim.set_b(1);
    sim.step();
    if (sim.get_and_y() != 0 || sim.get_or_y() != 1) {
        return 1;
    }
    sim.set_a(1);
    sim.set_b(1);
    sim.step();
    if (sim.get_and_y() != 1 || sim.get_or_y() != 1) {
        return 2;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "logic_binary_top", runner);
}

void testCaseEqCompileAndRun()
{
    Design design = buildCaseEqDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "caseeq_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("caseeq_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp caseeq fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp caseeq fixture should not emit errors");

    const std::string source = readFile(dir / "caseeq_top.cpp");

    const std::string runner = R"CPP(
#include "caseeq_top.hpp"

int main() {
    SSimTop sim;
    sim.set_a(0xAA);
    sim.set_b(0xAA);
    sim.step();
    if (sim.get_eq_y() != 1 || sim.get_ne_y() != 0) {
        return 1;
    }
    sim.set_b(0x55);
    sim.step();
    if (sim.get_eq_y() != 0 || sim.get_ne_y() != 1) {
        return 2;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "caseeq_top", runner);
}

void testCompareCompileAndRun()
{
    Design design = buildCompareDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "compare_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("compare_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp compare fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp compare fixture should not emit errors");

    const std::string source = readFile(dir / "compare_top.cpp");

    const std::string runner = R"CPP(
#include "compare_top.hpp"

int main() {
    SSimTop sim;
    sim.set_a(3);
    sim.set_b(5);
    sim.step();
    if (sim.get_lt_y() != 1 || sim.get_le_y() != 1 || sim.get_gt_y() != 0 || sim.get_ge_y() != 0 || sim.get_ne_y() != 1) {
        return 1;
    }
    sim.set_a(8);
    sim.set_b(8);
    sim.step();
    if (sim.get_lt_y() != 0 || sim.get_le_y() != 1 || sim.get_gt_y() != 0 || sim.get_ge_y() != 1 || sim.get_ne_y() != 0) {
        return 2;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "compare_top", runner);
}

void testXnorCompileAndRun()
{
    Design design = buildXnorDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "xnor_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("xnor_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp xnor fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp xnor fixture should not emit errors");

    const std::string source = readFile(dir / "xnor_top.cpp");

    const std::string runner = R"CPP(
#include "xnor_top.hpp"

int main() {
    SSimTop sim;
    sim.set_a(0xAA);
    sim.set_b(0x0F);
    sim.step();
    if (sim.get_y() != static_cast<unsigned char>(~(0xAA ^ 0x0F))) {
        return 1;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "xnor_top", runner);
}

void testSliceStaticCompileAndRun()
{
    Design design = buildSliceStaticDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "slice_static_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("slice_static_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp slice-static fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp slice-static fixture should not emit errors");

    const std::string source = readFile(dir / "slice_static_top.cpp");

    const std::string runner = R"CPP(
#include "slice_static_top.hpp"

int main() {
    SSimTop sim;
    sim.set_a(0xAB);
    sim.step();
    if (sim.get_y() != 0xA) {
        return 1;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "slice_static_top", runner);
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

    const std::string source = readFile(dir / "slice_dynamic_top.cpp");

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

void testWideBitDynamicSliceCompileAndRun()
{
    Design design = buildWideBitSliceDynamicDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "wide_bit_slice_dynamic_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("wide_bit_slice_dynamic_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp wide-bit slice-dynamic fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp wide-bit slice-dynamic fixture should not emit errors");

    const std::string header = readFile(dir / "wide_bit_slice_dynamic_top.hpp");
    const std::string source = readFile(dir / "wide_bit_slice_dynamic_top.cpp");

    const std::string runner = R"CPP(
#include "wide_bit_slice_dynamic_top.hpp"
#include <cstdint>
#include <vector>

int main() {
    SSimTop sim;
    sim.set_in(std::vector<std::uint64_t>{0x8000000000000001ULL, 0x5ULL, 0x2ULL});

    sim.set_index(0);
    sim.step();
    if (sim.get_out() != 1) {
        return 1;
    }

    sim.set_index(63);
    sim.step();
    if (sim.get_out() != 1) {
        return 2;
    }

    sim.set_index(64);
    sim.step();
    if (sim.get_out() != 1) {
        return 3;
    }

    sim.set_index(65);
    sim.step();
    if (sim.get_out() != 0) {
        return 4;
    }

    sim.set_index(129);
    sim.step();
    if (sim.get_out() != 1) {
        return 5;
    }

    sim.set_index(130);
    sim.step();
    if (sim.get_out() != 0) {
        return 6;
    }

    return 0;
}
)CPP";

    compileAndRunHarness(dir, "wide_bit_slice_dynamic_top", runner);
}

void testWideNibbleDynamicSliceCompileAndRun()
{
    Design design = buildWideNibbleSliceDynamicDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "wide_nibble_slice_dynamic_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("wide_nibble_slice_dynamic_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp wide-nibble slice-dynamic fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp wide-nibble slice-dynamic fixture should not emit errors");

    const std::string header = readFile(dir / "wide_nibble_slice_dynamic_top.hpp");
    const std::string source = readFile(dir / "wide_nibble_slice_dynamic_top.cpp");

    const std::string runner = R"CPP(
#include "wide_nibble_slice_dynamic_top.hpp"
#include <cstdint>
#include <vector>

int main() {
    SSimTop sim;
    sim.set_in(std::vector<std::uint64_t>{0x8000000000000001ULL, 0x5ULL, 0x2ULL});

    sim.set_index(0);
    sim.step();
    if (sim.get_out() != 0x1) {
        return 1;
    }

    sim.set_index(63);
    sim.step();
    if (sim.get_out() != 0xB) {
        return 2;
    }

    sim.set_index(64);
    sim.step();
    if (sim.get_out() != 0x5) {
        return 3;
    }

    sim.set_index(128);
    sim.step();
    if (sim.get_out() != 0x2) {
        return 4;
    }

    sim.set_index(129);
    sim.step();
    if (sim.get_out() != 0x1) {
        return 5;
    }

    sim.set_index(130);
    sim.step();
    if (sim.get_out() != 0x0) {
        return 6;
    }

    return 0;
}
)CPP";

    compileAndRunHarness(dir, "wide_nibble_slice_dynamic_top", runner);
}

void testWideVectorDynamicSliceCompileAndRun()
{
    Design design = buildWideVectorSliceDynamicDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "wide_vector_slice_dynamic_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("wide_vector_slice_dynamic_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp wide-vector slice-dynamic fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp wide-vector slice-dynamic fixture should not emit errors");

    const std::string header = readFile(dir / "wide_vector_slice_dynamic_top_internal.hpp");
    const std::string runner = R"CPP(
#include "wide_vector_slice_dynamic_top.hpp"
#include <cstdint>
#include <vector>

int main() {
    SSimTop sim;
    sim.set_in(std::vector<std::uint64_t>{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL, 0x2ULL});

    sim.set_index(5);
    sim.step();
    const auto out = sim.get_out();
    if (out.size() != 2) {
        return 1;
    }
    if (out[0] != 0x80091A2B3C4D5E6FULL) {
        return 2;
    }
    if (out[1] != 0x10ULL) {
        return 3;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "wide_vector_slice_dynamic_top", runner);
}

void testWideStaticSliceCompileAndRun()
{
    Design design = buildWideStaticSliceDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "wide_static_slice_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("wide_static_slice_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp wide-static slice fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp wide-static slice fixture should not emit errors");

    const std::string header = readFile(dir / "wide_static_slice_top_internal.hpp");

    const std::string runner = R"CPP(
#include "wide_static_slice_top.hpp"
#include <cstdint>
#include <vector>

int main() {
    SSimTop sim;
    sim.set_in(std::vector<std::uint64_t>{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL, 0x2ULL});
    sim.step();
    const auto out = sim.get_out();
    if (out.size() != 2) {
        return 1;
    }
    if (out[0] != 0x80091A2B3C4D5E6FULL) {
        return 2;
    }
    if (out[1] != 0x10ULL) {
        return 3;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "wide_static_slice_top", runner);
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
    expect(contains(header, "std::vector<std::uint64_t> input_a_ = std::vector<std::uint64_t>(2U, 0ULL)"),
           "wide-vector fixture should size vector inputs to their emitted word count");
    expect(contains(header, "std::vector<std::uint64_t> output_y_ = std::vector<std::uint64_t>(2U, 0ULL)"),
           "wide-vector fixture should size vector outputs to their emitted word count");

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

void testWideConcatCompileAndRun()
{
    Design design = buildWideConcatDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "wide_concat_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("wide_concat_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp wide-concat fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp wide-concat fixture should not emit errors");

    const std::string header = readFile(dir / "wide_concat_top_internal.hpp");

    const std::string runner = R"CPP(
#include "wide_concat_top.hpp"
#include <vector>

int main() {
    SSimTop sim;
    sim.set_msb(1);
    sim.set_rest(std::vector<std::uint64_t>{0x0123456789ABCDEFULL, 0x5ULL});
    sim.step();
    const auto out = sim.get_y();
    if (out.size() != 2) {
        return 1;
    }
    if (out[0] != 0x0123456789ABCDEFULL) {
        return 2;
    }
    if (out[1] != (0x5ULL | (1ULL << 36))) {
        return 3;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "wide_concat_top", runner);
}

void testWideBitwiseCompileAndRun()
{
    Design design = buildWideBitwiseDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "wide_bitwise_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("wide_bitwise_top");
    options.topOverrides = {"top"};
    options.attributes["behavior_shard_max_bytes"] = "256";
    options.attributes["activity_shard_watermark"] = "1";

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp wide-bitwise fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp wide-bitwise fixture should not emit errors");

    const std::string header = readFile(dir / "wide_bitwise_top_internal.hpp");
    expect(contains(header, "inline void wolvrix_gsim_bitwise_and_into"),
           "wide-bitwise fixture should emit allocation-free AND helper");
    expect(contains(header, "inline void wolvrix_gsim_bitwise_or_into"),
           "wide-bitwise fixture should emit allocation-free OR helper");
    expect(contains(header, "inline void wolvrix_gsim_bitwise_xor_into"),
           "wide-bitwise fixture should emit allocation-free XOR helper");
    expect(contains(header, "inline void wolvrix_gsim_bitwise_not_into"),
           "wide-bitwise fixture should emit allocation-free NOT helper");
    std::string source;
    for (std::size_t i = 0;; ++i)
    {
        const auto shardPath = dir / ("wide_bitwise_top_sched_" + std::to_string(i) + ".cpp");
        if (!std::filesystem::exists(shardPath))
        {
            break;
        }
        source += readFile(shardPath);
    }
    expect(!source.empty(), "wide-bitwise fixture should emit sharded sched files");
    expect(contains(source, "wolvrix_gsim_bitwise_and_into("),
           "sharded wide-bitwise fixture should lower AND into existing storage");
    expect(contains(source, "wolvrix_gsim_bitwise_or_into("),
           "sharded wide-bitwise fixture should lower OR into existing storage");
    expect(contains(source, "wolvrix_gsim_bitwise_xor_into("),
           "sharded wide-bitwise fixture should lower XOR into existing storage");
    expect(contains(source, "wolvrix_gsim_bitwise_not_into("),
           "sharded wide-bitwise fixture should lower NOT into existing storage");

    const std::string runner = R"CPP(
#include "wide_bitwise_top.hpp"
#include <cstdint>
#include <vector>

int main() {
    SSimTop sim;
    sim.set_a(std::vector<std::uint64_t>{0x00FF00FF00FF00FFULL, 0xFULL});
    sim.set_b(std::vector<std::uint64_t>{0x0F0F0F0F0F0F0F0FULL, 0x3ULL});
    sim.step();
    const auto andY = sim.get_and_y();
    const auto orY = sim.get_or_y();
    const auto xorY = sim.get_xor_y();
    const auto notY = sim.get_not_y();
    if (andY.size() != 2 || orY.size() != 2 || xorY.size() != 2 || notY.size() != 2) {
        return 1;
    }
    if (andY[0] != 0x000F000F000F000FULL || andY[1] != 0x3ULL) {
        return 2;
    }
    if (orY[0] != 0x0FFF0FFF0FFF0FFFULL || orY[1] != 0xFULL) {
        return 3;
    }
    if (xorY[0] != 0x0FF00FF00FF00FF0ULL || xorY[1] != 0xCULL) {
        return 4;
    }
    if (notY[0] != 0xFF00FF00FF00FF00ULL || notY[1] != 0xFFFFFFFF0ULL) {
        return 5;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "wide_bitwise_top", runner);
}

void testWideAdderSplitCompileAndRun()
{
    Design design = buildWideAdderSplitDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "wide_adder_split_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("wide_adder_split_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp wide-adder split fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp wide-adder split fixture should not emit errors");

    const std::string header = readFile(dir / "wide_adder_split_top_internal.hpp");

    const std::string runner = R"CPP(
#include "wide_adder_split_top.hpp"
#include <cstdint>
#include <vector>

int main() {
    SSimTop sim;
    sim.set_a(std::vector<std::uint64_t>{~0ULL, 0xFFFFFFFFFULL});
    sim.set_b(std::vector<std::uint64_t>{0ULL, 0ULL});
    sim.set_cin(1);
    sim.step();
    const auto sum = sim.get_sum();
    if (sum.size() != 2) {
        return 1;
    }
    if (sum[0] != 0ULL || sum[1] != 0ULL) {
        return 2;
    }
    if (sim.get_cout() != 1) {
        return 3;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "wide_adder_split_top", runner);
}

void testReplicateCompileAndRun()
{
    Design design = buildReplicateDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "replicate_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("replicate_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp replicate fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp replicate fixture should not emit errors");

    const std::string runner = R"CPP(
#include "replicate_top.hpp"

int main() {
    SSimTop sim;
    sim.set_in(0x15);
    sim.step();
    if (sim.get_y() != 0x15AD6B5) {
        return 1;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "replicate_top", runner);
}

void testWideReplicateCompileAndRun()
{
    Design design = buildWideReplicateDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "wide_replicate_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("wide_replicate_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp wide-replicate fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp wide-replicate fixture should not emit errors");

    const std::string header = readFile(dir / "wide_replicate_top_internal.hpp");

    const std::string runner = R"CPP(
#include "wide_replicate_top.hpp"
#include <cstdint>
#include <vector>

int main() {
    SSimTop sim;
    sim.set_in(std::vector<std::uint64_t>{0x0123456789ABCDEFULL, 0x21ULL});
    sim.step();
    const auto out = sim.get_y();
    if (out.size() != 3) {
        return 1;
    }
    if (out[0] != 0x0123456789ABCDEFULL) {
        return 2;
    }
    if (out[1] != 0x48D159E26AF37BE1ULL) {
        return 3;
    }
    if (out[2] != 0x840ULL) {
        return 4;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "wide_replicate_top", runner);
}

void testReduceCompileAndRun()
{
    Design design = buildReduceDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "reduce_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("reduce_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp reduce fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp reduce fixture should not emit errors");

    const std::string runner = R"CPP(
#include "reduce_top.hpp"

int main() {
    SSimTop sim;
    sim.set_in(0xFF);
    sim.step();
    if (sim.get_and_y() != 1 || sim.get_or_y() != 1 || sim.get_xor_y() != 0) {
        return 1;
    }
    sim.set_in(0x01);
    sim.step();
    if (sim.get_and_y() != 0 || sim.get_or_y() != 1 || sim.get_xor_y() != 1) {
        return 2;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "reduce_top", runner);
}

void testWideReduceCompileAndRun()
{
    Design design = buildWideReduceDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "wide_reduce_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("wide_reduce_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp wide-reduce fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp wide-reduce fixture should not emit errors");

    const std::string header = readFile(dir / "wide_reduce_top_internal.hpp");

    const std::string runner = R"CPP(
#include "wide_reduce_top.hpp"
#include <cstdint>
#include <vector>

int main() {
    SSimTop sim;
    sim.set_in(std::vector<std::uint64_t>{~0ULL, ~0ULL, 0x3ULL});
    sim.step();
    if (sim.get_and_y() != 1 || sim.get_or_y() != 1 || sim.get_xor_y() != 0) {
        return 1;
    }
    sim.set_in(std::vector<std::uint64_t>{0ULL, 0ULL, 0ULL});
    sim.step();
    if (sim.get_and_y() != 0 || sim.get_or_y() != 0 || sim.get_xor_y() != 0) {
        return 2;
    }
    sim.set_in(std::vector<std::uint64_t>{1ULL, 0ULL, 0ULL});
    sim.step();
    if (sim.get_and_y() != 0 || sim.get_or_y() != 1 || sim.get_xor_y() != 1) {
        return 3;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "wide_reduce_top", runner);
}

void testWideMaskedRegisterCompileAndRun()
{
    Design design = buildWideMaskedRegisterDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "wide_masked_register_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("wide_masked_register_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp wide masked-register fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp wide masked-register fixture should not emit errors");

    const std::string header = readFile(dir / "wide_masked_register_top_internal.hpp");

    const std::string runner = R"CPP(
#include "wide_masked_register_top.hpp"
#include <cstdint>
#include <vector>

int main() {
    SSimTop sim;
    sim.set_clk(0);
    sim.set_d(std::vector<std::uint64_t>{0x0123456789ABCDEFULL, 0x000000000000001FULL, 0x0ULL});
    sim.set_mask(std::vector<std::uint64_t>{~0ULL, 0xFULL, 0x0ULL});
    sim.step();
    sim.set_clk(1);
    sim.step();
    const auto out = sim.get_q();
    if (out.size() != 3) {
        return 1;
    }
    if (out[0] != 0x0123456789ABCDEFULL || out[1] != 0xFULL || out[2] != 0x0ULL) {
        return 2;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "wide_masked_register_top", runner);
}

void testWideFullMaskRegisterCompileAndRun()
{
    Design design = buildWideFullMaskRegisterDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "wide_fullmask_register_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("wide_fullmask_register_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp wide full-mask register fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp wide full-mask register fixture should not emit errors");
    bool sawOptimizedWideWriteback = false;
    const auto manifestPath = dir / "wide_fullmask_register_top.manifest";
    if (std::filesystem::exists(manifestPath))
    {
        std::ifstream manifest(manifestPath);
        std::string rel;
        while (std::getline(manifest, rel))
        {
            if (!rel.ends_with(".cpp"))
            {
                continue;
            }
            const std::string text = readFile(dir / rel);
            if (text.find("std::move(next_") != std::string::npos ||
                text.find("state_->stateVec[") != std::string::npos)
            {
                sawOptimizedWideWriteback = true;
                break;
            }
        }
    }
    expect(sawOptimizedWideWriteback,
           "EmitGsimCpp wide full-mask register fixture should optimize wide next-state writeback");

    const std::string runner = R"CPP(
#include "wide_fullmask_register_top.hpp"
#include <cstdint>
#include <vector>

int main() {
    SSimTop sim;
    sim.set_clk(0);
    sim.set_d(std::vector<std::uint64_t>{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL, 0x3ULL});
    sim.step();
    sim.set_clk(1);
    sim.step();
    const auto out = sim.get_q();
    if (out.size() != 3) {
        return 1;
    }
    if (out[0] != 0x0123456789ABCDEFULL || out[1] != 0xFEDCBA9876543210ULL || out[2] != 0x3ULL) {
        return 2;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "wide_fullmask_register_top", runner);
}

void testWideUnknownConstantCompileAndRun()
{
    Design design = buildWideUnknownConstantDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "wide_unknown_constant_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("wide_unknown_constant_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp wide unknown-constant fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp wide unknown-constant fixture should not emit errors");

    const std::string runner = R"CPP(
#include "wide_unknown_constant_top.hpp"
#include <cstdint>

int main() {
    SSimTop sim;
    sim.step();
    const auto out = sim.get_y();
    if (out.size() != 3) {
        return 1;
    }
    if (out[0] != 0x1ULL || out[1] != 0x0ULL || out[2] != 0x3ULL) {
        return 2;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "wide_unknown_constant_top", runner);
}

void testDualEdgeClockMetadataDeduplicatesPrevClockState()
{
    Design design = buildDualEdgeClockDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "dual_edge_clock_compile_only";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("dual_edge_clock_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp dual-edge fixture should still emit");
    expect(!diags.hasError(), "EmitGsimCpp dual-edge fixture should not emit diagnostics");

    const std::string header = readFile(dir / "dual_edge_clock_top.hpp");
    expect(countOccurrences(header, "bool prev_clk_ = false;") == 1,
           "dual-edge fixture should deduplicate prev clock storage by resolved clock name");
    expect(!contains(header, "supports only one clock domain"),
           "dual-edge fixture should no longer reject same-signal multi-edge domains outright");

    const char *compiler = std::getenv("CXX");
    const std::string compileCmd =
        std::string((compiler && *compiler) ? compiler : "c++") +
        " -std=c++20 -fsyntax-only -I " + dir.string() + " " + (dir / "dual_edge_clock_top.cpp").string();
    expect(std::system(compileCmd.c_str()) == 0,
           "dual-edge fixture should remain syntactically compilable after prev-clock deduplication");
}

void testMediumGraphsEnableSharding()
{
    Design design = buildMediumShardedDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "medium_sharded_emit";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("medium_sharded_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp medium sharded fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp medium sharded fixture should not emit diagnostics");
    expect(std::filesystem::exists(dir / "medium_sharded_top_sched_0.cpp"),
           "medium-size graphs should emit at least one sched shard once sharding is enabled");
    const std::string source = readFile(dir / "medium_sharded_top.cpp");
    expect(contains(source, "void SSimTop::replay_dirty_input_shards() {"),
           "medium-size sharded fixture should emit replay_dirty_input_shards helper");
}

void testDirtyReplayEdgeWithoutCommitRefreshesOutputs()
{
    Design design = buildShardedNoCommitInputOutputDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "dirty_replay_no_commit_refresh";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("dirty_replay_no_commit_top");
    options.topOverrides = {"top"};
    options.attributes["behavior_shard_max_bytes"] = "512";
    options.attributes["activity_shard_watermark"] = "1";

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp dirty-replay no-commit fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp dirty-replay no-commit fixture should not emit errors");

    const std::string source = readFile(dir / "dirty_replay_no_commit_top.cpp");
    expect(contains(source, "bool dirty_replayed_ = false;"),
           "sharded commit_step should track edge-local dirty replay");
    expect(contains(source, "non_clock_inputs_dirty_ || dirty_replayed_"),
           "sharded commit_step should settle after dirty replay even when no commit happens");
    expect(!contains(source, "if (sequential_edge_pending_) {\n            settle();"),
           "sharded dirty replay should not run a full settle before sampling sequential edge conditions");
    expect(contains(source, "if (non_clock_inputs_dirty_ && !dirty_replayed_)"),
           "sharded commit_step should not replay dirty shards between domains within one edge snapshot");

    const std::string runner = R"CPP(
#include "dirty_replay_no_commit_top.hpp"
#include <cstdint>

int main() {
    SSimTop sim;
    sim.set_a(0);
    sim.set_clk(0);
    sim.set_reset(0);
    sim.settle();
    if (sim.get_y() != 0) {
        return 1;
    }
    sim.set_a(1);
    sim.set_clk(1);
    sim.commit_step();
    if (sim.get_difftest__DOT__step() != 0) {
        return 2;
    }
    if (sim.get_y() != 1) {
        return 3;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "dirty_replay_no_commit_top", runner);
}

void testReplayDirtyInputShardsSkipsInputIndependentShards()
{
    Design design = buildSelectiveReplayShardedDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "selective_replay_sharded_emit";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("selective_replay_top");
    options.topOverrides = {"top"};
    options.attributes["behavior_shard_max_bytes"] = "512";
    options.attributes["activity_shard_watermark"] = "1";

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp selective-replay fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp selective-replay fixture should not emit diagnostics");

    const std::string source = readFile(dir / "selective_replay_top.cpp");
    const std::string marker = "void SSimTop::replay_dirty_input_shards() {";
    const auto markerPos = source.find(marker);
    expect(markerPos != std::string::npos,
           "selective-replay fixture should emit replay_dirty_input_shards helper");
    const auto endPos = source.find("}\n\nvoid SSimTop::commit_step()", markerPos);
    expect(endPos != std::string::npos,
           "selective-replay fixture should place replay helper before commit_step");
    const std::string replayBody = source.substr(markerPos, endPos - markerPos);

    std::size_t shardCount = 0;
    while (std::filesystem::exists(dir / ("selective_replay_top_sched_" + std::to_string(shardCount) + ".cpp")))
    {
        ++shardCount;
    }
    expect(shardCount > 1, "selective-replay fixture should emit multiple sched shards");

    std::size_t replayCalls = 0;
    std::size_t searchPos = 0;
    while ((searchPos = replayBody.find("sched_", searchPos)) != std::string::npos)
    {
        ++replayCalls;
        searchPos += 6;
    }

    expect(replayCalls > 0, "selective-replay fixture should replay at least one shard");
    expect(replayCalls < shardCount,
           "selective-replay fixture should skip shards that never depend on dirty non-clock inputs");
    const std::string header = readFile(dir / "selective_replay_top.hpp");
    expect(contains(header, "std::vector<std::uint64_t> active_shard_words_"),
           "sharded runtime should carry packed active-shard worklist words");
    expect(contains(header, "std::vector<std::uint32_t> active_word_queue_"),
           "sharded runtime should carry an active-word worklist queue");
    expect(contains(source, "void SSimTop::activate_shards(const std::uint32_t* indices, std::size_t count)"),
           "sharded runtime should expose compact active-shard activation helper");
    expect(contains(source, "while (active_cursor_ < active_word_queue_.size())"),
           "settle should drain the active-word worklist instead of always replaying every shard");
    expect(contains(source, "kShardSuccessorMask"),
           "settle should enqueue shard successors from generated word-mask fanout metadata");
    expect(contains(source, "activate_shard_mask"),
           "settle should enqueue cross-word successors as packed active-word masks");
    expect(contains(source, "active_bits_ |= kShardSuccessorMask"),
           "same-word successor activation should stay in the local active-word bitmap");
}


void testActiveWorklistSkipsIndependentBranchAndRunsConvergentFanout()
{
    Design design = buildConvergentActiveReplayDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "active_worklist_convergent_runtime";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("active_worklist_top");
    options.topOverrides = {"top"};
    options.attributes["behavior_shard_max_bytes"] = "64";
    options.attributes["activity_shard_watermark"] = "1";

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp convergent active-worklist fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp convergent active-worklist fixture should not emit diagnostics");

    std::size_t shardCount = 0;
    while (std::filesystem::exists(dir / ("active_worklist_top_sched_" + std::to_string(shardCount) + ".cpp")))
    {
        ++shardCount;
    }
    expect(shardCount > 4, "convergent active-worklist fixture should emit enough shards to prove branch selectivity");

    const std::string source = readFile(dir / "active_worklist_top.cpp");
    expect(contains(source, "switch (active_shard_)"),
           "active-worklist fixture should use switch dispatch instead of a linear active-shard scan");
    expect(contains(source, "kShardSuccessorMask"),
           "active-worklist fixture should emit packed successor fanout for convergent dependencies");

    instrumentSchedCounters(dir, "active_worklist_top", shardCount);

    const std::string runner = std::string(R"CPP(
#include "active_worklist_top.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>

int wolvrix_test_sched_counts[)CPP") + std::to_string(shardCount) + R"CPP(] = {};

static void clear_counts() {
    std::fill(std::begin(wolvrix_test_sched_counts), std::end(wolvrix_test_sched_counts), 0);
}

int main() {
    SSimTop sim;
    sim.set_a(0);
    sim.set_b(0);
    sim.settle();
    if (sim.get_y() != 0) {
        return 1;
    }

    clear_counts();
    sim.set_a(1);
    sim.settle();
    if (sim.get_y() != 1) {
        return 2;
    }
    int counts_after_a[)CPP" + std::to_string(shardCount) + R"CPP(] = {};
    for (std::size_t i = 0; i < )CPP" + std::to_string(shardCount) + R"CPP(; ++i) {
        counts_after_a[i] = wolvrix_test_sched_counts[i];
    }

    clear_counts();
    sim.set_b(1);
    sim.settle();
    if (sim.get_y() != 0) {
        return 3;
    }

    bool a_changed_executed_any = false;
    bool b_changed_executed_any = false;
    bool b_only_shard_skipped_by_a = false;
    bool shared_downstream_executed_by_both = false;
    for (std::size_t i = 0; i < )CPP" + std::to_string(shardCount) + R"CPP(; ++i) {
        const bool ran_for_a = counts_after_a[i] != 0;
        const bool ran_for_b = wolvrix_test_sched_counts[i] != 0;
        a_changed_executed_any = a_changed_executed_any || ran_for_a;
        b_changed_executed_any = b_changed_executed_any || ran_for_b;
        b_only_shard_skipped_by_a = b_only_shard_skipped_by_a || (!ran_for_a && ran_for_b);
        shared_downstream_executed_by_both = shared_downstream_executed_by_both || (ran_for_a && ran_for_b);
    }
    if (!a_changed_executed_any || !b_changed_executed_any) {
        return 4;
    }
    if (!b_only_shard_skipped_by_a) {
        return 5;
    }
    if (!shared_downstream_executed_by_both) {
        return 6;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "active_worklist_top", runner);
}

void testShiftCompileAndRun()
{
    Design design = buildShiftDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "shift_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("shift_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp shift fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp shift fixture should not emit errors");

    const std::string runner = R"CPP(
#include "shift_top.hpp"

int main() {
    SSimTop sim;
    sim.set_in(0x81);
    sim.set_amount(1);
    sim.step();
    if (sim.get_shl_y() != 0x02 || sim.get_lshr_y() != 0x40) {
        return 1;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "shift_top", runner);
}

void testWideShiftCompileAndRun()
{
    Design design = buildWideShiftDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "wide_shift_compile_run";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("wide_shift_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp wide-shift fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp wide-shift fixture should not emit errors");

    const std::string header = readFile(dir / "wide_shift_top_internal.hpp");

    const std::string runner = R"CPP(
#include "wide_shift_top.hpp"
#include <cstdint>
#include <vector>

int main() {
    SSimTop sim;
    sim.set_in(std::vector<std::uint64_t>{0x0123456789ABCDEFULL, 0x2ULL, 0x0ULL});
    sim.set_amount(4);
    sim.step();
    auto shl = sim.get_shl_y();
    auto lshr = sim.get_lshr_y();
    if (shl.size() != 3 || lshr.size() != 3) {
        return 1;
    }
    if (shl[0] != 0x123456789ABCDEF0ULL || shl[1] != 0x20ULL || shl[2] != 0x0ULL) {
        return 2;
    }
    if (lshr[0] != 0x20123456789ABCDEULL || lshr[1] != 0x0ULL || lshr[2] != 0x0ULL) {
        return 3;
    }
    sim.set_amount(80);
    sim.step();
    shl = sim.get_shl_y();
    lshr = sim.get_lshr_y();
    if (shl[0] != 0ULL || shl[1] != 0x456789ABCDEF0000ULL || shl[2] != 0x3ULL) {
        return 4;
    }
    if (lshr[0] != 0ULL || lshr[1] != 0ULL || lshr[2] != 0ULL) {
        return 5;
    }

    sim.set_in(std::vector<std::uint64_t>{0x0ULL, 0x0ULL, 0x2ULL});
    sim.set_amount(4);
    sim.step();
    auto ashr = sim.get_ashr_y();
    if (ashr.size() != 3) {
        return 6;
    }
    if (ashr[0] != 0ULL || ashr[1] != 0xE000000000000000ULL || ashr[2] != 0x3ULL) {
        return 7;
    }
    sim.set_amount(80);
    sim.step();
    ashr = sim.get_ashr_y();
    if (ashr[0] != 0xFFFE000000000000ULL || ashr[1] != 0xFFFFFFFFFFFFFFFFULL || ashr[2] != 0x3ULL) {
        return 8;
    }
    return 0;
}
)CPP";

    compileAndRunHarness(dir, "wide_shift_top", runner);
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
    options.attributes["behavior_shard_max_bytes"] = "512";
    options.attributes["activity_shard_watermark"] = "1";

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp pipeline fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp pipeline fixture should not emit errors");

    std::string generatedSources;
    for (const auto& entry : std::filesystem::directory_iterator(dir))
    {
        if (entry.path().extension() == ".cpp")
        {
            generatedSources += readFile(entry.path());
        }
    }
    expect(contains(generatedSources, "kTouchedStateFirstShards"),
           "activity watermark should activate only shards touched by changed register state");
    expect(contains(generatedSources, "activate_shards(kTouchedStateFirstShards"),
           "activity watermark should use compact touched-state activation tables");
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

void testClockFallbackUsesConsistentPrevClockName()
{
    Design design = buildStatefulOutputDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "clock_fallback_prev_name";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("clock_fallback_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp fallback-clock fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp fallback-clock fixture should not emit errors");

    const std::string header = readFile(dir / "clock_fallback_top.hpp");
    expect(!contains(header, "prev_clock_"),
           "fallback-clock fixture should not emit unresolved prev_clock_ references");
    expect(contains(header, "prev_clk_"),
           "fallback-clock fixture should consistently use prev_clk_ when input clock carrier is clk");
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

void testClockAliasFallbackUsesOnePrevClockStateName()
{
    Design design = buildClockAliasWithoutMaterializedExprDesign();
    runGsim(design, "top");

    const auto dir = artifactRoot() / "clock_alias_prev_name";
    cleanDir(dir);

    EmitDiagnostics diags;
    EmitGsimCpp emitter(&diags);
    EmitOptions options;
    options.outputDir = dir.string();
    options.outputFilename = std::string("clock_alias_top");
    options.topOverrides = {"top"};

    const EmitResult result = emitter.emit(design, options);
    expect(result.success, "EmitGsimCpp clock-alias fixture should succeed");
    expect(!diags.hasError(), "EmitGsimCpp clock-alias fixture should not emit errors");

    const std::string header = readFile(dir / "clock_alias_top.hpp");
    expect(!contains(header, "prev_clock_alias_"),
           "clock-alias fallback should not emit an unresolved prev_clock_alias_ state name");
    expect(contains(header, "prev_clk_"),
           "clock-alias fallback should reuse the resolved input clk storage name");

    const std::string runner = R"CPP(
#include "clock_alias_top.hpp"
#include <cstdint>

int main() {
    SSimTop sim;
    sim.set_d(0);
    sim.set_clk(0);
    sim.step();
    sim.set_d(1);
    sim.set_clk(1);
    sim.step();
    return sim.get_q() == 1 ? 0 : 1;
}
)CPP";

    compileAndRunHarness(dir, "clock_alias_top", runner);
}

} // namespace

int main()
{
    try
    {
        testHappyPathAfterRunningGsim();
        testDifftestCompatibilityAccessorsUseTopLevelPorts();
        testFailureWithoutPriorMetadata();
        testFailureOnPlaceholderContract();
        testFailureOnNamespacePathMismatch();
        testFailureOnStaleMetadataAfterMutation();
        testFailureOnStaleMetadataAfterDestructiveMutation();
        testGraphOnlyAndMultiHopTargetSelectionConsistency();
        testCrossRootInstancePathsStayDistinct();
        testSingleClockRuntimeCompileAndRun();
        testResetCompatibilitySetterDrivesTopLevelResetPort();
        testDpicImportNoOpCompileAndRun();
        testDpicCallCompileAndRun();
        testNoOutputShardedDpicConditionReplaysDirtyInputs();
        testDpicDirtyReplayIncludesProducerClosure();
        testShardedDirtyReplaySettlesLatchBeforeDpicCondition();
        testNoDiffGuardedDpicCallsCompileOut();
        testNoDiffGuardedValueDpicCallsDefaultOut();
        testNoDiffGuardedMultiResultDpicCallsDefaultOut();
        testDpicReturnValueFeedsSequentialWrite();
        testDpicOutputArgFeedsSequentialWrite();
        testDpicJtagTickMultiResultCallOnceAndConditionGated();
        testDpicJtagTickPreOnlySequentialClockDeclared();
        testDerivedClockEdgesSeePriorDomainCommits();
        testSettlePreservesDerivedClockInputEdges();
        testDpicSamplesPostSequentialSettleState();
        testLatchReadNoOpCompileAndRun();
        testLatchWriteCompileAndRun();
        testMemoryReadCompileAndRun();
        testMemoryWriteCompileAndRun();
        testLogicBinaryCompileAndRun();
        testCaseEqCompileAndRun();
        testCompareCompileAndRun();
        testXnorCompileAndRun();
        testSliceStaticCompileAndRun();
        testConcatCompileAndRun();
        testEqCompileAndRun();
        testBitwiseNotMasksToDeclaredWidth();
        testDynamicSliceCompileAndRun();
        testWideBitDynamicSliceCompileAndRun();
        testWideNibbleDynamicSliceCompileAndRun();
        testWideVectorDynamicSliceCompileAndRun();
        testWideStaticSliceCompileAndRun();
        testWideVectorPortsInitializeAndCompile();
        testWideConcatCompileAndRun();
        testWideBitwiseCompileAndRun();
        testWideAdderSplitCompileAndRun();
        testReplicateCompileAndRun();
        testWideReplicateCompileAndRun();
        testReduceCompileAndRun();
        testWideReduceCompileAndRun();
        testWideMaskedRegisterCompileAndRun();
        testWideFullMaskRegisterCompileAndRun();
        testWideUnknownConstantCompileAndRun();
        testDualEdgeClockMetadataDeduplicatesPrevClockState();
        testMediumGraphsEnableSharding();
        testDirtyReplayEdgeWithoutCommitRefreshesOutputs();
        testReplayDirtyInputShardsSkipsInputIndependentShards();
        testActiveWorklistSkipsIndependentBranchAndRunsConvergentFanout();
        testShiftCompileAndRun();
        testWideShiftCompileAndRun();
        testRegisterPipelineUsesNonBlockingSemantics();
        testKeyBitClockCarrierDoesNotEmitMissingInputClockAlias();
        testClockFallbackUsesConsistentPrevClockName();
        testEmitMetadataToggleSkipsLargeMetadataPayload();
        testEdgeWithoutCommitDoesNotAdvanceStep();
        testClockAliasFallbackUsesOnePrevClockStateName();
    }
    catch (const std::exception &ex)
    {
        return fail(ex.what());
    }
    return 0;
}
