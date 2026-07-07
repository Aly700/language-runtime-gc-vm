#pragma once

#include "lang/bytecode.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lang::frontend {

enum class Type { Int64, Bool, Pair, Invalid };

struct SourcePosition {
    std::size_t offset{0};
    std::size_t line{1};
    std::size_t column{1};
};

struct Diagnostic {
    SourcePosition position;
    std::string message;
};

struct CompileResult {
    std::optional<Module> module;
    std::optional<VerifiedModule> verified_module;
    std::optional<Function> function;
    Type result_type{Type::Invalid};
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool ok() const {
        return module.has_value() && diagnostics.empty();
    }
};

[[nodiscard]] const char* type_name(Type type);
[[nodiscard]] CompileResult compile_program(std::string_view source);

} // namespace lang::frontend
