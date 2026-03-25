#include "emit/system_verilog.hpp"
#include "core/grh.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace wolvrix::lib::emit;
using namespace wolvrix::lib::grh;

namespace
{

int fail(const std::string &message)
{
    std::cerr << "[emit_sv_top_reachable] " << message << '\n';
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

std::size_t countSubstring(std::string_view text, std::string_view needle)
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

void addNoPortModuleRef(Graph &graph,
                        OperationKind kind,
                        std::string_view instanceSymbol,
                        std::string_view targetModule)
{
    const auto op = graph.createOperation(kind, graph.internSymbol(std::string(instanceSymbol)));
    graph.setAttr(op, "moduleName", std::string(targetModule));
    graph.setAttr(op, "inputPortName", std::vector<std::string>{});
    graph.setAttr(op, "outputPortName", std::vector<std::string>{});
}

Design buildDesign()
{
    Design design;
    Graph &leaf = design.createGraph("leaf");
    Graph &mid = design.createGraph("mid");
    Graph &bbLeaf = design.createGraph("bb_leaf");
    Graph &topA = design.createGraph("top_a");
    Graph &orphan = design.createGraph("orphan");

    (void)leaf;
    (void)bbLeaf;
    (void)orphan;

    addNoPortModuleRef(mid, OperationKind::kInstance, "u_leaf", "leaf");
    addNoPortModuleRef(topA, OperationKind::kInstance, "u_mid", "mid");
    addNoPortModuleRef(topA, OperationKind::kBlackbox, "u_bb_leaf", "bb_leaf");

    design.registerGraphAlias("top_a#(8)", topA);

    design.markAsTop("top_a");
    design.markAsTop("orphan");
    return design;
}

bool verifySingleModuleFile(const std::filesystem::path &path, std::string_view moduleName, std::string &error)
{
    const std::string text = readFile(path);
    if (text.empty())
    {
        error = "failed to read split module file: " + path.string();
        return false;
    }
    if (text.find("module " + std::string(moduleName)) == std::string::npos)
    {
        error = "split module file missing expected module declaration: " + path.string();
        return false;
    }
    if (countSubstring(text, "module ") != 1)
    {
        error = "split module file should contain exactly one module declaration: " + path.string();
        return false;
    }
    if (countSubstring(text, "endmodule") != 1)
    {
        error = "split module file should contain exactly one endmodule: " + path.string();
        return false;
    }
    return true;
}

} // namespace

#ifndef WOLF_SV_EMIT_ARTIFACT_DIR
#error "WOLF_SV_EMIT_ARTIFACT_DIR must be defined"
#endif

int main()
{
    const Design design = buildDesign();

    const std::filesystem::path artifactRoot = std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR);
    const std::filesystem::path singlePath = artifactRoot / "emit_top_reachable.sv";
    const std::filesystem::path orphanPath = artifactRoot / "emit_top_orphan_only.sv";
    const std::filesystem::path splitDir = artifactRoot / "emit_top_split_modules";

    std::error_code ec;
    std::filesystem::remove(singlePath, ec);
    ec.clear();
    std::filesystem::remove(orphanPath, ec);
    ec.clear();
    std::filesystem::remove_all(splitDir, ec);

    EmitDiagnostics diagSingle;
    EmitSystemVerilog emitterSingle(&diagSingle);
    EmitOptions singleOptions;
    singleOptions.outputDir = artifactRoot.string();
    singleOptions.outputFilename = std::string("emit_top_reachable.sv");
    singleOptions.topOverrides = {"top_a"};

    const EmitResult singleResult = emitterSingle.emit(design, singleOptions);
    if (!singleResult.success)
    {
        return fail("single-file emit with top override failed");
    }
    if (diagSingle.hasError())
    {
        return fail("single-file emit with top override reported diagnostics errors");
    }
    if (singleResult.artifacts.size() != 1)
    {
        return fail("single-file emit should report exactly one artifact");
    }

    const std::string singleOutput = readFile(singlePath);
    if (singleOutput.empty())
    {
        return fail("failed to read single-file reachable output");
    }
    if (singleOutput.find("top_a#(8)") != std::string::npos)
    {
        return fail("alias text should not appear as an emitted module or instance target name");
    }
    if (singleOutput.find("module top_a") == std::string::npos ||
        singleOutput.find("module mid") == std::string::npos ||
        singleOutput.find("module leaf") == std::string::npos)
    {
        return fail("reachable single-file emit is missing expected modules");
    }
    if (singleOutput.find("module bb_leaf") != std::string::npos)
    {
        return fail("top-reachable emit should not traverse blackbox module references into internal definitions");
    }
    if (singleOutput.find("module top_a#(8)") != std::string::npos)
    {
        return fail("single-file emit should keep canonical graph symbol instead of alias as module name");
    }
    if (singleOutput.find("module orphan") != std::string::npos)
    {
        return fail("reachable single-file emit should not include orphan");
    }

    EmitDiagnostics diagOrphan;
    EmitSystemVerilog emitterOrphan(&diagOrphan);
    EmitOptions orphanOptions;
    orphanOptions.outputDir = artifactRoot.string();
    orphanOptions.outputFilename = std::string("emit_top_orphan_only.sv");
    orphanOptions.topOverrides = {"orphan"};

    const EmitResult orphanResult = emitterOrphan.emit(design, orphanOptions);
    if (!orphanResult.success)
    {
        return fail("single-file orphan-only emit failed");
    }
    if (diagOrphan.hasError())
    {
        return fail("single-file orphan-only emit reported diagnostics errors");
    }

    const std::string orphanOutput = readFile(orphanPath);
    if (orphanOutput.empty())
    {
        return fail("failed to read orphan-only output");
    }
    if (orphanOutput.find("module orphan") == std::string::npos)
    {
        return fail("orphan-only emit is missing orphan module");
    }
    if (orphanOutput.find("module top_a") != std::string::npos ||
        orphanOutput.find("module mid") != std::string::npos ||
        orphanOutput.find("module leaf") != std::string::npos ||
        orphanOutput.find("module bb_leaf") != std::string::npos)
    {
        return fail("orphan-only emit should not include other modules");
    }

    EmitDiagnostics diagSplit;
    EmitSystemVerilog emitterSplit(&diagSplit);
    EmitOptions splitOptions;
    splitOptions.outputDir = splitDir.string();
    splitOptions.topOverrides = {"top_a"};
    splitOptions.splitModules = true;

    const EmitResult splitResult = emitterSplit.emit(design, splitOptions);
    if (!splitResult.success)
    {
        return fail("split-modules emit failed");
    }
    if (diagSplit.hasError())
    {
        return fail("split-modules emit reported diagnostics errors");
    }
    if (splitResult.artifacts.size() != 3)
    {
        return fail("split-modules emit should report exactly three artifacts for reachable instance graphs only");
    }

    std::set<std::string> artifactNames;
    for (const auto &artifact : splitResult.artifacts)
    {
        const std::filesystem::path path(artifact);
        if (!std::filesystem::exists(path))
        {
            return fail("reported split artifact does not exist");
        }
        artifactNames.insert(path.filename().string());
    }

    const std::set<std::string> expectedArtifacts = {"leaf.sv", "mid.sv", "top_a.sv"};
    if (artifactNames != expectedArtifacts)
    {
        return fail("split-modules artifact names do not match emitted module names");
    }
    if (std::filesystem::exists(splitDir / "orphan.sv"))
    {
        return fail("split-modules emit should not create an orphan module file");
    }

    std::string verifyError;
    if (!verifySingleModuleFile(splitDir / "top_a.sv", "top_a", verifyError))
    {
        return fail(verifyError);
    }
    if (!verifySingleModuleFile(splitDir / "mid.sv", "mid", verifyError))
    {
        return fail(verifyError);
    }
    if (!verifySingleModuleFile(splitDir / "leaf.sv", "leaf", verifyError))
    {
        return fail(verifyError);
    }
    if (std::filesystem::exists(splitDir / "bb_leaf.sv"))
    {
        return fail("split-modules emit should not materialize blackbox module references as internal module files");
    }

    const std::filesystem::path stalePath = splitDir / "stale_only.sv";
    const std::filesystem::path staleManagedPath = splitDir / "leaf.sv";
    {
        std::ofstream stale(stalePath);
        stale << "module stale_only;\nendmodule\n";
    }
    {
        std::ofstream staleManaged(staleManagedPath);
        staleManaged << "module leaf;\nendmodule\n";
    }
    if (!std::filesystem::exists(stalePath))
    {
        return fail("failed to create stale split-module file");
    }
    if (!std::filesystem::exists(staleManagedPath))
    {
        return fail("failed to create stale managed split-module file");
    }

    EmitDiagnostics diagSplitTopOnly;
    EmitSystemVerilog emitterSplitTopOnly(&diagSplitTopOnly);
    EmitOptions splitTopOnlyOptions;
    splitTopOnlyOptions.outputDir = splitDir.string();
    splitTopOnlyOptions.topOverrides = {"orphan"};
    splitTopOnlyOptions.splitModules = true;

    const EmitResult splitTopOnlyResult = emitterSplitTopOnly.emit(design, splitTopOnlyOptions);
    if (!splitTopOnlyResult.success)
    {
        return fail("split-modules re-emit on existing directory failed");
    }
    if (diagSplitTopOnly.hasError())
    {
        return fail("split-modules re-emit on existing directory reported diagnostics errors");
    }
    if (!std::filesystem::exists(stalePath))
    {
        return fail("split-modules re-emit should not delete unrelated .sv files from the output directory");
    }
    if (std::filesystem::exists(splitDir / "top_a.sv") ||
        std::filesystem::exists(splitDir / "mid.sv") ||
        std::filesystem::exists(splitDir / "leaf.sv"))
    {
        return fail("split-modules re-emit should remove stale module files that are no longer reachable");
    }
    if (!verifySingleModuleFile(splitDir / "orphan.sv", "orphan", verifyError))
    {
        return fail(verifyError);
    }

    // If regeneration fails after discovering an existing split directory, previously generated
    // outputs should remain intact instead of being deleted first.
    const std::filesystem::path failureDir = artifactRoot / "emit_top_split_modules_failure";
    std::filesystem::remove_all(failureDir, ec);
    EmitDiagnostics diagFailureSeed;
    EmitSystemVerilog emitterFailureSeed(&diagFailureSeed);
    EmitOptions failureSeedOptions;
    failureSeedOptions.outputDir = failureDir.string();
    failureSeedOptions.topOverrides = {"top_a"};
    failureSeedOptions.splitModules = true;
    const EmitResult failureSeedResult = emitterFailureSeed.emit(design, failureSeedOptions);
    if (!failureSeedResult.success || diagFailureSeed.hasError())
    {
        return fail("failed to seed split-modules failure fixture");
    }
    std::filesystem::create_directories(failureDir / ".mid.sv.tmp", ec);
    ec.clear();
    EmitDiagnostics diagFailureRetry;
    EmitSystemVerilog emitterFailureRetry(&diagFailureRetry);
    const EmitResult failureRetryResult = emitterFailureRetry.emit(design, failureSeedOptions);
    if (failureRetryResult.success)
    {
        return fail("split-modules emit should fail when a temp output path cannot be created");
    }
    if (!std::filesystem::exists(failureDir / "top_a.sv") ||
        !std::filesystem::exists(failureDir / "mid.sv") ||
        !std::filesystem::exists(failureDir / "leaf.sv"))
    {
        return fail("failed split-modules regeneration should preserve the previous emitted module set");
    }

    const std::filesystem::path splitAsFile = artifactRoot / "emit_top_split_as_file.sv";
    {
        std::ofstream file(splitAsFile);
        file << "not_a_directory\n";
    }
    EmitDiagnostics diagSplitFile;
    EmitSystemVerilog emitterSplitFile(&diagSplitFile);
    EmitOptions splitFileOptions;
    splitFileOptions.outputDir = splitAsFile.string();
    splitFileOptions.topOverrides = {"top_a"};
    splitFileOptions.splitModules = true;
    const EmitResult splitFileResult = emitterSplitFile.emit(design, splitFileOptions);
    if (splitFileResult.success)
    {
        return fail("split-modules emit should fail when outputDir points at a regular file");
    }
    if (!diagSplitFile.hasError())
    {
        return fail("split-modules emit should report diagnostics when outputDir is not a directory");
    }

    {
        Design externalBbDesign = buildDesign();
        Graph &bbTop = externalBbDesign.createGraph("bb_top");
        addNoPortModuleRef(bbTop, OperationKind::kBlackbox, "u_ext", "external_ip");
        externalBbDesign.markAsTop("bb_top");
        EmitDiagnostics bbDiags;
        EmitSystemVerilog bbEmitter(&bbDiags);
        EmitOptions bbOptions;
        bbOptions.outputDir = artifactRoot.string();
        bbOptions.outputFilename = std::string("emit_bb_top.sv");
        bbOptions.topOverrides = {"bb_top"};
        const EmitResult bbResult = bbEmitter.emit(externalBbDesign, bbOptions);
        if (!bbResult.success || bbDiags.hasError())
        {
            return fail("top-reachable emit should allow unresolved external blackbox references");
        }
    }

    {
        Design badDesign = buildDesign();
        Graph &badTop = badDesign.createGraph("bad_top");
        addNoPortModuleRef(badTop, OperationKind::kInstance, "u_missing", "missing_leaf");
        badDesign.markAsTop("bad_top");
        EmitDiagnostics badDiags;
        EmitSystemVerilog badEmitter(&badDiags);
        EmitOptions badOptions;
        badOptions.outputDir = artifactRoot.string();
        badOptions.outputFilename = std::string("emit_bad_top.sv");
        badOptions.topOverrides = {"bad_top"};
        const EmitResult badResult = badEmitter.emit(badDesign, badOptions);
        if (badResult.success || !badDiags.hasError())
        {
            return fail("top-reachable emit should fail on unresolved instance targets");
        }
    }

    return 0;
}
