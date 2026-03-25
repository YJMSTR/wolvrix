#include "core/ingest.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "slang/analysis/AnalysisManager.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/driver/Driver.h"

namespace {

int fail(const std::string& message) {
    std::cerr << "[ingest-graph-assembly-register-init] " << message << '\n';
    return 1;
}

struct CompilationBundle {
    slang::driver::Driver driver;
    std::unique_ptr<slang::ast::Compilation> compilation;
};

std::unique_ptr<CompilationBundle> compileInput(const std::filesystem::path& sourcePath,
                                                std::string_view topModule) {
    auto bundle = std::make_unique<CompilationBundle>();
    auto& driver = bundle->driver;
    driver.addStandardArgs();
    driver.languageVersion = slang::LanguageVersion::v1800_2023;
    if (!topModule.empty()) {
        driver.options.topModules.emplace_back(topModule);
    }

    std::vector<std::string> argStorage;
    argStorage.emplace_back("ingest-graph-assembly-register-init");
    argStorage.emplace_back(sourcePath.string());
    std::vector<const char*> argv;
    argv.reserve(argStorage.size());
    for (const std::string& arg : argStorage) {
        argv.push_back(arg.c_str());
    }

    if (!driver.parseCommandLine(static_cast<int>(argv.size()), argv.data())) {
        return nullptr;
    }
    if (!driver.processOptions()) {
        return nullptr;
    }
    if (!driver.parseAllSources()) {
        return nullptr;
    }

    bundle->compilation = driver.createCompilation();
    if (!bundle->compilation) {
        return nullptr;
    }
    driver.reportCompilation(*bundle->compilation, /* quiet */ true);
    driver.runAnalysis(*bundle->compilation);
    return bundle;
}

std::optional<std::string> getAttrString(const wolvrix::lib::grh::Operation& op,
                                         std::string_view key) {
    auto attr = op.attr(key);
    if (!attr) {
        return std::nullopt;
    }
    if (const auto* value = std::get_if<std::string>(&*attr)) {
        return *value;
    }
    return std::nullopt;
}

int testGraphAssemblyRegisterInit(const std::filesystem::path& sourcePath) {
    auto bundle = compileInput(sourcePath, "graph_assembly_register_init");
    if (!bundle || !bundle->compilation) {
        return fail("Failed to compile " + sourcePath.string());
    }

    wolvrix::lib::ingest::ConvertDriver driver;
    wolvrix::lib::grh::Design design = driver.convert(bundle->compilation->getRoot());

    const wolvrix::lib::grh::Graph* graph = design.findGraph("graph_assembly_register_init");
    if (!graph) {
        return fail("Missing graph_assembly_register_init graph");
    }

    int regDecls = 0;
    int regReads = 0;
    int regWrites = 0;
    bool foundRandomDecl = false;
    bool foundRandomRead = false;
    bool foundURandomDecl = false;
    bool foundWideDecl = false;
    bool foundXDecl = false;

    for (wolvrix::lib::grh::OperationId opId : graph->operations()) {
        wolvrix::lib::grh::Operation op = graph->getOperation(opId);
        switch (op.kind()) {
        case wolvrix::lib::grh::OperationKind::kRegister: {
            ++regDecls;
            auto initValue = getAttrString(op, "initValue");
            if (op.symbolText() == "random_bits") {
                if (!initValue || *initValue != "$random") {
                    return fail("kRegister random_bits missing initValue=$random");
                }
                foundRandomDecl = true;
            }
            else if (op.symbolText() == "urandom_bits") {
                if (!initValue || *initValue != "$urandom") {
                    return fail("kRegister urandom_bits missing initValue=$urandom");
                }
                foundURandomDecl = true;
            }
            else if (op.symbolText() == "wide_init") {
                if (!initValue || initValue->find("128'h") != 0 ||
                    initValue->find("123456789abcdef0011223344556677") == std::string::npos) {
                    return fail("kRegister wide_init did not preserve full 128-bit literal");
                }
                foundWideDecl = true;
            }
            else if (op.symbolText() == "x_init") {
                if (!initValue || initValue->find("4'h") != 0 ||
                    (initValue->back() != 'x' && initValue->back() != 'X')) {
                    return fail("kRegister x_init did not preserve four-state literal");
                }
                foundXDecl = true;
            }
            break;
        }
        case wolvrix::lib::grh::OperationKind::kRegisterReadPort: {
            ++regReads;
            auto regSymbol = getAttrString(op, "regSymbol");
            if (regSymbol && *regSymbol == "random_bits") {
                foundRandomRead = true;
            }
            break;
        }
        case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
            ++regWrites;
            break;
        default:
            break;
        }
    }

    if (!foundRandomDecl) {
        return fail("Expected kRegister declaration for random_bits");
    }
    if (!foundURandomDecl) {
        return fail("Expected kRegister declaration for urandom_bits");
    }
    if (!foundWideDecl) {
        return fail("Expected kRegister declaration for wide_init");
    }
    if (!foundXDecl) {
        return fail("Expected kRegister declaration for x_init");
    }
    if (!foundRandomRead) {
        return fail("Expected kRegisterReadPort for random_bits");
    }
    if (regDecls != 4 || regReads != 4) {
        return fail("Unexpected kRegister/kRegisterReadPort count");
    }
    if (regWrites != 0) {
        return fail("Did not expect kRegisterWritePort for declaration initializer only");
    }
    return 0;
}

} // namespace

#ifndef WOLF_SV_INGEST_GRAPH_ASSEMBLY_REGISTER_INIT_DATA_PATH
#error "WOLF_SV_INGEST_GRAPH_ASSEMBLY_REGISTER_INIT_DATA_PATH must be defined"
#endif

int main() {
    const std::filesystem::path sourcePath =
        WOLF_SV_INGEST_GRAPH_ASSEMBLY_REGISTER_INIT_DATA_PATH;
    return testGraphAssemblyRegisterInit(sourcePath);
}
