#include "type_checker_internal.hpp"

#include "diagnostics.hpp"

#include <cassert>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace lang::frontend::detail {
namespace {

struct TypedValue {
    TypeSpec type{invalid_type()};
    std::set<std::size_t> object_sites;
};

struct FieldState {
    TypedValue left;
    TypedValue right;
};

struct LocalState {
    std::string name;
    TypeSpec declared_type{invalid_type()};
    std::uint32_t index{0};
    bool initialized{false};
    TypedValue value;
    SourcePosition declaration_position;
};

struct FlowState {
    std::vector<LocalState> locals;
    std::vector<std::optional<FieldState>> fields_by_site;
};

bool operator==(const TypedValue& lhs, const TypedValue& rhs) {
    return lhs.type == rhs.type && lhs.object_sites == rhs.object_sites;
}

bool operator==(const FieldState& lhs, const FieldState& rhs) {
    return lhs.left == rhs.left && lhs.right == rhs.right;
}

bool operator==(const LocalState& lhs, const LocalState& rhs) {
    return lhs.name == rhs.name && lhs.declared_type == rhs.declared_type &&
           lhs.index == rhs.index && lhs.initialized == rhs.initialized &&
           lhs.value == rhs.value;
}

bool operator==(const FlowState& lhs, const FlowState& rhs) {
    return lhs.locals == rhs.locals && lhs.fields_by_site == rhs.fields_by_site;
}

TypedValue invalid_value() { return TypedValue{invalid_type(), {}}; }
TypedValue scalar_value(TypeSpec type) { return TypedValue{std::move(type), {}}; }

TypedValue pair_value(TypeSpec type, std::size_t site) {
    TypedValue value;
    value.type = std::move(type);
    value.object_sites.insert(site);
    return value;
}

TypedValue value_from_type(TypeSpec type) {
    return TypedValue{std::move(type), {}};
}

TypedValue value_as_declared_type(const TypedValue& value, TypeSpec declared_type) {
    TypedValue coerced;
    coerced.type = std::move(declared_type);
    if (is_pair(coerced.type)) {
        coerced.object_sites = value.object_sites;
    }
    return coerced;
}

TypedValue join_values(const TypedValue& lhs, const TypedValue& rhs) {
    if (is_invalid(lhs.type)) {
        return rhs;
    }
    if (is_invalid(rhs.type)) {
        return lhs;
    }
    const auto joined_type = join_types(lhs.type, rhs.type);
    if (is_invalid(joined_type)) {
        return invalid_value();
    }
    TypedValue result;
    result.type = joined_type;
    if (is_pair(result.type)) {
        result.object_sites = lhs.object_sites;
        result.object_sites.insert(rhs.object_sites.begin(), rhs.object_sites.end());
    }
    return result;
}

FieldState join_fields(const FieldState& lhs, const FieldState& rhs) {
    return FieldState{join_values(lhs.left, rhs.left), join_values(lhs.right, rhs.right)};
}

FlowState join_states(const FlowState& lhs, const FlowState& rhs) {
    FlowState result = lhs;
    assert(lhs.locals.size() == rhs.locals.size());
    for (std::size_t i = 0; i < result.locals.size(); ++i) {
        result.locals[i].initialized = lhs.locals[i].initialized && rhs.locals[i].initialized;
        if (result.locals[i].initialized) {
            result.locals[i].value = join_values(lhs.locals[i].value, rhs.locals[i].value);
        }
    }

    assert(lhs.fields_by_site.size() == rhs.fields_by_site.size());
    for (std::size_t i = 0; i < result.fields_by_site.size(); ++i) {
        const auto& left = lhs.fields_by_site[i];
        const auto& right = rhs.fields_by_site[i];
        if (left.has_value() && right.has_value()) {
            result.fields_by_site[i] = join_fields(*left, *right);
        } else if (right.has_value()) {
            result.fields_by_site[i] = *right;
        }
    }
    return result;
}

struct FunctionSymbol {
    std::string name;
    SourcePosition position;
    std::size_t index{0};
    std::vector<TypeSpec> parameters;
    TypeSpec return_type{invalid_type()};
};

class TypeChecker {
public:
    explicit TypeChecker(std::size_t pair_site_count) : pair_site_count_(pair_site_count) {
        state_.fields_by_site.resize(pair_site_count_);
    }

    TypeSpec check(Program& program) {
        collect_function_symbols(program);
        for (auto& function : program.functions) {
            check_function(function);
        }

        state_ = initial_state();
        for (auto& statement : program.statements) {
            check_statement(statement, state_, true);
        }
        const auto result = check_expr(*program.result, state_);
        program.entry_local_count = static_cast<std::uint32_t>(state_.locals.size());
        return result.type;
    }

    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const {
        return diagnostics_;
    }

    [[nodiscard]] std::uint32_t local_count() const {
        return static_cast<std::uint32_t>(state_.locals.size());
    }

private:
    FlowState initial_state() const {
        FlowState state;
        state.fields_by_site.resize(pair_site_count_);
        return state;
    }

    void collect_function_symbols(Program& program) {
        for (std::size_t i = 0; i < program.functions.size(); ++i) {
            auto& declaration = program.functions[i];
            declaration.function_index = i + 1;
            if (find_function(declaration.name) != nullptr) {
                diagnose(declaration.position,
                         "function '" + declaration.name + "' is already defined");
                continue;
            }

            FunctionSymbol symbol;
            symbol.name = declaration.name;
            symbol.position = declaration.position;
            symbol.index = declaration.function_index;
            symbol.return_type = declaration.return_type;
            for (const auto& parameter : declaration.parameters) {
                symbol.parameters.push_back(parameter.type);
            }
            functions_.push_back(std::move(symbol));
        }
    }

    void check_function(FunctionDecl& function) {
        FlowState state = initial_state();
        for (auto& parameter : function.parameters) {
            if (find_local(state, parameter.name) != nullptr) {
                diagnose(parameter.position,
                         "parameter '" + parameter.name + "' is already defined");
                continue;
            }
            const auto index = static_cast<std::uint32_t>(state.locals.size());
            parameter.local_index = index;
            state.locals.push_back(LocalState{parameter.name,
                                              parameter.type,
                                              index,
                                              true,
                                              value_from_type(parameter.type),
                                              parameter.position});
        }

        for (auto& statement : function.statements) {
            check_statement(statement, state, true);
        }
        const auto result = check_expr(*function.result, state);
        if (!is_invalid(result.type) &&
            !value_conforms_to_type(result, function.return_type, state)) {
            diagnose(function.result->position,
                     "function '" + function.name + "' returns " +
                         type_name(result.type) + " but is declared " +
                         type_name(function.return_type));
        }
        function.local_count = static_cast<std::uint32_t>(state.locals.size());
    }

    FunctionSymbol* find_function(const std::string& name) {
        for (auto& function : functions_) {
            if (function.name == name) {
                return &function;
            }
        }
        return nullptr;
    }

    const FunctionSymbol* find_function(const std::string& name) const {
        for (const auto& function : functions_) {
            if (function.name == name) {
                return &function;
            }
        }
        return nullptr;
    }

    LocalState* find_local(FlowState& state, const std::string& name) {
        for (auto& local : state.locals) {
            if (local.name == name) {
                return &local;
            }
        }
        return nullptr;
    }

    const LocalState* find_local(const FlowState& state, const std::string& name) const {
        for (const auto& local : state.locals) {
            if (local.name == name) {
                return &local;
            }
        }
        return nullptr;
    }

    void diagnose(SourcePosition position, std::string message) {
        add_diagnostic(diagnostics_, position, std::move(message));
    }

    std::optional<TypedValue> try_load_field(const TypedValue& receiver,
                                             const std::string& field,
                                             const FlowState& state) const {
        if (!is_pair(receiver.type)) {
            return std::nullopt;
        }

        std::optional<TypedValue> loaded;
        if (receiver.type.has_pair_fields()) {
            const auto& field_type = field == "left" ? *receiver.type.left
                                                     : *receiver.type.right;
            loaded = value_from_type(field_type);
        }

        for (const auto site : receiver.object_sites) {
            if (site >= state.fields_by_site.size() ||
                !state.fields_by_site[site].has_value()) {
                return std::nullopt;
            }
            const auto& fields = *state.fields_by_site[site];
            const auto& value = field == "left" ? fields.left : fields.right;
            if (!loaded.has_value()) {
                loaded = value;
            } else {
                loaded = join_values(*loaded, value);
            }
        }

        if (!loaded.has_value() || is_invalid(loaded->type)) {
            return std::nullopt;
        }
        return loaded;
    }

    bool value_conforms_to_type(const TypedValue& value, const TypeSpec& target,
                                const FlowState& state) const {
        if (is_invalid(value.type) || is_invalid(target)) {
            return false;
        }
        if (target.kind == TypeSpec::Kind::Int64 || target.kind == TypeSpec::Kind::Bool) {
            return value.type == target;
        }
        if (!is_pair(value.type)) {
            return false;
        }
        if (!target.has_pair_fields()) {
            return true;
        }

        const auto left = try_load_field(value, "left", state);
        const auto right = try_load_field(value, "right", state);
        if (!left.has_value() || !right.has_value()) {
            return false;
        }
        return value_conforms_to_type(*left, *target.left, state) &&
               value_conforms_to_type(*right, *target.right, state);
    }

    void check_statement(Statement& statement, FlowState& state, bool allow_let) {
        switch (statement.kind) {
        case Statement::Kind::Let:
            check_let(statement, state, allow_let);
            break;
        case Statement::Kind::Assign:
            check_assignment(statement, state);
            break;
        case Statement::Kind::If:
            check_if(statement, state);
            break;
        case Statement::Kind::While:
            check_while(statement, state);
            break;
        }
    }

    void check_let(Statement& statement, FlowState& state, bool allow_let) {
        if (!allow_let) {
            diagnose(statement.position,
                     "let declarations are only allowed at the top level");
            return;
        }
        if (find_local(state, statement.name) != nullptr) {
            diagnose(statement.position, "local '" + statement.name + "' is already defined");
            return;
        }

        const auto initializer = check_expr(*statement.initializer, state);
        if (!is_invalid(initializer.type) &&
            !value_conforms_to_type(initializer, statement.declared_type, state)) {
            diagnose(statement.equals_position,
                     "cannot initialize local '" + statement.name + "' of type " +
                         type_name(statement.declared_type) + " with " +
                         type_name(initializer.type));
            return;
        }

        const auto index = static_cast<std::uint32_t>(state.locals.size());
        statement.local_index = index;
        state.locals.push_back(LocalState{statement.name,
                                          statement.declared_type,
                                          index,
                                          true,
                                          value_as_declared_type(initializer,
                                                                 statement.declared_type),
                                          statement.position});
    }

    void check_assignment(Statement& statement, FlowState& state) {
        const auto assigned = check_expr(*statement.value, state);
        if (statement.target.fields.empty()) {
            auto* local = find_local(state, statement.target.base_name);
            if (local == nullptr) {
                diagnose(statement.target.base_position,
                         "undefined variable '" + statement.target.base_name + "'");
                return;
            }
            statement.target.local_index = local->index;
            if (!is_invalid(assigned.type) &&
                !value_conforms_to_type(assigned, local->declared_type, state)) {
                diagnose(statement.equals_position,
                         "cannot assign " + type_name(assigned.type) +
                             " to local '" + local->name + "' of type " +
                             type_name(local->declared_type));
                return;
            }
            local->initialized = true;
            local->value = value_as_declared_type(assigned, local->declared_type);
            return;
        }

        const auto receiver = check_lvalue_prefix(statement.target, state,
                                                 statement.target.fields.size() - 1);
        const auto& field = statement.target.fields.back();
        if (is_invalid(receiver.type) || is_invalid(assigned.type)) {
            return;
        }
        if (!is_pair(receiver.type)) {
            diagnose(field.position, "field assignment requires pair");
            return;
        }

        auto existing = load_field(receiver, field.name, field.position, state);
        if (is_invalid(existing.type)) {
            return;
        }
        if (!value_conforms_to_type(assigned, existing.type, state)) {
            diagnose(statement.equals_position,
                     "cannot assign " + type_name(assigned.type) +
                         " to field '" + field.name + "' of type " +
                         type_name(existing.type));
            return;
        }

        store_field(receiver, field.name, assigned, state);
    }

    void check_if(Statement& statement, FlowState& state) {
        const auto condition = check_expr(*statement.condition, state);
        if (!is_invalid(condition.type) && condition.type != bool_type()) {
            diagnose(statement.condition->position, "if condition must be bool");
        }

        auto then_state = state;
        auto else_state = state;
        for (auto& inner : statement.then_branch) {
            check_statement(inner, then_state, false);
        }
        for (auto& inner : statement.else_branch) {
            check_statement(inner, else_state, false);
        }
        state = join_states(then_state, else_state);
    }

    void check_while(Statement& statement, FlowState& state) {
        FlowState head = state;
        for (std::size_t iteration = 0; iteration < max_loop_iterations(state); ++iteration) {
            auto body_input = head;
            const auto condition = check_expr(*statement.condition, body_input);
            if (!is_invalid(condition.type) && condition.type != bool_type()) {
                diagnose(statement.condition->position, "while condition must be bool");
            }

            auto body_output = body_input;
            for (auto& inner : statement.body) {
                check_statement(inner, body_output, false);
            }

            auto joined = join_states(head, body_output);
            if (joined == head) {
                state = joined;
                return;
            }
            head = std::move(joined);
        }
        diagnose(statement.position, "while type state did not reach a fixed point");
        state = std::move(head);
    }

    std::size_t max_loop_iterations(const FlowState& state) const {
        return state.fields_by_site.size() + state.locals.size() + 8;
    }

    TypedValue check_expr(Expr& expression, FlowState& state) {
        switch (expression.kind) {
        case Expr::Kind::IntLiteral:
            return annotate(expression, scalar_value(int64_type()));
        case Expr::Kind::BoolLiteral:
            return annotate(expression, scalar_value(bool_type()));
        case Expr::Kind::Variable:
            return check_variable(expression, state);
        case Expr::Kind::PairLiteral:
            return check_pair_literal(expression, state);
        case Expr::Kind::Binary:
            return check_binary(expression, state);
        case Expr::Kind::Field:
            return check_field_access(expression, state);
        case Expr::Kind::Call:
            return check_call(expression, state);
        }
        return annotate(expression, invalid_value());
    }

    TypedValue annotate(Expr& expression, TypedValue value) {
        expression.inferred_type = value.type;
        expression.object_sites = value.object_sites;
        return value;
    }

    TypedValue check_variable(Expr& expression, FlowState& state) {
        auto* local = find_local(state, expression.name);
        if (local == nullptr) {
            diagnose(expression.position, "undefined variable '" + expression.name + "'");
            return annotate(expression, invalid_value());
        }
        expression.local_index = local->index;
        if (!local->initialized) {
            diagnose(expression.position,
                     "local '" + expression.name + "' may be uninitialized");
            return annotate(expression, invalid_value());
        }
        return annotate(expression, local->value);
    }

    TypedValue check_pair_literal(Expr& expression, FlowState& state) {
        const auto left = check_expr(*expression.left, state);
        const auto right = check_expr(*expression.right, state);
        if (expression.pair_site >= state.fields_by_site.size()) {
            diagnose(expression.position, "internal pair site index out of range");
            return annotate(expression, invalid_value());
        }

        // Agreement accommodation: source pair field typing mirrors the verifier's
        // allocation-site field lattice. Reusing a pair constructor site across loop
        // iterations joins field states instead of overwriting them.
        FieldState fields{left, right};
        auto& slot = state.fields_by_site[expression.pair_site];
        if (slot.has_value()) {
            slot = join_fields(*slot, fields);
        } else {
            slot = fields;
        }
        const auto inferred_type = pair_type(left.type, right.type);
        return annotate(expression, pair_value(inferred_type, expression.pair_site));
    }

    TypedValue check_binary(Expr& expression, FlowState& state) {
        const auto left = check_expr(*expression.left, state);
        const auto right = check_expr(*expression.right, state);
        if (expression.binary_op == '+') {
            if ((!is_invalid(left.type) && left.type != int64_type()) ||
                (!is_invalid(right.type) && right.type != int64_type())) {
                diagnose(expression.operator_position, "operator '+' requires i64 operands");
                return annotate(expression, invalid_value());
            }
            return annotate(expression, scalar_value(int64_type()));
        }
        if (expression.binary_op == '<') {
            if ((!is_invalid(left.type) && left.type != int64_type()) ||
                (!is_invalid(right.type) && right.type != int64_type())) {
                diagnose(expression.operator_position, "operator '<' requires i64 operands");
                return annotate(expression, invalid_value());
            }
            return annotate(expression, scalar_value(bool_type()));
        }
        diagnose(expression.operator_position, "unknown binary operator");
        return annotate(expression, invalid_value());
    }

    TypedValue check_field_access(Expr& expression, FlowState& state) {
        const auto receiver = check_expr(*expression.receiver, state);
        const auto field = load_field(receiver, expression.name, expression.position, state);
        return annotate(expression, field);
    }

    TypedValue check_call(Expr& expression, FlowState& state) {
        std::vector<TypedValue> arguments;
        arguments.reserve(expression.arguments.size());
        for (auto& argument : expression.arguments) {
            arguments.push_back(check_expr(*argument, state));
        }

        const auto* function = find_function(expression.name);
        if (function == nullptr) {
            diagnose(expression.position,
                     "cannot call non-function name '" + expression.name + "'");
            return annotate(expression, invalid_value());
        }

        expression.callee_index = function->index;
        if (arguments.size() != function->parameters.size()) {
            diagnose(expression.position,
                     "function '" + expression.name + "' expects " +
                         std::to_string(function->parameters.size()) +
                         " argument(s) but got " + std::to_string(arguments.size()));
            return annotate(expression, invalid_value());
        }

        bool valid = true;
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            if (is_invalid(arguments[i].type)) {
                valid = false;
                continue;
            }
            if (!value_conforms_to_type(arguments[i], function->parameters[i], state)) {
                diagnose(expression.arguments[i]->position,
                         "argument " + std::to_string(i + 1) + " of function '" +
                             expression.name + "' expects " +
                             type_name(function->parameters[i]) + " but got " +
                             type_name(arguments[i].type));
                valid = false;
            }
        }
        if (!valid) {
            return annotate(expression, invalid_value());
        }
        return annotate(expression, value_from_type(function->return_type));
    }

    TypedValue check_lvalue_prefix(LValue& lvalue, FlowState& state,
                                   std::size_t field_count) {
        auto* local = find_local(state, lvalue.base_name);
        if (local == nullptr) {
            diagnose(lvalue.base_position, "undefined variable '" + lvalue.base_name + "'");
            return invalid_value();
        }
        lvalue.local_index = local->index;
        if (!local->initialized) {
            diagnose(lvalue.base_position,
                     "local '" + lvalue.base_name + "' may be uninitialized");
            return invalid_value();
        }

        auto current = local->value;
        for (std::size_t i = 0; i < field_count; ++i) {
            const auto& field = lvalue.fields[i];
            current = load_field(current, field.name, field.position, state);
            if (is_invalid(current.type)) {
                return current;
            }
        }
        return current;
    }

    TypedValue load_field(const TypedValue& receiver, const std::string& field,
                          SourcePosition position, const FlowState& state) {
        if (is_invalid(receiver.type)) {
            return invalid_value();
        }
        if (!is_pair(receiver.type)) {
            diagnose(position, "field access requires pair");
            return invalid_value();
        }
        const auto loaded_field = try_load_field(receiver, field, state);
        if (!loaded_field.has_value()) {
            diagnose(position, "pair field type is unknown");
            return invalid_value();
        }
        return *loaded_field;
    }

    void store_field(const TypedValue& receiver, const std::string& field,
                     const TypedValue& assigned, FlowState& state) {
        for (const auto site : receiver.object_sites) {
            assert(site < state.fields_by_site.size());
            assert(state.fields_by_site[site].has_value());
            auto& fields = *state.fields_by_site[site];
            if (field == "left") {
                fields.left = join_values(fields.left, assigned);
            } else {
                fields.right = join_values(fields.right, assigned);
            }
        }
    }

    std::size_t pair_site_count_{0};
    FlowState state_;
    std::vector<FunctionSymbol> functions_;
    std::vector<Diagnostic> diagnostics_;
};

} // namespace

TypeCheckResult check_program(Program& program) {
    TypeChecker checker(program.pair_site_count);
    auto result_type = checker.check(program);
    return TypeCheckResult{std::move(result_type),
                            std::vector<Diagnostic>(checker.diagnostics().begin(),
                                                    checker.diagnostics().end())};
}

} // namespace lang::frontend::detail
