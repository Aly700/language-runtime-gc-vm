#pragma once

#include "parser.hpp"

#include <vector>

namespace lang::frontend::detail {

struct TypeCheckResult {
    TypeSpec result_type{invalid_type()};
    std::vector<Diagnostic> diagnostics;
};

TypeCheckResult check_program(Program& program);

} // namespace lang::frontend::detail
