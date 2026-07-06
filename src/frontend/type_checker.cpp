#include "lang/frontend/type_checker.hpp"

namespace lang::frontend {

TypeCheckResult type_check_placeholder(const std::string& source) {
    if (source.find("type_error") != std::string::npos) {
        return TypeCheckResult{false, {Diagnostic{"placeholder type error marker found"}}};
    }
    return TypeCheckResult{};
}

} // namespace lang::frontend
