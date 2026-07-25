#pragma once

#include "parser.hpp"

#include <span>
#include <string>

namespace lang::frontend::detail {

[[nodiscard]] bool contains_type_parameter(const TypeSpec& type);

[[nodiscard]] TypeSpec substitute_type_parameters(
    const TypeSpec& type, std::span<const TypeSpec> arguments);

[[nodiscard]] std::string canonical_concrete_type_key(
    const TypeSpec& type);

[[nodiscard]] std::string canonical_type_argument_tuple_key(
    std::span<const TypeSpec> arguments);

[[nodiscard]] std::string mangle_generic_function_name(
    const std::string& name, std::span<const TypeSpec> arguments);

[[nodiscard]] FunctionDecl instantiate_generic_function(
    const FunctionDecl& declaration,
    std::span<const TypeSpec> arguments);

void recollect_concrete_lambdas(Program& program);

} // namespace lang::frontend::detail
