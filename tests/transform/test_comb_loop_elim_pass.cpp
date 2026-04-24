#include "core/grh.hpp"
#include "transform/comb_loop_elim.hpp"
#include "core/transform.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace wolvrix::lib::transform;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[comb-loop-elim-tests] " << message << '\n';
        return 1;
    }

    bool hasCombLoopWarning(const PassDiagnostics &diags)
    {
        for (const auto &msg : diags.messages())
        {
            if (msg.passName == "comb-loop-elim" &&
                msg.message.find("comb loop detected") != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    std::size_t countOpKind(const wolvrix::lib::grh::Graph &graph,
                            wolvrix::lib::grh::OperationKind kind)
    {
        std::size_t count = 0;
        for (const auto &opId : graph.operations())
        {
            if (graph.getOperation(opId).kind() == kind)
            {
                ++count;
            }
        }
        return count;
    }

    wolvrix::lib::grh::ValueId makeValue(wolvrix::lib::grh::Graph &graph,
                                         const std::string &name,
                                         int32_t width)
    {
        const wolvrix::lib::grh::SymbolId sym = graph.internSymbol(name);
        return graph.createValue(sym, width, false);
    }

    wolvrix::lib::grh::OperationId makeSliceStatic(wolvrix::lib::grh::Graph &graph,
                                                   wolvrix::lib::grh::ValueId base,
                                                   wolvrix::lib::grh::ValueId result,
                                                   int64_t low,
                                                   int64_t high,
                                                   const std::string &name)
    {
        const wolvrix::lib::grh::SymbolId sym = graph.internSymbol(name);
        const wolvrix::lib::grh::OperationId op =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kSliceStatic, sym);
        graph.addOperand(op, base);
        graph.addResult(op, result);
        graph.setAttr(op, "sliceStart", low);
        graph.setAttr(op, "sliceEnd", high);
        return op;
    }

    wolvrix::lib::grh::OperationId makeConcat(wolvrix::lib::grh::Graph &graph,
                                              const std::string &name,
                                              const std::vector<wolvrix::lib::grh::ValueId> &operands,
                                              wolvrix::lib::grh::ValueId result)
    {
        const wolvrix::lib::grh::SymbolId sym = graph.internSymbol(name);
        const wolvrix::lib::grh::OperationId op =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kConcat, sym);
        for (const auto operand : operands)
        {
            graph.addOperand(op, operand);
        }
        graph.addResult(op, result);
        return op;
    }

    wolvrix::lib::grh::ValueId makeConst(wolvrix::lib::grh::Graph &graph,
                                         const std::string &valueName,
                                         const std::string &opName,
                                         int32_t width,
                                         const std::string &literal)
    {
        const auto value = graph.createValue(graph.internSymbol(valueName), width, false);
        const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant,
                                              graph.internSymbol(opName));
        graph.addResult(op, value);
        graph.setAttr(op, "constValue", literal);
        return value;
    }

    wolvrix::lib::grh::OperationId makeSliceDynamic(wolvrix::lib::grh::Graph &graph,
                                                    wolvrix::lib::grh::ValueId base,
                                                    wolvrix::lib::grh::ValueId index,
                                                    wolvrix::lib::grh::ValueId result,
                                                    int64_t width,
                                                    const std::string &name)
    {
        const auto sym = graph.internSymbol(name);
        const auto op =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kSliceDynamic, sym);
        graph.addOperand(op, base);
        graph.addOperand(op, index);
        graph.addResult(op, result);
        graph.setAttr(op, "sliceWidth", width);
        return op;
    }

    wolvrix::lib::grh::OperationId makeAssign(wolvrix::lib::grh::Graph &graph,
                                              wolvrix::lib::grh::ValueId src,
                                              wolvrix::lib::grh::ValueId dst,
                                              const std::string &name)
    {
        const auto sym = graph.internSymbol(name);
        const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kAssign, sym);
        graph.addOperand(op, src);
        graph.addResult(op, dst);
        return op;
    }

    wolvrix::lib::grh::OperationId makeBinary(wolvrix::lib::grh::Graph &graph,
                                              wolvrix::lib::grh::OperationKind kind,
                                              wolvrix::lib::grh::ValueId lhs,
                                              wolvrix::lib::grh::ValueId rhs,
                                              wolvrix::lib::grh::ValueId result,
                                              const std::string &name)
    {
        const auto sym = graph.internSymbol(name);
        const auto op = graph.createOperation(kind, sym);
        graph.addOperand(op, lhs);
        graph.addOperand(op, rhs);
        graph.addResult(op, result);
        return op;
    }

    int test_false_loop_fixed()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_false");

        auto a_high = makeValue(graph, "a_high", 4);
        auto b_high = makeValue(graph, "b_high", 4);

        auto a_low_from_b = makeValue(graph, "a_low_from_b", 4);
        auto a = makeValue(graph, "a", 8);
        auto b_low_from_a = makeValue(graph, "b_low_from_a", 4);
        auto b = makeValue(graph, "b", 8);

        makeSliceStatic(graph, b, a_low_from_b, 0, 3, "slice_b_low");
        makeConcat(graph, "concat_a", {a_high, a_low_from_b}, a);

        makeSliceStatic(graph, a, b_low_from_a, 4, 7, "slice_a_high");
        makeConcat(graph, "concat_b", {b_high, b_low_from_a}, b);

        const std::size_t valuesBefore = graph.values().size();

        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>());

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected comb-loop-elim to succeed");
        }
        if (!res.changed)
        {
            return fail("Expected comb-loop-elim to report changes for false loop");
        }
        if (!diags.messages().empty())
        {
            return fail("Expected no diagnostics for fixed false loop");
        }

        if (graph.values().size() <= valuesBefore)
        {
            return fail("Expected comb-loop-elim to split values for false loop");
        }

        CombLoopElimOptions verifyOptions;
        verifyOptions.fixFalseLoops = false;
        PassManager verifyManager;
        verifyManager.addPass(std::make_unique<CombLoopElimPass>(verifyOptions));

        PassDiagnostics verifyDiags;
        PassManagerResult verifyRes{};
        try
        {
            verifyRes = verifyManager.run(design, verifyDiags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during verify run: ") + ex.what());
        }
        if (!verifyRes.success || verifyDiags.hasError())
        {
            return fail("Expected comb-loop-elim to succeed during verify run");
        }
        for (const auto &msg : verifyDiags.messages())
        {
            if (msg.passName == "comb-loop-elim" &&
                msg.message.find("comb loop detected") != std::string::npos)
            {
                return fail("Expected no comb-loop warnings after split");
            }
        }

        return 0;
    }


    int test_true_loop_reported()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_true");

        auto a = makeValue(graph, "a", 1);
        auto x = makeValue(graph, "x", 1);
        auto y = makeValue(graph, "y", 1);

        const auto xorOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                 graph.internSymbol("xor_xy"));
        graph.addOperand(xorOp, a);
        graph.addOperand(xorOp, y);
        graph.addResult(xorOp, x);

        const auto notOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kNot,
                                                 graph.internSymbol("not_x"));
        graph.addOperand(notOp, x);
        graph.addResult(notOp, y);

        CombLoopElimOptions options;
        options.fixFalseLoops = false;
        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>(options));

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected comb-loop-elim to succeed with warnings");
        }
        if (res.changed)
        {
            return fail("Expected comb-loop-elim to avoid changes for true loop");
        }

        bool foundWarning = false;
        for (const auto &msg : diags.messages())
        {
            if (msg.passName == "comb-loop-elim" &&
                msg.message.find("comb loop detected") != std::string::npos)
            {
                foundWarning = true;
                break;
            }
        }
        if (!foundWarning)
        {
            return fail("Expected comb-loop-elim warning for true loop");
        }
        return 0;
    }

    int test_dynamic_slice_false_loop_fixed()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_dynamic_false");

        auto cin = makeValue(graph, "cin", 1);
        auto p0 = makeValue(graph, "p0", 1);
        auto p1 = makeValue(graph, "p1", 1);
        auto carry = makeValue(graph, "carry", 3);

        auto zero32 = makeConst(graph, "zero32", "zero32_op", 32, "32'd0");
        auto one32 = makeConst(graph, "one32", "one32_op", 32, "32'd1");
        auto idx0 = makeValue(graph, "idx0", 5);
        auto idx1 = makeValue(graph, "idx1", 5);
        makeSliceStatic(graph, zero32, idx0, 0, 4, "idx0_slice");
        makeSliceStatic(graph, one32, idx1, 0, 4, "idx1_slice");

        auto fa0_cin = makeValue(graph, "fa0_cin", 1);
        auto fa1_cin = makeValue(graph, "fa1_cin", 1);
        makeSliceDynamic(graph, carry, idx0, fa0_cin, 1, "fa0_cin_slice");
        makeSliceDynamic(graph, carry, idx1, fa1_cin, 1, "fa1_cin_slice");

        auto fa0_cout = makeValue(graph, "fa0_cout", 1);
        auto fa1_cout = makeValue(graph, "fa1_cout", 1);
        makeBinary(graph, wolvrix::lib::grh::OperationKind::kOr, fa0_cin, p0, fa0_cout, "fa0_or");
        makeBinary(graph, wolvrix::lib::grh::OperationKind::kOr, fa1_cin, p1, fa1_cout, "fa1_or");

        auto carry_hi = makeValue(graph, "carry_hi", 2);
        auto shifted = makeValue(graph, "shifted", 3);
        auto carry_seed = makeValue(graph, "carry_seed", 1);
        auto carry_next = makeValue(graph, "carry_next", 3);
        makeSliceStatic(graph, carry, carry_hi, 1, 2, "carry_hi_slice");
        makeConcat(graph, "shifted_concat", {carry_hi, cin}, shifted);
        makeSliceStatic(graph, shifted, carry_seed, 0, 0, "carry_seed_slice");
        makeConcat(graph, "carry_next_concat", {fa1_cout, fa0_cout, carry_seed}, carry_next);
        makeAssign(graph, carry_next, carry, "carry_assign");

        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>());

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during dynamic false-loop run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected comb-loop-elim to succeed for dynamic false loop");
        }
        if (!res.changed)
        {
            return fail("Expected comb-loop-elim to rewrite dynamic false loop");
        }
        for (const auto &msg : diags.messages())
        {
            if (msg.passName == "comb-loop-elim" &&
                msg.message.find("comb loop detected") != std::string::npos)
            {
                return fail("Expected no comb-loop warnings for dynamic false loop");
            }
        }

        CombLoopElimOptions verifyOptions;
        verifyOptions.fixFalseLoops = false;
        PassManager verifyManager;
        verifyManager.addPass(std::make_unique<CombLoopElimPass>(verifyOptions));

        PassDiagnostics verifyDiags;
        PassManagerResult verifyRes{};
        try
        {
            verifyRes = verifyManager.run(design, verifyDiags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during dynamic false-loop verify run: ") + ex.what());
        }
        if (!verifyRes.success || verifyDiags.hasError())
        {
            return fail("Expected verify run to succeed for dynamic false loop");
        }
        for (const auto &msg : verifyDiags.messages())
        {
            if (msg.passName == "comb-loop-elim" &&
                msg.message.find("comb loop detected") != std::string::npos)
            {
                return fail("Expected no comb-loop warnings after dynamic false loop rewrite");
            }
        }

        return 0;
    }


    int test_add_concat_static_slice_carry_coupled_loop_reported()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_static_add_carry_loop");

        auto s = makeValue(graph, "s", 4);
        auto one = makeConst(graph, "one4", "one4_op", 4, "4'd1");
        auto sum = makeValue(graph, "s_sum", 4);
        makeBinary(graph, wolvrix::lib::grh::OperationKind::kAdd, s, one, sum, "s_add_one");

        auto msb = makeValue(graph, "msb", 1);
        auto lsb = makeValue(graph, "lsb", 3);
        makeSliceStatic(graph, sum, msb, 3, 3, "sum_msb_slice");
        makeSliceStatic(graph, sum, lsb, 0, 2, "sum_lsb_slice");

        auto next = makeValue(graph, "s_next", 4);
        makeConcat(graph, "s_next_concat", {msb, lsb}, next);
        makeAssign(graph, next, s, "s_assign");

        CombLoopElimOptions options;
        options.fixFalseLoops = false;
        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>(options));
        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during static add/concat/slice run: ") + ex.what());
        }
        if (!res.success || diags.hasError()) return fail("Expected pass to succeed for static add/concat/slice carry-coupled loop");
        if (res.changed) return fail("Expected static carry-coupled add loop not to be rewritten as a false loop");
        if (!hasCombLoopWarning(diags)) return fail("Expected comb-loop warning for static carry-coupled add loop");
        if (countOpKind(graph, wolvrix::lib::grh::OperationKind::kAdd) == 0) return fail("Expected kAdd to remain present");
        if (countOpKind(graph, wolvrix::lib::grh::OperationKind::kConcat) == 0) return fail("Expected kConcat to remain present");
        if (countOpKind(graph, wolvrix::lib::grh::OperationKind::kSliceStatic) == 0) return fail("Expected kSliceStatic to remain present");
        return 0;
    }

    int test_add_concat_dynamic_slice_carry_coupled_loop_reported()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_dynamic_add_carry_loop");

        auto s = makeValue(graph, "s", 4);
        auto one = makeConst(graph, "one4", "one4_op", 4, "4'd1");
        auto sum = makeValue(graph, "s_sum", 4);
        makeBinary(graph, wolvrix::lib::grh::OperationKind::kAdd, s, one, sum, "s_add_one");

        auto idx0 = makeConst(graph, "idx0", "idx0_op", 2, "2'd0");
        auto msb = makeValue(graph, "msb", 1);
        auto lsbDyn = makeValue(graph, "lsb_dyn", 3);
        makeSliceStatic(graph, sum, msb, 3, 3, "sum_msb_slice");
        makeSliceDynamic(graph, sum, idx0, lsbDyn, 3, "sum_lsb_dyn_slice");

        auto next = makeValue(graph, "s_next", 4);
        makeConcat(graph, "s_next_concat", {msb, lsbDyn}, next);
        makeAssign(graph, next, s, "s_assign");

        CombLoopElimOptions options;
        options.fixFalseLoops = false;
        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>(options));
        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during dynamic add/concat/slice run: ") + ex.what());
        }
        if (!res.success || diags.hasError()) return fail("Expected pass to succeed for dynamic add/concat/slice carry-coupled loop");
        if (res.changed) return fail("Expected dynamic carry-coupled add loop not to be rewritten as a false loop");
        if (!hasCombLoopWarning(diags)) return fail("Expected comb-loop warning for dynamic carry-coupled add loop");
        if (countOpKind(graph, wolvrix::lib::grh::OperationKind::kSliceDynamic) == 0) return fail("Expected kSliceDynamic to be exercised");
        if (countOpKind(graph, wolvrix::lib::grh::OperationKind::kAdd) == 0) return fail("Expected kAdd to remain present");
        return 0;
    }

} // namespace

int main()
{
    if (int rc = test_false_loop_fixed(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_dynamic_slice_false_loop_fixed(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_add_concat_static_slice_carry_coupled_loop_reported(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_add_concat_dynamic_slice_carry_coupled_loop_reported(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_true_loop_reported(); rc != 0)
    {
        return rc;
    }
    return 0;
}
