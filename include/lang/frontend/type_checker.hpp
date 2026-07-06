#pragma once

#include <string>
#include <vector>

namespace lang::frontend {

enum class Type { Int64, Bool, Pair, Nil };

struct Diagnostic {
    std::string message;
};

struct TypeCheckResult {
    bool ok{true};
    std::vector<Diagnostic> diagnostics;
};

TypeCheckResult type_check_placeholder(const std::string& source);

} // namespace lang::frontend
