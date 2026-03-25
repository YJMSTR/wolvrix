#include "core/store.hpp"
#include "core/grh.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace wolvrix::lib::store;
using namespace wolvrix::lib::grh;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[store_json] " << message << '\n';
        return 1;
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

    Design buildDemoDesign()
    {
        Design design;
        Graph &graph = design.createGraph("demo");
        Graph &helper = design.createGraph("helper");

        ValueId in = graph.createValue(graph.internSymbol("in"), 8, false);
        graph.bindInputPort("in", in);

        ValueId out = graph.createValue(graph.internSymbol("out"), 8, false);
        graph.bindOutputPort("out", out);

        OperationId add = graph.createOperation(OperationKind::kAdd, graph.internSymbol("add0"));
        graph.addOperand(add, in);
        ValueId sum = graph.createValue(graph.internSymbol("sum"), 8, false);
        graph.addResult(add, sum);
        graph.setAttr(add, "weights", AttributeValue(std::vector<int64_t>{1, 2}));

        OperationId assign = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign0"));
        graph.addOperand(assign, sum);
        graph.addResult(assign, out);

        ValueId helperIn = helper.createValue(helper.internSymbol("in"), 8, false);
        helper.bindInputPort("in", helperIn);
        ValueId helperOut = helper.createValue(helper.internSymbol("out"), 8, false);
        helper.bindOutputPort("out", helperOut);
        OperationId helperAssign = helper.createOperation(OperationKind::kAssign, helper.internSymbol("assign_helper"));
        helper.addOperand(helperAssign, helperIn);
        helper.addResult(helperAssign, helperOut);

        design.registerGraphAlias("demo_alias", graph);
        design.markAsTop(graph.symbol());
        design.markAsTop(helper.symbol());
        return design;
    }

} // namespace

#ifndef WOLF_SV_EMIT_ARTIFACT_DIR
#error "WOLF_SV_EMIT_ARTIFACT_DIR must be defined"
#endif

int main()
{
    // Case 1: missing top graphs should fail gracefully.
    StoreDiagnostics diagNoTop;
    StoreJson emitterNoTop(&diagNoTop);
    Design emptyDesign;
    StoreResult resultNoTop = emitterNoTop.store(emptyDesign);
    if (resultNoTop.success)
    {
        return fail("StoreJson should fail when no tops are present");
    }
    if (!diagNoTop.hasError())
    {
        return fail("Expected diagnostics to capture missing tops for StoreJson");
    }

    Design design = buildDemoDesign();

    // Case 2: prettyCompact JSON emission with compact keys.
    StoreDiagnostics diagPrettyCompact;
    StoreJson emitterPrettyCompact(&diagPrettyCompact);
    StoreOptions prettyCompactOptions;
    prettyCompactOptions.outputDir = std::string(WOLF_SV_EMIT_ARTIFACT_DIR);
    prettyCompactOptions.topOverrides = {"demo"};

    StoreResult prettyCompactResult = emitterPrettyCompact.store(design, prettyCompactOptions);
    if (!prettyCompactResult.success)
    {
        return fail("StoreJson prettyCompact path failed");
    }
    if (diagPrettyCompact.hasError())
    {
        return fail("Unexpected diagnostics errors for prettyCompact emit");
    }
    if (prettyCompactResult.artifacts.empty())
    {
        return fail("PrettyCompact emit did not report an artifact");
    }

    const std::filesystem::path prettyCompactPath = prettyCompactResult.artifacts.front();
    const std::string prettyCompactJson = readFile(prettyCompactPath);
    if (prettyCompactJson.find("\"vals\"") == std::string::npos || prettyCompactJson.find("\"ops\"") == std::string::npos)
    {
        return fail("Compressed keys vals/ops not found in prettyCompact JSON");
    }
    if (prettyCompactJson.find("\"tops\"") == std::string::npos)
    {
        return fail("Top graph list is missing in prettyCompact JSON");
    }
    if (prettyCompactJson.find("\"aliases\"") == std::string::npos ||
        prettyCompactJson.find("demo_alias") == std::string::npos)
    {
        return fail("Alias map is missing in prettyCompact JSON");
    }
    if (prettyCompactJson.find("\"attrs\"") == std::string::npos || prettyCompactJson.find("\"int[]\"") == std::string::npos)
    {
        return fail("Attribute payload missing expected compact layout");
    }
    const std::size_t valsInlinePos = prettyCompactJson.find("{\"sym\": \"in\"");
    if (valsInlinePos == std::string::npos)
    {
        return fail("Value entry not rendered inline in prettyCompact mode");
    }
    const std::size_t newlineAfterVal = prettyCompactJson.find('\n', valsInlinePos);
    const std::size_t braceAfterVal = prettyCompactJson.find('}', valsInlinePos);
    if (newlineAfterVal != std::string::npos && newlineAfterVal < braceAfterVal)
    {
        return fail("Value entry spans multiple lines in prettyCompact mode");
    }

    Design parsed = Design::fromJsonString(prettyCompactJson);
    if (!parsed.findGraph("demo"))
    {
        return fail("Round-trip parsed design missing demo graph");
    }
    if (parsed.findGraph("helper"))
    {
        return fail("Top-filtered JSON should not serialize unrelated helper graph");
    }
    Graph *aliasGraph = parsed.findGraph("demo_alias");
    if (!aliasGraph || aliasGraph->symbol() != "demo")
    {
        return fail("Round-trip parsed design missing alias mapping");
    }

    // Case 3: unresolved topOverrides should fail atomically instead of returning partial JSON.
    StoreDiagnostics diagMissingTop;
    StoreJson emitterMissingTop(&diagMissingTop);
    StoreOptions missingTopOptions = prettyCompactOptions;
    missingTopOptions.topOverrides = {"demo", "missing_top"};
    const auto missingTopJson = emitterMissingTop.storeToString(design, missingTopOptions);
    if (missingTopJson.has_value())
    {
        return fail("storeToString should fail when any requested top is unresolved");
    }
    if (!diagMissingTop.hasError())
    {
        return fail("Expected diagnostics for unresolved topOverrides");
    }

    // Case 3b: unresolved external blackboxes should remain serializable in top-filtered export.
    Design externalBbDesign;
    Graph &bbTop = externalBbDesign.createGraph("bb_top");
    const auto bbIn = bbTop.createValue(bbTop.internSymbol("in"), 1, false);
    const auto bbOut = bbTop.createValue(bbTop.internSymbol("out"), 1, false);
    bbTop.bindInputPort("in", bbIn);
    bbTop.bindOutputPort("out", bbOut);
    const auto bbInst = bbTop.createOperation(OperationKind::kBlackbox, bbTop.internSymbol("u_ext"));
    bbTop.setAttr(bbInst, "moduleName", std::string("external_ip"));
    bbTop.setAttr(bbInst, "inputPortName", std::vector<std::string>{"in"});
    bbTop.setAttr(bbInst, "outputPortName", std::vector<std::string>{"out"});
    bbTop.addOperand(bbInst, bbIn);
    bbTop.addResult(bbInst, bbOut);
    externalBbDesign.markAsTop("bb_top");
    StoreDiagnostics diagExternalBb;
    StoreJson emitterExternalBb(&diagExternalBb);
    StoreOptions externalBbOptions;
    externalBbOptions.topOverrides = {"bb_top"};
    const auto externalBbJson = emitterExternalBb.storeToString(externalBbDesign, externalBbOptions);
    if (!externalBbJson.has_value() || diagExternalBb.hasError())
    {
        return fail("top-filtered JSON should allow unresolved external blackboxes");
    }

    // Case 4: compact mode should differ from prettyCompact output and avoid newlines.
    StoreDiagnostics diagCompact;
    StoreJson emitterCompact(&diagCompact);
    StoreOptions compactOptions = prettyCompactOptions;
    compactOptions.jsonMode = JsonPrintMode::Compact;

    StoreResult compactResult = emitterCompact.store(design, compactOptions);
    if (!compactResult.success || diagCompact.hasError())
    {
        return fail("Compact emit failed");
    }

    const std::string compactJson = readFile(compactResult.artifacts.front());
    if (compactJson == prettyCompactJson)
    {
        return fail("Compact emit should produce different layout from prettyCompact emit");
    }
    if (compactJson.find('\n') != std::string::npos)
    {
        return fail("Compact JSON should not contain newlines");
    }

    return 0;
}
