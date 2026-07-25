#include "generics.hpp"

#include <cassert>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace lang::frontend::detail {
namespace {

TypeSpec substitute_type(const TypeSpec& type,
                         std::span<const TypeSpec> arguments) {
    if (type.kind == TypeSpec::Kind::TypeParameter) {
        if (!type.type_parameter_index.has_value() ||
            *type.type_parameter_index >= arguments.size()) {
            throw std::logic_error(
                "generic type parameter index is out of range");
        }
        auto substituted = arguments[*type.type_parameter_index];
        substituted.position = type.position;
        return substituted;
    }

    TypeSpec result = type;
    result.left.reset();
    result.right.reset();
    result.element.reset();
    result.key.reset();
    result.value.reset();
    result.weak_target.reset();
    result.function_parameters.clear();
    result.function_return.reset();
    result.generic_arguments.clear();
    if (type.left != nullptr) {
        result.left = std::make_shared<TypeSpec>(
            substitute_type(*type.left, arguments));
    }
    if (type.right != nullptr) {
        result.right = std::make_shared<TypeSpec>(
            substitute_type(*type.right, arguments));
    }
    if (type.element != nullptr) {
        result.element = std::make_shared<TypeSpec>(
            substitute_type(*type.element, arguments));
    }
    if (type.key != nullptr) {
        result.key = std::make_shared<TypeSpec>(
            substitute_type(*type.key, arguments));
    }
    if (type.value != nullptr) {
        result.value = std::make_shared<TypeSpec>(
            substitute_type(*type.value, arguments));
    }
    if (type.weak_target != nullptr) {
        result.weak_target = std::make_shared<TypeSpec>(
            substitute_type(*type.weak_target, arguments));
    }
    result.function_parameters.reserve(type.function_parameters.size());
    for (const auto& parameter : type.function_parameters) {
        result.function_parameters.push_back(
            substitute_type(parameter, arguments));
    }
    if (type.function_return != nullptr) {
        result.function_return = std::make_shared<TypeSpec>(
            substitute_type(*type.function_return, arguments));
    }
    result.generic_arguments.reserve(type.generic_arguments.size());
    for (const auto& argument : type.generic_arguments) {
        result.generic_arguments.push_back(
            substitute_type(argument, arguments));
    }
    return result;
}

std::unique_ptr<Expr> clone_expr(const Expr& source,
                                 std::span<const TypeSpec> arguments);

LValue clone_lvalue(const LValue& source,
                    std::span<const TypeSpec> arguments) {
    LValue result;
    result.base_name = source.base_name;
    result.base_position = source.base_position;
    result.steps.reserve(source.steps.size());
    for (const auto& source_step : source.steps) {
        LValueStep step;
        step.kind = source_step.kind;
        step.name = source_step.name;
        step.position = source_step.position;
        if (source_step.index != nullptr) {
            step.index = clone_expr(*source_step.index, arguments);
        }
        result.steps.push_back(std::move(step));
    }
    return result;
}

MatchArm clone_match_arm(const MatchArm& source,
                         std::span<const TypeSpec> arguments);

Statement clone_statement(const Statement& source,
                          std::span<const TypeSpec> arguments) {
    Statement result;
    result.kind = source.kind;
    result.position = source.position;
    result.equals_position = source.equals_position;
    result.name = source.name;
    result.declared_type =
        substitute_type(source.declared_type, arguments);
    if (source.initializer != nullptr) {
        result.initializer = clone_expr(*source.initializer, arguments);
    }
    result.target = clone_lvalue(source.target, arguments);
    if (source.value != nullptr) {
        result.value = clone_expr(*source.value, arguments);
    }
    if (source.condition != nullptr) {
        result.condition = clone_expr(*source.condition, arguments);
    }
    result.then_branch.reserve(source.then_branch.size());
    for (const auto& statement : source.then_branch) {
        result.then_branch.push_back(
            clone_statement(statement, arguments));
    }
    result.else_branch.reserve(source.else_branch.size());
    for (const auto& statement : source.else_branch) {
        result.else_branch.push_back(
            clone_statement(statement, arguments));
    }
    result.body.reserve(source.body.size());
    for (const auto& statement : source.body) {
        result.body.push_back(clone_statement(statement, arguments));
    }
    result.loop_names = source.loop_names;
    result.loop_name_positions = source.loop_name_positions;
    if (source.iterable != nullptr) {
        result.iterable = clone_expr(*source.iterable, arguments);
    }
    if (source.range_upper != nullptr) {
        result.range_upper = clone_expr(*source.range_upper, arguments);
    }
    result.match_arms.reserve(source.match_arms.size());
    for (const auto& arm : source.match_arms) {
        result.match_arms.push_back(clone_match_arm(arm, arguments));
    }
    result.catch_body.reserve(source.catch_body.size());
    for (const auto& statement : source.catch_body) {
        result.catch_body.push_back(
            clone_statement(statement, arguments));
    }
    result.catch_name = source.catch_name;
    result.catch_position = source.catch_position;
    result.catch_type = substitute_type(source.catch_type, arguments);
    return result;
}

MatchArm clone_match_arm(const MatchArm& source,
                         std::span<const TypeSpec> arguments) {
    MatchArm result;
    result.case_name = source.case_name;
    result.position = source.position;
    result.bindings.reserve(source.bindings.size());
    for (const auto& binding : source.bindings) {
        result.bindings.push_back(
            MatchBinding{binding.name, binding.position, 0});
    }
    result.body.reserve(source.body.size());
    for (const auto& statement : source.body) {
        result.body.push_back(clone_statement(statement, arguments));
    }
    return result;
}

std::shared_ptr<LambdaExpr> clone_lambda(
    const LambdaExpr& source, std::span<const TypeSpec> arguments) {
    auto result = std::make_shared<LambdaExpr>();
    result->position = source.position;
    result->parameters.reserve(source.parameters.size());
    for (const auto& source_parameter : source.parameters) {
        Parameter parameter;
        parameter.name = source_parameter.name;
        parameter.position = source_parameter.position;
        parameter.type =
            substitute_type(source_parameter.type, arguments);
        result->parameters.push_back(std::move(parameter));
    }
    result->return_type =
        substitute_type(source.return_type, arguments);
    result->statements.reserve(source.statements.size());
    for (const auto& statement : source.statements) {
        result->statements.push_back(
            clone_statement(statement, arguments));
    }
    if (source.result != nullptr) {
        result->result = clone_expr(*source.result, arguments);
    }
    return result;
}

std::unique_ptr<Expr> clone_expr(const Expr& source,
                                 std::span<const TypeSpec> arguments) {
    auto result = std::make_unique<Expr>();
    result->kind = source.kind;
    result->position = source.position;
    result->operator_position = source.operator_position;
    result->int_value = source.int_value;
    result->bool_value = source.bool_value;
    result->string_value = source.string_value;
    result->name = source.name;
    result->binary_op = source.binary_op;
    result->pair_site = source.pair_site;
    result->array_element_type =
        substitute_type(source.array_element_type, arguments);
    result->map_key_type =
        substitute_type(source.map_key_type, arguments);
    result->map_value_type =
        substitute_type(source.map_value_type, arguments);
    result->type_arguments_position =
        source.type_arguments_position;
    result->explicit_type_arguments.reserve(
        source.explicit_type_arguments.size());
    for (const auto& type_argument : source.explicit_type_arguments) {
        result->explicit_type_arguments.push_back(
            substitute_type(type_argument, arguments));
    }
    if (source.left != nullptr) {
        result->left = clone_expr(*source.left, arguments);
    }
    if (source.right != nullptr) {
        result->right = clone_expr(*source.right, arguments);
    }
    if (source.receiver != nullptr) {
        result->receiver = clone_expr(*source.receiver, arguments);
    }
    result->arguments.reserve(source.arguments.size());
    for (const auto& argument : source.arguments) {
        result->arguments.push_back(clone_expr(*argument, arguments));
    }
    result->field_names = source.field_names;
    result->field_positions = source.field_positions;
    if (source.lambda != nullptr) {
        result->lambda = clone_lambda(*source.lambda, arguments);
    }
    return result;
}

void append_key(std::string& out, const TypeSpec& type) {
    switch (type.kind) {
    case TypeSpec::Kind::Int64:
        out += "I";
        return;
    case TypeSpec::Kind::Bool:
        out += "B";
        return;
    case TypeSpec::Kind::Str:
        out += "S";
        return;
    case TypeSpec::Kind::Pair:
        if (!type.has_pair_fields()) {
            out += "P0";
            return;
        }
        out += "P2[";
        append_key(out, *type.left);
        out += "][";
        append_key(out, *type.right);
        out += "]";
        return;
    case TypeSpec::Kind::Array:
        if (type.element == nullptr) {
            throw std::logic_error(
                "array type omitted element in generic key");
        }
        out += "A[";
        append_key(out, *type.element);
        out += "]";
        return;
    case TypeSpec::Kind::Function:
        if (type.function_return == nullptr) {
            throw std::logic_error(
                "function type omitted return in generic key");
        }
        out += "F" +
               std::to_string(type.function_parameters.size()) + "[";
        for (const auto& parameter : type.function_parameters) {
            append_key(out, parameter);
            out += ";";
        }
        out += "][";
        append_key(out, *type.function_return);
        out += "]";
        return;
    case TypeSpec::Kind::Map:
        if (type.key == nullptr || type.value == nullptr) {
            throw std::logic_error(
                "map type omitted entry in generic key");
        }
        out += "M[";
        append_key(out, *type.key);
        out += "][";
        append_key(out, *type.value);
        out += "]";
        return;
    case TypeSpec::Kind::Weak:
        if (type.weak_target == nullptr) {
            throw std::logic_error(
                "weak type omitted target in generic key");
        }
        out += "W[";
        append_key(out, *type.weak_target);
        out += "]";
        return;
    case TypeSpec::Kind::Ephemeron:
        if (type.key == nullptr || type.value == nullptr) {
            throw std::logic_error(
                "ephemeron type omitted entry in generic key");
        }
        out += "E[";
        append_key(out, *type.key);
        out += "][";
        append_key(out, *type.value);
        out += "]";
        return;
    case TypeSpec::Kind::Named:
        if (!type.named_type_index.has_value()) {
            throw std::logic_error(
                "unresolved named type in generic key");
        }
        out += "N" + std::to_string(*type.named_type_index) +
               ":" + std::to_string(type.name.size()) + ":" +
               type.name;
        return;
    case TypeSpec::Kind::Record:
        if (!type.record_layout_index.has_value()) {
            throw std::logic_error(
                "unresolved record type in generic key");
        }
        out += "R" + std::to_string(*type.record_layout_index) +
               ":" + std::to_string(type.name.size()) + ":" +
               type.name;
        return;
    case TypeSpec::Kind::Variant:
        if (!type.variant_layout_index.has_value()) {
            throw std::logic_error(
                "unresolved variant type in generic key");
        }
        out += "V" + std::to_string(*type.variant_layout_index) +
               ":" + std::to_string(type.name.size()) + ":" +
               type.name;
        return;
    case TypeSpec::Kind::TypeParameter:
        throw std::logic_error(
            "type parameter reached concrete generic key");
    case TypeSpec::Kind::Nil:
        throw std::logic_error("nil reached concrete generic key");
    case TypeSpec::Kind::Invalid:
        throw std::logic_error(
            "invalid type reached concrete generic key");
    }
    throw std::logic_error("unknown type in generic key");
}

void collect_expr_lambdas(
    Expr& expression,
    std::vector<std::shared_ptr<LambdaExpr>>& lambdas);

void collect_statement_lambdas(
    Statement& statement,
    std::vector<std::shared_ptr<LambdaExpr>>& lambdas) {
    if (statement.initializer != nullptr) {
        collect_expr_lambdas(*statement.initializer, lambdas);
    }
    if (statement.value != nullptr) {
        collect_expr_lambdas(*statement.value, lambdas);
    }
    if (statement.condition != nullptr) {
        collect_expr_lambdas(*statement.condition, lambdas);
    }
    if (statement.iterable != nullptr) {
        collect_expr_lambdas(*statement.iterable, lambdas);
    }
    if (statement.range_upper != nullptr) {
        collect_expr_lambdas(*statement.range_upper, lambdas);
    }
    for (auto& step : statement.target.steps) {
        if (step.index != nullptr) {
            collect_expr_lambdas(*step.index, lambdas);
        }
    }
    for (auto& inner : statement.then_branch) {
        collect_statement_lambdas(inner, lambdas);
    }
    for (auto& inner : statement.else_branch) {
        collect_statement_lambdas(inner, lambdas);
    }
    for (auto& inner : statement.body) {
        collect_statement_lambdas(inner, lambdas);
    }
    for (auto& arm : statement.match_arms) {
        for (auto& inner : arm.body) {
            collect_statement_lambdas(inner, lambdas);
        }
    }
    for (auto& inner : statement.catch_body) {
        collect_statement_lambdas(inner, lambdas);
    }
}

void collect_expr_lambdas(
    Expr& expression,
    std::vector<std::shared_ptr<LambdaExpr>>& lambdas) {
    if (expression.left != nullptr) {
        collect_expr_lambdas(*expression.left, lambdas);
    }
    if (expression.right != nullptr) {
        collect_expr_lambdas(*expression.right, lambdas);
    }
    if (expression.receiver != nullptr) {
        collect_expr_lambdas(*expression.receiver, lambdas);
    }
    for (auto& argument : expression.arguments) {
        collect_expr_lambdas(*argument, lambdas);
    }
    if (expression.lambda == nullptr) {
        return;
    }
    for (auto& statement : expression.lambda->statements) {
        collect_statement_lambdas(statement, lambdas);
    }
    if (expression.lambda->result != nullptr) {
        collect_expr_lambdas(*expression.lambda->result, lambdas);
    }
    lambdas.push_back(expression.lambda);
}

void collect_function_lambdas(
    FunctionDecl& function,
    std::vector<std::shared_ptr<LambdaExpr>>& lambdas) {
    for (auto& statement : function.statements) {
        collect_statement_lambdas(statement, lambdas);
    }
    if (function.result != nullptr) {
        collect_expr_lambdas(*function.result, lambdas);
    }
}

} // namespace

bool contains_type_parameter(const TypeSpec& type) {
    if (type.kind == TypeSpec::Kind::TypeParameter) {
        return true;
    }
    if (type.left != nullptr && contains_type_parameter(*type.left)) {
        return true;
    }
    if (type.right != nullptr && contains_type_parameter(*type.right)) {
        return true;
    }
    if (type.element != nullptr && contains_type_parameter(*type.element)) {
        return true;
    }
    if (type.key != nullptr && contains_type_parameter(*type.key)) {
        return true;
    }
    if (type.value != nullptr && contains_type_parameter(*type.value)) {
        return true;
    }
    if (type.weak_target != nullptr &&
        contains_type_parameter(*type.weak_target)) {
        return true;
    }
    for (const auto& parameter : type.function_parameters) {
        if (contains_type_parameter(parameter)) {
            return true;
        }
    }
    for (const auto& argument : type.generic_arguments) {
        if (contains_type_parameter(argument)) {
            return true;
        }
    }
    return type.function_return != nullptr &&
           contains_type_parameter(*type.function_return);
}

TypeSpec substitute_type_parameters(
    const TypeSpec& type, std::span<const TypeSpec> arguments) {
    return substitute_type(type, arguments);
}

std::string canonical_concrete_type_key(const TypeSpec& type) {
    std::string result;
    append_key(result, type);
    return result;
}

std::string canonical_type_argument_tuple_key(
    std::span<const TypeSpec> arguments) {
    std::string result = "T" + std::to_string(arguments.size()) + ":";
    for (const auto& argument : arguments) {
        const auto key = canonical_concrete_type_key(argument);
        result += std::to_string(key.size()) + ":" + key;
    }
    return result;
}

std::string mangle_generic_function_name(
    const std::string& name, std::span<const TypeSpec> arguments) {
    return name + "$mono$" +
           canonical_type_argument_tuple_key(arguments);
}

std::string render_generic_type_name(
    const std::string& name, std::span<const TypeSpec> arguments) {
    std::string result = name + "<";
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (i != 0) {
            result += ", ";
        }
        result += type_name(arguments[i]);
    }
    result += ">";
    return result;
}

FunctionDecl instantiate_generic_function(
    const FunctionDecl& declaration,
    std::span<const TypeSpec> arguments) {
    if (declaration.type_parameters.size() != arguments.size()) {
        throw std::logic_error(
            "generic instantiation argument count mismatch");
    }
    FunctionDecl result;
    result.name =
        mangle_generic_function_name(declaration.name, arguments);
    result.position = declaration.position;
    result.declaration_order = declaration.declaration_order;
    result.parameters.reserve(declaration.parameters.size());
    for (const auto& source_parameter : declaration.parameters) {
        Parameter parameter;
        parameter.name = source_parameter.name;
        parameter.position = source_parameter.position;
        parameter.type =
            substitute_type(source_parameter.type, arguments);
        result.parameters.push_back(std::move(parameter));
    }
    result.return_type =
        substitute_type(declaration.return_type, arguments);
    result.statements.reserve(declaration.statements.size());
    for (const auto& statement : declaration.statements) {
        result.statements.push_back(
            clone_statement(statement, arguments));
    }
    if (declaration.result != nullptr) {
        result.result = clone_expr(*declaration.result, arguments);
    }
    return result;
}

TypeDecl instantiate_generic_type(
    const TypeDecl& declaration, std::span<const TypeSpec> arguments) {
    if (declaration.type_parameters.size() != arguments.size()) {
        throw std::logic_error(
            "generic type instantiation argument count mismatch");
    }
    TypeDecl result;
    result.name =
        mangle_generic_function_name(declaration.name, arguments);
    result.position = declaration.position;
    result.body_position = declaration.body_position;
    result.body = substitute_type(declaration.body, arguments);
    result.declaration_order = declaration.declaration_order;
    return result;
}

RecordDecl instantiate_generic_record(
    const RecordDecl& declaration, std::span<const TypeSpec> arguments) {
    if (declaration.type_parameters.size() != arguments.size()) {
        throw std::logic_error(
            "generic record instantiation argument count mismatch");
    }
    RecordDecl result;
    result.name =
        mangle_generic_function_name(declaration.name, arguments);
    result.position = declaration.position;
    result.declaration_order = declaration.declaration_order;
    result.fields.reserve(declaration.fields.size());
    for (const auto& source_field : declaration.fields) {
        RecordFieldDecl field;
        field.name = source_field.name;
        field.position = source_field.position;
        field.type = substitute_type(source_field.type, arguments);
        result.fields.push_back(std::move(field));
    }
    return result;
}

VariantDecl instantiate_generic_variant(
    const VariantDecl& declaration, std::span<const TypeSpec> arguments) {
    if (declaration.type_parameters.size() != arguments.size()) {
        throw std::logic_error(
            "generic variant instantiation argument count mismatch");
    }
    VariantDecl result;
    result.name =
        mangle_generic_function_name(declaration.name, arguments);
    result.position = declaration.position;
    result.declaration_order = declaration.declaration_order;
    result.cases.reserve(declaration.cases.size());
    for (const auto& source_case : declaration.cases) {
        VariantCaseDecl variant_case;
        variant_case.name = source_case.name;
        variant_case.position = source_case.position;
        variant_case.fields.reserve(source_case.fields.size());
        for (const auto& source_field : source_case.fields) {
            variant_case.fields.push_back(
                substitute_type(source_field, arguments));
        }
        result.cases.push_back(std::move(variant_case));
    }
    return result;
}

void recollect_concrete_lambdas(Program& program) {
    program.lambdas.clear();
    for (auto& function : program.functions) {
        collect_function_lambdas(function, program.lambdas);
    }
    for (auto& statement : program.statements) {
        collect_statement_lambdas(statement, program.lambdas);
    }
    if (program.result != nullptr) {
        collect_expr_lambdas(*program.result, program.lambdas);
    }
}

} // namespace lang::frontend::detail
