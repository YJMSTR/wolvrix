#include "core/emit.hpp"
#include "core/grh.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace wolvrix::lib::emit;
using namespace wolvrix::lib::grh;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[emit_base] " << message << '\n';
        return 1;
    }

    class StubEmit : public Emit
    {
    public:
        using Emit::Emit;

        std::size_t callCount = 0;
        std::size_t lastTopCount = 0;
        std::filesystem::path lastOutputPath;

    private:
        EmitResult emitImpl(const Design &, std::span<const Graph *const> topGraphs, const EmitOptions &options) override
        {
            ++callCount;
            lastTopCount = topGraphs.size();

            EmitResult result;
            if (!options.outputDir)
            {
                result.success = false;
                return result;
            }

            lastOutputPath = std::filesystem::path(*options.outputDir) / "emit_stub.txt";
            auto stream = openOutputFile(lastOutputPath);
            if (!stream)
            {
                result.success = false;
                return result;
            }

            *stream << "emit_stub";
            result.artifacts.push_back(lastOutputPath.string());
            return result;
        }
    };

} // namespace

int main()
{
#ifndef WOLF_SV_EMIT_ARTIFACT_DIR
#error "WOLF_SV_EMIT_ARTIFACT_DIR must be defined"
#endif

    // Case 1: no tops available
    EmitDiagnostics diagNoTop;
    StubEmit emitterNoTop(&diagNoTop);
    Design emptyDesign;
    EmitResult noTopResult = emitterNoTop.emit(emptyDesign);
    if (noTopResult.success)
    {
        return fail("Expected emit to fail when no top graphs are present");
    }
    if (!diagNoTop.hasError())
    {
        return fail("Expected diagnostics to record an error for missing tops");
    }
    if (emitterNoTop.callCount != 0)
    {
        return fail("emitImpl should not be invoked when tops are missing");
    }

    // Case 2: override points to missing top
    EmitDiagnostics diagMissingOverride;
    StubEmit emitterMissingOverride(&diagMissingOverride);
    Design designWithTop;
    designWithTop.createGraph("demo");
    EmitOptions missingOverrideOptions;
    missingOverrideOptions.topOverrides.push_back("absent_top");
    EmitResult missingOverrideResult = emitterMissingOverride.emit(designWithTop, missingOverrideOptions);
    if (missingOverrideResult.success)
    {
        return fail("Expected emit to fail when override top cannot be resolved");
    }
    if (!diagMissingOverride.hasError())
    {
        return fail("Expected diagnostics to capture missing override error");
    }
    if (emitterMissingOverride.callCount != 0)
    {
        return fail("emitImpl should not be called when override tops are unresolved");
    }

    // Case 2b: override list mixes valid and missing tops; emission must still be all-or-nothing.
    EmitDiagnostics diagMixedOverride;
    StubEmit emitterMixedOverride(&diagMixedOverride);
    EmitOptions mixedOverrideOptions;
    mixedOverrideOptions.outputDir = std::string(WOLF_SV_EMIT_ARTIFACT_DIR);
    mixedOverrideOptions.topOverrides.push_back("demo");
    mixedOverrideOptions.topOverrides.push_back("absent_top");
    EmitResult mixedOverrideResult = emitterMixedOverride.emit(designWithTop, mixedOverrideOptions);
    if (mixedOverrideResult.success)
    {
        return fail("Expected emit to fail when any override top cannot be resolved");
    }
    if (!diagMixedOverride.hasError())
    {
        return fail("Expected diagnostics to capture mixed override resolution error");
    }
    if (emitterMixedOverride.callCount != 0)
    {
        return fail("emitImpl should not be called when override tops are only partially resolved");
    }

    // Case 2c: canonical name plus alias for the same graph should still resolve to one top.
    EmitDiagnostics diagAliasOverride;
    StubEmit emitterAliasOverride(&diagAliasOverride);
    Graph *demoGraph = designWithTop.findGraph("demo");
    if (demoGraph == nullptr)
    {
        return fail("Expected demo graph to exist before alias registration");
    }
    designWithTop.registerGraphAlias("demo_alias", *demoGraph);
    EmitOptions aliasOverrideOptions;
    aliasOverrideOptions.outputDir = std::string(WOLF_SV_EMIT_ARTIFACT_DIR);
    aliasOverrideOptions.topOverrides.push_back("demo");
    aliasOverrideOptions.topOverrides.push_back("demo_alias");
    EmitResult aliasOverrideResult = emitterAliasOverride.emit(designWithTop, aliasOverrideOptions);
    if (!aliasOverrideResult.success)
    {
        return fail("Expected emit to succeed when alias and canonical top name resolve to the same graph");
    }
    if (diagAliasOverride.hasError())
    {
        return fail("Did not expect diagnostics for alias + canonical duplicate top override");
    }
    if (emitterAliasOverride.callCount != 1 || emitterAliasOverride.lastTopCount != 1)
    {
        return fail("Alias + canonical duplicate top overrides should resolve to exactly one top graph");
    }

    // Case 3: successful path with output
    EmitDiagnostics diagOk;
    StubEmit emitterOk(&diagOk);
    Design okDesign;
    Graph &topGraph = okDesign.createGraph("top");
    okDesign.markAsTop(topGraph.symbol());

    EmitOptions okOptions;
    okOptions.outputDir = std::string(WOLF_SV_EMIT_ARTIFACT_DIR);

    EmitResult okResult = emitterOk.emit(okDesign, okOptions);
    if (!okResult.success)
    {
        return fail("Expected emit to succeed for valid top and output dir");
    }
    if (diagOk.hasError())
    {
        return fail("Unexpected diagnostics errors for successful emit path");
    }
    if (emitterOk.callCount != 1)
    {
        return fail("emitImpl should be invoked once on successful emit");
    }
    if (emitterOk.lastTopCount != 1)
    {
        return fail("emitImpl should see exactly one top graph");
    }
    if (okResult.artifacts.empty())
    {
        return fail("EmitResult should record produced artifacts");
    }

    const std::filesystem::path artifactPath = emitterOk.lastOutputPath;
    if (!std::filesystem::exists(artifactPath))
    {
        return fail("Expected output artifact file to be created");
    }

    std::ifstream artifactFile(artifactPath);
    std::string content;
    artifactFile >> content;
    if (content != "emit_stub")
    {
        return fail("Unexpected artifact content from emitImpl");
    }

    return 0;
}
