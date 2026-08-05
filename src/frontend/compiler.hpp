#pragma once

#include "parser.hpp"

#include "lang/bytecode.hpp"

#include <optional>
#include <vector>

namespace lang::frontend::detail {

struct CompileModuleResult {
    std::optional<VerifiedModule> verified_module;
    std::optional<OptimizationStats> optimization_stats;
    std::vector<Diagnostic> diagnostics;
};

CompileModuleResult compile_checked_program(const Program& program,
                                            const TypeSpec& result_type,
                                            const OptimizerOptions*
                                                optimizer_options = nullptr);

} // namespace lang::frontend::detail
