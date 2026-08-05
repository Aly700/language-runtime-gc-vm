#pragma once

#include "lang/bytecode.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace lang {

struct OptimizerOptions {
    bool constant_folding{true};
    bool dead_code_elimination{true};
    bool peephole{true};
};

struct OptimizationStats {
    std::uint64_t instructions_before{0};
    std::uint64_t instructions_after{0};
    std::uint64_t folds_applied{0};
    std::uint64_t blocks_eliminated{0};
    std::uint64_t peepholes_applied{0};
};

struct OptimizationResult {
    std::optional<VerifiedModule> verified_module;
    OptimizationStats stats;
    std::vector<VerifierDiagnostic> diagnostics;

    [[nodiscard]] bool ok() const {
        return verified_module.has_value() && diagnostics.empty();
    }
};

[[nodiscard]] OptimizationResult optimize_module(
    Module module, OptimizerOptions options = {});

// Proves that the final verifier is load-bearing. This follows the ordinary
// optimizer path, corrupts the raw output immediately before verification,
// and must never return a VerifiedModule.
[[nodiscard]] OptimizationResult
TEST_ONLY_optimize_module_with_invalid_output(
    Module module, OptimizerOptions options = {});

} // namespace lang
