#pragma once

#include "lang/frontend/type_checker.hpp"

#include <string>
#include <vector>

namespace lang::frontend::detail {

void add_diagnostic(std::vector<Diagnostic>& diagnostics, SourcePosition position,
                    std::string message);

} // namespace lang::frontend::detail
