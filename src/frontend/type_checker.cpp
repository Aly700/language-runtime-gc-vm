#include "lang/frontend/type_checker.hpp"

#include "compiler.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "type_checker_internal.hpp"

#include <string_view>
#include <utility>

namespace lang::frontend {

const char* type_name(Type type) {
    switch (type) {
    case Type::Int64:
        return "i64";
    case Type::Bool:
        return "bool";
    case Type::Pair:
        return "pair";
    case Type::Invalid:
        return "invalid";
    }
    return "invalid";
}

CompileResult compile_program(std::string_view source) {
    auto lexed = detail::lex_source(source);
    if (!lexed.diagnostics.empty()) {
        CompileResult result;
        result.result_type = Type::Invalid;
        result.diagnostics = std::move(lexed.diagnostics);
        return result;
    }

    auto parsed = detail::parse_tokens(std::move(lexed.tokens));
    if (!parsed.program.has_value()) {
        CompileResult result;
        result.result_type = Type::Invalid;
        result.diagnostics = std::move(parsed.diagnostics);
        return result;
    }

    auto checked = detail::check_program(*parsed.program);
    const auto coarse_result_type = detail::public_type(checked.result_type);
    if (!checked.diagnostics.empty()) {
        CompileResult result;
        result.result_type = coarse_result_type;
        result.diagnostics = std::move(checked.diagnostics);
        return result;
    }

    auto compiled =
        detail::compile_checked_program(*parsed.program, checked.result_type);
    if (!compiled.module.has_value()) {
        CompileResult result;
        result.result_type = coarse_result_type;
        result.diagnostics = std::move(compiled.diagnostics);
        return result;
    }

    CompileResult result;
    result.result_type = coarse_result_type;
    result.module = std::move(*compiled.module);
    result.function = result.module->functions.at(result.module->entry_function);
    return result;
}

} // namespace lang::frontend
