#include "type_checker_internal.hpp"

#include "diagnostics.hpp"

#include <algorithm>
#include <cassert>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace lang::frontend::detail {
namespace {

struct TypedValue {
    TypeSpec type{invalid_type()};
    std::set<std::size_t> object_sites;
    bool includes_nil{false};
    std::shared_ptr<TypedValue> array_element;
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
    bool immutable_loop_variable{false};
    bool immutable_match_binding{false};
};

struct FlowState {
    std::vector<LocalState> locals;
    std::vector<std::optional<FieldState>> fields_by_site;
};

bool operator==(const TypedValue& lhs, const TypedValue& rhs) {
    const bool elements_equal =
        (lhs.array_element == nullptr && rhs.array_element == nullptr) ||
        (lhs.array_element != nullptr && rhs.array_element != nullptr &&
         *lhs.array_element == *rhs.array_element);
    return lhs.type == rhs.type && lhs.object_sites == rhs.object_sites &&
           lhs.includes_nil == rhs.includes_nil && elements_equal;
}

bool operator==(const FieldState& lhs, const FieldState& rhs) {
    return lhs.left == rhs.left && lhs.right == rhs.right;
}

bool operator==(const LocalState& lhs, const LocalState& rhs) {
    return lhs.name == rhs.name && lhs.declared_type == rhs.declared_type &&
           lhs.index == rhs.index && lhs.initialized == rhs.initialized &&
           lhs.value == rhs.value &&
           lhs.immutable_loop_variable == rhs.immutable_loop_variable &&
           lhs.immutable_match_binding == rhs.immutable_match_binding;
}

bool operator==(const FlowState& lhs, const FlowState& rhs) {
    return lhs.locals == rhs.locals && lhs.fields_by_site == rhs.fields_by_site;
}

TypedValue invalid_value() { return TypedValue{invalid_type(), {}, false, nullptr}; }
TypedValue scalar_value(TypeSpec type) {
    return TypedValue{std::move(type), {}, false, nullptr};
}
TypedValue nil_value() { return TypedValue{nil_type(), {}, true, nullptr}; }

TypedValue pair_value(TypeSpec type, std::size_t site) {
    TypedValue value;
    value.type = std::move(type);
    value.object_sites.insert(site);
    return value;
}

TypedValue array_value(TypeSpec type, TypedValue element) {
    TypedValue value;
    value.type = std::move(type);
    value.array_element = std::make_shared<TypedValue>(std::move(element));
    return value;
}

bool is_scalar_array_element_type(const TypeSpec& type) {
    return type.kind == TypeSpec::Kind::Int64 || type.kind == TypeSpec::Kind::Bool;
}

bool is_reference_array_element_type(const TypeSpec& type) {
    return type.kind == TypeSpec::Kind::Pair || type.kind == TypeSpec::Kind::Named ||
           type.kind == TypeSpec::Kind::Record ||
           type.kind == TypeSpec::Kind::Variant ||
           type.kind == TypeSpec::Kind::Array || type.kind == TypeSpec::Kind::Str ||
           type.kind == TypeSpec::Kind::Function || type.kind == TypeSpec::Kind::Map ||
           type.kind == TypeSpec::Kind::Weak ||
           type.kind == TypeSpec::Kind::Ephemeron;
}

bool is_object_type(const TypeSpec& type) {
    return is_reference_array_element_type(type);
}

bool is_weak_target_type(const TypeSpec& type) {
    return type.kind == TypeSpec::Kind::Pair ||
           type.kind == TypeSpec::Kind::Named ||
           type.kind == TypeSpec::Kind::Record ||
           type.kind == TypeSpec::Kind::Variant ||
           type.kind == TypeSpec::Kind::Array ||
           type.kind == TypeSpec::Kind::Str ||
           type.kind == TypeSpec::Kind::Function ||
           type.kind == TypeSpec::Kind::Map ||
           type.kind == TypeSpec::Kind::Weak ||
           type.kind == TypeSpec::Kind::Ephemeron;
}

bool is_valid_map_key_type(const TypeSpec& type) {
    return type.kind == TypeSpec::Kind::Int64 ||
           type.kind == TypeSpec::Kind::Bool ||
           type.kind == TypeSpec::Kind::Str;
}

bool is_known_nonnil_reference(const TypedValue& value) {
    return is_object_type(value.type) && !value.includes_nil;
}

TypedValue value_from_type(TypeSpec type) {
    if (type.kind == TypeSpec::Kind::Array && type.element != nullptr) {
        auto element = value_from_type(*type.element);
        if (is_reference_array_element_type(*type.element)) {
            element.includes_nil = false;
        }
        return array_value(std::move(type), std::move(element));
    }
    const bool includes_nil = type.kind == TypeSpec::Kind::Named ||
                              type.kind == TypeSpec::Kind::Record ||
                              type.kind == TypeSpec::Kind::Variant;
    return TypedValue{std::move(type), {}, includes_nil, nullptr};
}

TypedValue value_as_declared_type(const TypedValue& value, TypeSpec declared_type) {
    TypedValue coerced;
    coerced.type = std::move(declared_type);
    if (is_object_type(coerced.type)) {
        coerced.object_sites = value.object_sites;
        coerced.includes_nil = value.includes_nil || value.type.kind == TypeSpec::Kind::Nil;
    }
    if (coerced.type.kind == TypeSpec::Kind::Array &&
        coerced.type.element != nullptr) {
        if (value.array_element != nullptr) {
            coerced.array_element =
                std::make_shared<TypedValue>(value_as_declared_type(
                    *value.array_element, *coerced.type.element));
        } else {
            coerced.array_element =
                std::make_shared<TypedValue>(value_from_type(*coerced.type.element));
        }
        if (is_reference_array_element_type(*coerced.type.element)) {
            coerced.array_element->includes_nil = false;
        }
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
    if (is_object_type(result.type)) {
        result.object_sites = lhs.object_sites;
        result.object_sites.insert(rhs.object_sites.begin(), rhs.object_sites.end());
        result.includes_nil = lhs.includes_nil || rhs.includes_nil ||
                              lhs.type.kind == TypeSpec::Kind::Nil ||
                              rhs.type.kind == TypeSpec::Kind::Nil;
    }
    if (result.type.kind == TypeSpec::Kind::Array) {
        if (lhs.array_element != nullptr && rhs.array_element != nullptr) {
            result.array_element = std::make_shared<TypedValue>(
                join_values(*lhs.array_element, *rhs.array_element));
        } else if (lhs.array_element != nullptr) {
            result.array_element = lhs.array_element;
        } else {
            result.array_element = rhs.array_element;
        }
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
    std::size_t closure_layout_index{0};
    std::vector<TypeSpec> parameters;
    TypeSpec return_type{invalid_type()};
};

struct TypeSymbol {
    std::string name;
    SourcePosition position;
    std::size_t index{0};
    TypeSpec body{invalid_type()};
};

struct RecordSymbol {
    std::string name;
    SourcePosition position;
    std::size_t index{0};
    RecordDecl* declaration{nullptr};
};

struct VariantSymbol {
    std::string name;
    SourcePosition position;
    std::size_t index{0};
    VariantDecl* declaration{nullptr};
};

class TypeChecker {
public:
    explicit TypeChecker(std::size_t pair_site_count) : pair_site_count_(pair_site_count) {
        state_.fields_by_site.resize(pair_site_count_);
    }

    TypeSpec check(Program& program) {
        collect_type_symbols(program);
        collect_function_symbols(program);
        for (std::size_t i = 0; i < program.lambdas.size(); ++i) {
            program.lambdas[i]->function_index =
                1 + program.functions.size() + i;
            program.lambdas[i]->closure_layout_index =
                program.functions.size() + i;
        }
        for (auto& function : program.functions) {
            check_function(function);
        }

        LocalContext entry_context;
        auto* previous_local_context = local_context_;
        local_context_ = &entry_context;
        state_ = initial_state();
        (void)check_block(program.statements, state_, true);
        const auto result = check_expr(*program.result, state_);
        if (result.type.kind == TypeSpec::Kind::Nil) {
            diagnose(program.result->position,
                     "nil literal requires a named object type context");
        } else if (result.includes_nil &&
                   result.type.kind != TypeSpec::Kind::Named &&
                   result.type.kind != TypeSpec::Kind::Record &&
                   result.type.kind != TypeSpec::Kind::Variant) {
            diagnose(program.result->position,
                     "program result requires non-nil value of type " +
                         type_name(result.type));
        }
        program.entry_local_count =
            static_cast<std::uint32_t>(entry_context.prototypes.size());
        local_context_ = previous_local_context;
        return result.type;
    }

    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const {
        return diagnostics_;
    }

    [[nodiscard]] std::uint32_t local_count() const {
        return static_cast<std::uint32_t>(state_.locals.size());
    }

private:
    struct LocalContext {
        std::vector<LocalState> prototypes;
    };

    struct CaptureContext {
        FlowState* outer_state{nullptr};
        LambdaExpr* lambda{nullptr};
        CaptureContext* parent{nullptr};
    };

    struct LoopFlowContext {
        std::vector<FlowState> break_states;
        std::vector<FlowState> continue_states;
    };

    FlowState initial_state() const {
        FlowState state;
        state.fields_by_site.resize(pair_site_count_);
        return state;
    }

    static std::string hidden_loop_local_name(std::uint32_t index) {
        return std::string("\x1f") + "for$" + std::to_string(index);
    }

    static std::string hidden_match_local_name(std::uint32_t index) {
        return std::string("\x1f") + "match$" + std::to_string(index);
    }

    void sync_locals(FlowState& state) const {
        assert(local_context_ != nullptr);
        while (state.locals.size() < local_context_->prototypes.size()) {
            state.locals.push_back(
                local_context_->prototypes[state.locals.size()]);
        }
    }

    std::uint32_t allocate_local(FlowState& state, std::string name,
                                 TypeSpec type, TypedValue value,
                                 SourcePosition position,
                                 bool immutable_loop_variable = false) {
        assert(local_context_ != nullptr);
        const auto index = static_cast<std::uint32_t>(
            local_context_->prototypes.size());
        LocalState local{std::move(name), std::move(type), index, true,
                         std::move(value), position,
                         immutable_loop_variable, false};
        local_context_->prototypes.push_back(local);
        sync_locals(state);
        state.locals[index] = std::move(local);
        return index;
    }

    void register_existing_loop_local(std::uint32_t index,
                                      const TypeSpec& type,
                                      SourcePosition position) {
        assert(local_context_ != nullptr);
        if (index < local_context_->prototypes.size()) {
            return;
        }
        assert(index == local_context_->prototypes.size() &&
               "loop-local indexes must be allocated in source order");
        local_context_->prototypes.push_back(LocalState{
            hidden_loop_local_name(index), type, index, false,
            value_from_type(type), position, false, false});
    }

    void collect_type_symbols(Program& program) {
        std::set<std::string> nominal_names;
        for (std::size_t i = 0; i < program.types.size(); ++i) {
            auto& declaration = program.types[i];
            if (!nominal_names.insert(declaration.name).second) {
                diagnose(declaration.position,
                         "type '" + declaration.name + "' is already defined");
                continue;
            }

            TypeSymbol symbol;
            symbol.name = declaration.name;
            symbol.position = declaration.position;
            symbol.index = types_.size();
            declaration.type_index = symbol.index;
            types_.push_back(std::move(symbol));
        }

        for (auto& declaration : program.records) {
            if (!nominal_names.insert(declaration.name).second) {
                diagnose(declaration.position,
                         "type '" + declaration.name + "' is already defined");
                continue;
            }
            RecordSymbol symbol;
            symbol.name = declaration.name;
            symbol.position = declaration.position;
            symbol.index = records_.size();
            symbol.declaration = &declaration;
            declaration.layout_index = symbol.index;
            records_.push_back(std::move(symbol));
        }

        for (auto& declaration : program.variants) {
            if (!nominal_names.insert(declaration.name).second) {
                diagnose(declaration.position,
                         "type '" + declaration.name + "' is already defined");
                continue;
            }
            VariantSymbol symbol;
            symbol.name = declaration.name;
            symbol.position = declaration.position;
            symbol.index = variants_.size();
            symbol.declaration = &declaration;
            declaration.layout_index = symbol.index;
            variants_.push_back(std::move(symbol));
        }

        for (auto& declaration : program.types) {
            resolve_type(declaration.body);
            if (!declaration.body.has_pair_fields()) {
                diagnose(declaration.position,
                         "type '" + declaration.name +
                             "' must be declared as pair<...>");
                continue;
            }
            if (declaration.type_index < types_.size() &&
                types_[declaration.type_index].name == declaration.name) {
                types_[declaration.type_index].body = declaration.body;
            }
        }


        for (auto& declaration : program.records) {
            std::set<std::string> field_names;
            for (auto& field : declaration.fields) {
                if (!field_names.insert(field.name).second) {
                    diagnose(field.position,
                             "record field '" + field.name +
                                 "' is already defined in '" +
                                 declaration.name + "'");
                }
                resolve_type(field.type);
            }
        }


        for (auto& declaration : program.variants) {
            std::set<std::string> case_names;
            for (auto& variant_case : declaration.cases) {
                if (!case_names.insert(variant_case.name).second) {
                    diagnose(variant_case.position,
                             "variant case '" + variant_case.name +
                                 "' is already defined in '" +
                                 declaration.name + "'");
                }
                for (auto& field : variant_case.fields) {
                    resolve_type(field);
                }
            }
        }

        for (auto& function : program.functions) {
            for (auto& parameter : function.parameters) {
                resolve_type(parameter.type);
            }
            resolve_type(function.return_type);
            for (auto& statement : function.statements) {
                resolve_statement_types(statement);
            }
        }
        for (auto& statement : program.statements) {
            resolve_statement_types(statement);
        }
    }

    void collect_function_symbols(Program& program) {
        for (std::size_t i = 0; i < program.functions.size(); ++i) {
            auto& declaration = program.functions[i];
            declaration.function_index = i + 1;
            declaration.closure_layout_index = i;
            if (find_function(declaration.name) != nullptr) {
                diagnose(declaration.position,
                         "function '" + declaration.name + "' is already defined");
                continue;
            }

            FunctionSymbol symbol;
            symbol.name = declaration.name;
            symbol.position = declaration.position;
            symbol.index = declaration.function_index;
            symbol.closure_layout_index = declaration.closure_layout_index;
            symbol.return_type = declaration.return_type;
            for (const auto& parameter : declaration.parameters) {
                symbol.parameters.push_back(parameter.type);
            }
            functions_.push_back(std::move(symbol));
        }
    }

    void check_function(FunctionDecl& function) {
        LocalContext context;
        auto* previous_local_context = local_context_;
        local_context_ = &context;
        FlowState state = initial_state();
        for (auto& parameter : function.parameters) {
            if (find_local(state, parameter.name) != nullptr) {
                diagnose(parameter.position,
                         "parameter '" + parameter.name + "' is already defined");
                continue;
            }
            parameter.local_index = allocate_local(
                state, parameter.name, parameter.type,
                value_from_type(parameter.type), parameter.position);
        }

        (void)check_block(function.statements, state, true);
        const auto result = check_expr(*function.result, state);
        if (!is_invalid(result.type) &&
            !value_conforms_to_type(result, function.return_type, state)) {
            diagnose(function.result->position,
                     "function '" + function.name + "' returns " +
                         type_name(result.type) + " but is declared " +
                         type_name(function.return_type));
        }
        function.local_count =
            static_cast<std::uint32_t>(context.prototypes.size());
        local_context_ = previous_local_context;
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

    TypeSymbol* find_type(const std::string& name) {
        for (auto& type : types_) {
            if (type.name == name) {
                return &type;
            }
        }
        return nullptr;
    }

    RecordSymbol* find_record(const std::string& name) {
        for (auto& record : records_) {
            if (record.name == name) {
                return &record;
            }
        }
        return nullptr;
    }

    const RecordSymbol* find_record(const std::string& name) const {
        for (const auto& record : records_) {
            if (record.name == name) {
                return &record;
            }
        }
        return nullptr;
    }

    const RecordSymbol* find_record(const TypeSpec& type) const {
        if (type.kind != TypeSpec::Kind::Record ||
            !type.record_layout_index.has_value() ||
            *type.record_layout_index >= records_.size()) {
            return nullptr;
        }
        return &records_[*type.record_layout_index];
    }

    VariantSymbol* find_variant(const std::string& name) {
        for (auto& variant : variants_) {
            if (variant.name == name) {
                return &variant;
            }
        }
        return nullptr;
    }

    const VariantSymbol* find_variant(const TypeSpec& type) const {
        if (type.kind != TypeSpec::Kind::Variant ||
            !type.variant_layout_index.has_value() ||
            *type.variant_layout_index >= variants_.size()) {
            return nullptr;
        }
        return &variants_[*type.variant_layout_index];
    }

    const TypeSymbol* find_type(const TypeSpec& type) const {
        if (type.kind != TypeSpec::Kind::Named || !type.named_type_index.has_value() ||
            *type.named_type_index >= types_.size()) {
            return nullptr;
        }
        return &types_[*type.named_type_index];
    }

    void resolve_type(TypeSpec& type) {
        if (type.has_pair_fields()) {
            resolve_type(*type.left);
            resolve_type(*type.right);
            return;
        }
        if (type.kind == TypeSpec::Kind::Array && type.element != nullptr) {
            resolve_type(*type.element);
            return;
        }
        if (type.kind == TypeSpec::Kind::Function &&
            type.function_return != nullptr) {
            for (auto& parameter : type.function_parameters) {
                resolve_type(parameter);
            }
            resolve_type(*type.function_return);
            return;
        }
        if (type.kind == TypeSpec::Kind::Map && type.key != nullptr &&
            type.value != nullptr) {
            resolve_type(*type.key);
            resolve_type(*type.value);
            if (!is_invalid(*type.key) && !is_valid_map_key_type(*type.key)) {
                diagnose(type.key->position,
                         "map key type must be i64, bool, or str");
            }
            return;
        }
        if (type.kind == TypeSpec::Kind::Weak &&
            type.weak_target != nullptr) {
            resolve_type(*type.weak_target);
            if (!is_invalid(*type.weak_target) &&
                !is_weak_target_type(*type.weak_target)) {
                diagnose(type.weak_target->position,
                         "weak target type must be an object type");
                type = invalid_type();
            }
            return;
        }
        if (type.kind == TypeSpec::Kind::Ephemeron && type.key != nullptr &&
            type.value != nullptr) {
            resolve_type(*type.key);
            resolve_type(*type.value);
            if (!is_invalid(*type.key) && !is_weak_target_type(*type.key)) {
                diagnose(type.key->position,
                         "ephemeron key type must be an object type");
                type = invalid_type();
            }
            return;
        }
        if (type.kind == TypeSpec::Kind::Record ||
            type.kind == TypeSpec::Kind::Variant) {
            return;
        }
        if (type.kind != TypeSpec::Kind::Named) {
            return;
        }
        auto* symbol = find_type(type.name);
        if (symbol != nullptr) {
            type.named_type_index = symbol->index;
            return;
        }
        if (auto* record = find_record(type.name); record != nullptr) {
            type = record_type(record->name, record->index, type.position);
            return;
        }
        auto* variant = find_variant(type.name);
        if (variant == nullptr) {
            diagnose(type.position, "unknown type '" + type.name + "'");
            type = invalid_type();
            return;
        }
        type = variant_type(variant->name, variant->index, type.position);
    }

    void resolve_statement_types(Statement& statement) {
        if (statement.kind == Statement::Kind::Let) {
            resolve_type(statement.declared_type);
        }
        if (statement.kind == Statement::Kind::TryCatch) {
            resolve_type(statement.catch_type);
        }
        for (auto& inner : statement.then_branch) {
            resolve_statement_types(inner);
        }
        for (auto& inner : statement.else_branch) {
            resolve_statement_types(inner);
        }
        for (auto& inner : statement.body) {
            resolve_statement_types(inner);
        }
        for (auto& arm : statement.match_arms) {
            for (auto& inner : arm.body) {
                resolve_statement_types(inner);
            }
        }
        for (auto& inner : statement.catch_body) {
            resolve_statement_types(inner);
        }
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

    std::optional<std::size_t> find_capture_index(
        const LambdaExpr& lambda, const std::string& name) const {
        for (std::size_t i = 0; i < lambda.captures.size(); ++i) {
            if (lambda.captures[i].name == name) {
                return i;
            }
        }
        return std::nullopt;
    }

    bool capture_name_is_visible(const CaptureContext& context,
                                 const std::string& name) const {
        if (find_capture_index(*context.lambda, name).has_value() ||
            find_local(*context.outer_state, name) != nullptr) {
            return true;
        }
        return context.parent != nullptr &&
               capture_name_is_visible(*context.parent, name);
    }

    std::optional<TypedValue> try_ensure_capture(
        CaptureContext& context, const std::string& name,
        SourcePosition use_position) {
        if (const auto existing = find_capture_index(*context.lambda, name);
            existing.has_value()) {
            auto captured =
                value_from_type(context.lambda->captures[*existing].type);
            captured.includes_nil = false;
            return captured;
        }

        if (auto* local = find_local(*context.outer_state, name); local != nullptr) {
            if (!local->initialized) {
                diagnose(use_position,
                         "captured local '" + name + "' may be uninitialized");
                return invalid_value();
            }
            if (local->value.includes_nil) {
                diagnose(use_position,
                         "captured local '" + name +
                             "' requires non-nil value");
                return invalid_value();
            }
            context.lambda->captures.push_back(
                CaptureSpec{name, local->declaration_position,
                            local->declared_type, local->index, false});
            auto captured = value_from_type(local->declared_type);
            captured.includes_nil = false;
            return captured;
        }

        if (context.parent == nullptr) {
            return std::nullopt;
        }
        auto parent_value = try_ensure_capture(*context.parent, name, use_position);
        if (!parent_value.has_value()) {
            return std::nullopt;
        }
        if (is_invalid(parent_value->type)) {
            return invalid_value();
        }
        const auto parent_index =
            find_capture_index(*context.parent->lambda, name);
        assert(parent_index.has_value() &&
               "parent capture resolution must install capture metadata");
        context.lambda->captures.push_back(
            CaptureSpec{name, use_position,
                        context.parent->lambda->captures[*parent_index].type,
                        static_cast<std::uint32_t>(*parent_index), true});
        auto captured = value_from_type(context.lambda->captures.back().type);
        captured.includes_nil = false;
        return captured;
    }

    void diagnose(SourcePosition position, std::string message) {
        add_diagnostic(diagnostics_, position, std::move(message));
    }

    std::optional<TypedValue> try_load_field(const TypedValue& receiver,
                                             const std::string& field,
                                             const FlowState& state) const {
        if (!is_pair(receiver.type) || receiver.includes_nil) {
            return std::nullopt;
        }

        std::optional<TypedValue> loaded;
        if (receiver.type.has_pair_fields()) {
            const auto& field_type = field == "left" ? *receiver.type.left
                                                     : *receiver.type.right;
            loaded = value_from_type(field_type);
        }
        if (receiver.type.kind == TypeSpec::Kind::Named) {
            const auto* symbol = find_type(receiver.type);
            if (symbol == nullptr || !symbol->body.has_pair_fields()) {
                return std::nullopt;
            }
            const auto& field_type = field == "left" ? *symbol->body.left
                                                     : *symbol->body.right;
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
        std::set<std::string> assumptions;
        return value_conforms_to_type(value, target, state, assumptions);
    }

    std::string conformance_key(const TypedValue& value,
                                const TypeSpec& target) const {
        std::ostringstream out;
        out << type_name(value.type) << "|";
        for (const auto site : value.object_sites) {
            out << site << ",";
        }
        out << "->" << type_name(target);
        if (target.named_type_index.has_value()) {
            out << "#" << *target.named_type_index;
        }
        return out.str();
    }

    bool has_nonnil_evidence(const TypedValue& value) const {
        return is_pair(value.type) && (value.type.kind == TypeSpec::Kind::Named ||
                                      value.type.has_pair_fields() ||
                                      !value.object_sites.empty());
    }

    bool value_conforms_to_type(const TypedValue& value, const TypeSpec& target,
                                const FlowState& state,
                                std::set<std::string>& assumptions) const {
        if (is_invalid(value.type) || is_invalid(target)) {
            return false;
        }
        if (target.kind == TypeSpec::Kind::Array) {
            if (value.type.kind != TypeSpec::Kind::Array ||
                value.includes_nil || target.element == nullptr ||
                value.type.element == nullptr) {
                return false;
            }
            if (is_scalar_array_element_type(*target.element) !=
                is_scalar_array_element_type(*value.type.element)) {
                return false;
            }
            if (value.array_element != nullptr) {
                return value_conforms_to_type(*value.array_element, *target.element,
                                              state, assumptions);
            }
            return *value.type.element == *target.element;
        }
        if (target.kind == TypeSpec::Kind::Map) {
            return !value.includes_nil && value.type == target &&
                   target.key != nullptr && target.value != nullptr &&
                   is_valid_map_key_type(*target.key);
        }
        if (target.kind == TypeSpec::Kind::Weak) {
            return !value.includes_nil && value.type == target &&
                   target.weak_target != nullptr &&
                   is_weak_target_type(*target.weak_target);
        }
        if (target.kind == TypeSpec::Kind::Ephemeron) {
            return !value.includes_nil && value.type == target &&
                   target.key != nullptr && target.value != nullptr &&
                   is_weak_target_type(*target.key);
        }
        if (target.kind == TypeSpec::Kind::Record) {
            if (value.type.kind == TypeSpec::Kind::Nil) {
                return true;
            }
            return value.type == target;
        }
        if (target.kind == TypeSpec::Kind::Variant) {
            if (value.type.kind == TypeSpec::Kind::Nil) {
                return true;
            }
            return value.type == target;
        }
        if (target.kind == TypeSpec::Kind::Named) {
            if (value.type.kind == TypeSpec::Kind::Nil) {
                return true;
            }
            if (!is_pair(value.type)) {
                return false;
            }
            if (!has_nonnil_evidence(value)) {
                return value.includes_nil;
            }
            const auto* symbol = find_type(target);
            if (symbol == nullptr) {
                return false;
            }
            const auto key = conformance_key(value, target);
            // Termination mirrors the verifier: named recursive types are a finite
            // graph, and pair allocation-site facts are finite. Re-entering the same
            // value-shape-to-named-type obligation is the coinductive case.
            if (!assumptions.insert(key).second) {
                return true;
            }
            auto nonnil = value;
            nonnil.includes_nil = false;
            return value_conforms_to_type(nonnil, symbol->body, state, assumptions);
        }
        if (target.kind == TypeSpec::Kind::Int64 ||
            target.kind == TypeSpec::Kind::Bool ||
            target.kind == TypeSpec::Kind::Str ||
            target.kind == TypeSpec::Kind::Function) {
            return !value.includes_nil && value.type == target;
        }
        if (value.includes_nil || value.type.kind == TypeSpec::Kind::Nil) {
            return false;
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
        return value_conforms_to_type(*left, *target.left, state, assumptions) &&
               value_conforms_to_type(*right, *target.right, state, assumptions);
    }

    bool check_statement(Statement& statement, FlowState& state, bool allow_let) {
        switch (statement.kind) {
        case Statement::Kind::Let:
            check_let(statement, state, allow_let);
            return true;
        case Statement::Kind::Assign:
            check_assignment(statement, state);
            return true;
        case Statement::Kind::If:
            return check_if(statement, state);
        case Statement::Kind::While:
            return check_while(statement, state);
        case Statement::Kind::ForIn:
            check_for_in(statement, state);
            return true;
        case Statement::Kind::Match:
            return check_match(statement, state);
        case Statement::Kind::Throw: {
            const auto value = check_expr(*statement.value, state);
            if (value.type.kind != TypeSpec::Kind::Variant || value.includes_nil) {
                diagnose(statement.position,
                         "throw requires a proven non-nil variant value");
            }
            return false;
        }
        case Statement::Kind::TryCatch:
            return check_try_catch(statement, state);
        case Statement::Kind::Break:
        case Statement::Kind::Continue:
            return check_loop_control(statement, state);
        case Statement::Kind::Print: {
            const auto value = check_expr(*statement.value, state);
            if (!is_invalid(value.type) &&
                (value.type != str_type() || value.includes_nil)) {
                diagnose(statement.value->position,
                         value.type == str_type()
                             ? "print requires non-nil str"
                             : "print expects str");
            }
            return true;
        }
        case Statement::Kind::EphemeronSet: {
            const auto receiver = check_expr(*statement.initializer, state);
            const auto value = check_expr(*statement.value, state);
            if (receiver.includes_nil ||
                receiver.type.kind != TypeSpec::Kind::Ephemeron ||
                receiver.type.value == nullptr) {
                diagnose(statement.position, "set_value requires ephemeron");
            } else if (!value_conforms_to_type(value, *receiver.type.value, state)) {
                diagnose(statement.value->position,
                         "set_value expects " + type_name(*receiver.type.value) +
                             " but got " + type_name(value.type));
            }
            return true;
        }
        }
        return true;
    }

    bool check_try_catch(Statement& statement, FlowState& state) {
        if (statement.catch_type.kind != TypeSpec::Kind::Variant) {
            diagnose(statement.catch_type.position,
                     "catch type must be a nominal variant");
        }
        auto try_state = state;
        const bool try_falls = check_block(statement.body, try_state, false);
        if (!statement.catch_local_allocated) {
            statement.catch_local_index = static_cast<std::uint32_t>(
                local_context_->prototypes.size());
            local_context_->prototypes.push_back(LocalState{
                hidden_match_local_name(statement.catch_local_index),
                statement.catch_type, statement.catch_local_index, false,
                value_from_type(statement.catch_type), statement.catch_position,
                false, false});
            statement.catch_local_allocated = true;
        }
        sync_locals(state);
        sync_locals(try_state);
        auto catch_state = state;
        catch_state.locals[statement.catch_local_index].name = statement.catch_name;
        catch_state.locals[statement.catch_local_index].initialized = true;
        auto caught = value_from_type(statement.catch_type);
        caught.includes_nil = false;
        catch_state.locals[statement.catch_local_index].value = std::move(caught);
        catch_state.locals[statement.catch_local_index].immutable_match_binding = true;
        const bool catch_falls = check_block(statement.catch_body, catch_state, false);
        sync_locals(catch_state);
        catch_state.locals[statement.catch_local_index].name =
            hidden_match_local_name(statement.catch_local_index);
        catch_state.locals[statement.catch_local_index].initialized = false;
        catch_state.locals[statement.catch_local_index].immutable_match_binding = false;
        sync_locals(try_state);
        sync_locals(catch_state);
        if (try_falls && catch_falls) state = join_states(try_state, catch_state);
        else if (try_falls) state = std::move(try_state);
        else if (catch_falls) state = std::move(catch_state);
        return try_falls || catch_falls;
    }

    bool check_block(std::vector<Statement>& statements, FlowState& state,
                     bool allow_let) {
        bool falls_through = true;
        for (auto& statement : statements) {
            if (!falls_through) {
                break;
            }
            falls_through = check_statement(statement, state, allow_let);
        }
        return falls_through;
    }

    bool check_loop_control(const Statement& statement,
                            const FlowState& state) {
        if (loop_contexts_.empty()) {
            diagnose(statement.position,
                     statement.kind == Statement::Kind::Break
                         ? "'break' is only allowed inside a loop"
                         : "'continue' is only allowed inside a loop");
            return true;
        }
        auto& context = *loop_contexts_.back();
        if (statement.kind == Statement::Kind::Break) {
            context.break_states.push_back(state);
        } else {
            context.continue_states.push_back(state);
        }
        return false;
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
        const bool nullable_weak_get_binding =
            initializer.includes_nil &&
            initializer.type == statement.declared_type &&
            is_weak_target_type(statement.declared_type);
        if (!is_invalid(initializer.type) && !nullable_weak_get_binding &&
            !value_conforms_to_type(initializer, statement.declared_type,
                                    state)) {
            diagnose(statement.equals_position,
                     "cannot initialize local '" + statement.name + "' of type " +
                         type_name(statement.declared_type) + " with " +
                         type_name(initializer.type));
            return;
        }

        statement.local_index = allocate_local(
            state, statement.name, statement.declared_type,
            value_as_declared_type(initializer, statement.declared_type),
            statement.position);
    }

    void check_assignment(Statement& statement, FlowState& state) {
        const auto assigned = check_expr(*statement.value, state);
        if (statement.target.steps.empty()) {
            auto* local = find_local(state, statement.target.base_name);
            if (local == nullptr) {
                if (capture_context_ != nullptr) {
                    const auto captured = try_ensure_capture(
                        *capture_context_, statement.target.base_name,
                        statement.target.base_position);
                    if (captured.has_value()) {
                        diagnose(statement.target.base_position,
                                 "cannot assign to immutable capture '" +
                                     statement.target.base_name + "'");
                        return;
                    }
                }
                diagnose(statement.target.base_position,
                         "undefined variable '" + statement.target.base_name + "'");
                return;
            }
            statement.target.local_index = local->index;
            if (local->immutable_loop_variable) {
                diagnose(statement.target.base_position,
                         "cannot assign to immutable loop variable '" +
                             local->name + "'");
                return;
            }
            if (local->immutable_match_binding) {
                diagnose(statement.target.base_position,
                         "cannot assign to immutable match binding '" +
                             local->name + "'");
                return;
            }
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
                                                 statement.target.steps.size() - 1);
        auto& final_step = statement.target.steps.back();
        if (is_invalid(receiver.type) || is_invalid(assigned.type)) {
            return;
        }
        final_step.receiver_type = receiver.type;

        if (receiver.includes_nil) {
            diagnose(final_step.position,
                     "assignment requires non-nil value of type " +
                         type_name(receiver.type));
            return;
        }

        if (final_step.kind == LValueStep::Kind::Index) {
            const auto index = check_expr(*final_step.index, state);
            if (receiver.type.kind == TypeSpec::Kind::Map &&
                receiver.type.key != nullptr && receiver.type.value != nullptr) {
                if (!is_invalid(index.type) &&
                    !value_conforms_to_type(index, *receiver.type.key, state)) {
                    diagnose(final_step.index->position,
                             "map key expects " + type_name(*receiver.type.key) +
                                 " but got " + type_name(index.type));
                    return;
                }
                statement.target.receiver_type = receiver.type;
                statement.target.element_type = *receiver.type.value;
                final_step.element_type = *receiver.type.value;
                if (!value_conforms_to_type(assigned, *receiver.type.value,
                                            state)) {
                    diagnose(statement.equals_position,
                             "cannot assign " + type_name(assigned.type) +
                                 " to map value of type " +
                                 type_name(*receiver.type.value));
                }
                return;
            }
            if (!is_invalid(index.type) && index.type != int64_type()) {
                diagnose(final_step.index->position,
                         receiver.type.kind == TypeSpec::Kind::Str
                             ? "string index must be i64"
                             : "array index must be i64");
                return;
            }
            if (receiver.type.kind == TypeSpec::Kind::Str) {
                diagnose(final_step.position, "string values are immutable");
                return;
            }
            if (receiver.type.kind != TypeSpec::Kind::Array ||
                receiver.type.element == nullptr) {
                diagnose(final_step.position, "indexing requires array");
                return;
            }
            statement.target.receiver_type = receiver.type;
            statement.target.element_type = *receiver.type.element;
            final_step.element_type = *receiver.type.element;
            if (!value_conforms_to_type(assigned, *receiver.type.element, state)) {
                diagnose(statement.equals_position,
                         "cannot assign " + type_name(assigned.type) +
                             " to array element of type " +
                             type_name(*receiver.type.element));
                return;
            }
            (void)require_nonnil_ref_array_element(
                assigned, *receiver.type.element, statement.equals_position);
            return;
        }

        const auto& field = final_step;
        if (receiver.type.kind == TypeSpec::Kind::Record) {
            auto existing = load_record_field(
                receiver, field.name, field.position,
                &final_step.record_layout_index,
                &final_step.record_field_index);
            if (is_invalid(existing.type)) {
                return;
            }
            statement.target.receiver_type = receiver.type;
            if (!value_conforms_to_type(assigned, existing.type, state)) {
                diagnose(statement.equals_position,
                         "cannot assign " + type_name(assigned.type) +
                             " to field '" + field.name + "' of type " +
                             type_name(existing.type));
            }
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

    void deactivate_match_bindings(const MatchArm& arm, FlowState& state) {
        sync_locals(state);
        for (const auto& binding : arm.bindings) {
            assert(binding.local_index < state.locals.size());
            auto& local = state.locals[binding.local_index];
            local.name = hidden_match_local_name(binding.local_index);
            local.initialized = false;
            local.immutable_match_binding = false;
        }
    }

    bool check_match(Statement& statement, FlowState& state) {
        const auto subject = check_expr(*statement.condition, state);
        const auto* variant = find_variant(subject.type);
        if (variant == nullptr) {
            if (!is_invalid(subject.type)) {
                diagnose(statement.condition->position,
                         "match subject must have variant type");
            }
            return true;
        }
        if (subject.includes_nil) {
            diagnose(statement.condition->position,
                     "match subject requires a proven non-nil variant");
        }
        statement.match_variant_layout_index = variant->index;

        std::set<std::string> seen_cases;
        std::vector<bool> covered(variant->declaration->cases.size(), false);
        bool valid = !subject.includes_nil;
        for (auto& arm : statement.match_arms) {
            std::optional<std::size_t> case_index;
            for (std::size_t i = 0; i < variant->declaration->cases.size(); ++i) {
                if (variant->declaration->cases[i].name == arm.case_name) {
                    case_index = i;
                    break;
                }
            }
            if (!case_index.has_value()) {
                diagnose(arm.position, "unknown variant case '" + arm.case_name + "'");
                valid = false;
                continue;
            }
            arm.case_index = *case_index;
            if (!seen_cases.insert(arm.case_name).second) {
                diagnose(arm.position, "duplicate match arm for case '" +
                                           arm.case_name + "'");
                valid = false;
            } else {
                covered[*case_index] = true;
            }
            const auto& fields = variant->declaration->cases[*case_index].fields;
            if (arm.bindings.size() != fields.size()) {
                diagnose(arm.position, "match arm '" + arm.case_name +
                                           "' expects " +
                                           std::to_string(fields.size()) +
                                           " bindings but got " +
                                           std::to_string(arm.bindings.size()));
                valid = false;
            }
        }
        std::string missing;
        for (std::size_t i = 0; i < covered.size(); ++i) {
            if (!covered[i]) {
                if (!missing.empty()) {
                    missing += ", ";
                }
                missing += variant->declaration->cases[i].name;
            }
        }
        if (!missing.empty()) {
            diagnose(statement.position,
                     "non-exhaustive match; missing cases: " + missing);
            valid = false;
        }

        if (!statement.match_locals_allocated) {
            for (auto& arm : statement.match_arms) {
                if (arm.case_index >= variant->declaration->cases.size()) {
                    continue;
                }
                const auto& fields = variant->declaration->cases[arm.case_index].fields;
                const auto count = std::min(fields.size(), arm.bindings.size());
                std::set<std::string> arm_names;
                for (std::size_t i = 0; i < count; ++i) {
                    auto& binding = arm.bindings[i];
                    bool conflict = false;
                    if (!arm_names.insert(binding.name).second) {
                        diagnose(binding.position, "duplicate match binding '" +
                                                       binding.name + "'");
                        conflict = true;
                    }
                    if (find_local(state, binding.name) != nullptr) {
                        diagnose(binding.position, "match binding '" + binding.name +
                                                       "' conflicts with an existing local");
                        conflict = true;
                    }
                    if (capture_context_ != nullptr &&
                        capture_name_is_visible(*capture_context_, binding.name)) {
                        diagnose(binding.position, "match binding '" + binding.name +
                                                       "' conflicts with an existing capture");
                        conflict = true;
                    }
                    (void)conflict;
                    binding.local_index = static_cast<std::uint32_t>(
                        local_context_->prototypes.size());
                    auto value = value_from_type(fields[i]);
                    local_context_->prototypes.push_back(LocalState{
                        hidden_match_local_name(binding.local_index), fields[i],
                        binding.local_index, false, std::move(value), binding.position,
                        false, false});
                    sync_locals(state);
                }
            }
            statement.match_locals_allocated = true;
        } else {
            sync_locals(state);
        }

        std::optional<FlowState> joined;
        bool any_falls = false;
        for (auto& arm : statement.match_arms) {
            if (arm.case_index >= variant->declaration->cases.size()) {
                continue;
            }
            sync_locals(state);
            auto arm_state = state;
            const auto& fields = variant->declaration->cases[arm.case_index].fields;
            const auto count = std::min(fields.size(), arm.bindings.size());
            for (std::size_t i = 0; i < count; ++i) {
                auto& local = arm_state.locals[arm.bindings[i].local_index];
                local.name = arm.bindings[i].name;
                local.declared_type = fields[i];
                local.initialized = true;
                local.value = value_from_type(fields[i]);
                local.declaration_position = arm.bindings[i].position;
                local.immutable_match_binding = true;
            }
            std::vector<std::pair<std::size_t, std::size_t>> loop_sizes;
            loop_sizes.reserve(loop_contexts_.size());
            for (auto* context : loop_contexts_) {
                loop_sizes.push_back({context->break_states.size(),
                                      context->continue_states.size()});
            }
            const bool falls = check_block(arm.body, arm_state, false);
            deactivate_match_bindings(arm, arm_state);
            for (std::size_t i = 0; i < loop_contexts_.size(); ++i) {
                auto& context = *loop_contexts_[i];
                for (auto& broken : context.break_states) {
                    sync_locals(broken);
                }
                for (auto& continued : context.continue_states) {
                    sync_locals(continued);
                }
                for (std::size_t j = loop_sizes[i].first;
                     j < context.break_states.size(); ++j) {
                    deactivate_match_bindings(arm, context.break_states[j]);
                }
                for (std::size_t j = loop_sizes[i].second;
                     j < context.continue_states.size(); ++j) {
                    deactivate_match_bindings(arm, context.continue_states[j]);
                }
            }
            if (falls) {
                if (joined.has_value()) {
                    sync_locals(*joined);
                }
                joined = joined.has_value() ? join_states(*joined, arm_state)
                                            : std::optional<FlowState>(arm_state);
                any_falls = true;
            }
        }
        if (any_falls) {
            state = std::move(*joined);
        }
        return valid && any_falls;
    }

    bool check_if(Statement& statement, FlowState& state) {
        const auto condition = check_expr(*statement.condition, state);
        if (!is_invalid(condition.type) && condition.type != bool_type()) {
            diagnose(statement.condition->position, "if condition must be bool");
        }

        auto then_state = state;
        auto else_state = state;
        refine_is_nil_condition(*statement.condition, then_state, else_state);
        const bool then_falls =
            check_block(statement.then_branch, then_state, false);
        sync_locals(then_state);
        sync_locals(else_state);
        const bool else_falls =
            check_block(statement.else_branch, else_state, false);
        sync_locals(then_state);
        sync_locals(else_state);
        if (then_falls && else_falls) {
            state = join_states(then_state, else_state);
        } else if (then_falls) {
            state = std::move(then_state);
        } else if (else_falls) {
            state = std::move(else_state);
        }
        return then_falls || else_falls;
    }

    bool check_while(Statement& statement, FlowState& state) {
        FlowState head = state;
        for (std::size_t iteration = 0; iteration < max_loop_iterations(state); ++iteration) {
            auto body_input = head;
            const auto condition = check_expr(*statement.condition, body_input);
            if (!is_invalid(condition.type) && condition.type != bool_type()) {
                diagnose(statement.condition->position, "while condition must be bool");
            }

            auto body_output = body_input;
            LoopFlowContext loop;
            loop_contexts_.push_back(&loop);
            const bool body_falls =
                check_block(statement.body, body_output, false);
            loop_contexts_.pop_back();

            sync_locals(head);
            sync_locals(body_output);
            for (auto& continued : loop.continue_states) {
                sync_locals(continued);
            }
            for (auto& broken : loop.break_states) {
                sync_locals(broken);
            }

            auto joined = head;
            if (body_falls) {
                joined = join_states(joined, body_output);
            }
            for (const auto& continued : loop.continue_states) {
                joined = join_states(joined, continued);
            }
            if (joined == head) {
                const bool condition_is_true_literal =
                    statement.condition->kind == Expr::Kind::BoolLiteral &&
                    statement.condition->bool_value;
                std::optional<FlowState> exit;
                if (!condition_is_true_literal) {
                    exit = head;
                }
                for (const auto& broken : loop.break_states) {
                    exit = exit.has_value() ? join_states(*exit, broken)
                                            : std::optional<FlowState>(broken);
                }
                if (exit.has_value()) {
                    state = std::move(*exit);
                    return true;
                }
                state = std::move(joined);
                return false;
            }
            head = std::move(joined);
        }
        diagnose(statement.position, "while type state did not reach a fixed point");
        state = std::move(head);
        return true;
    }

    bool loop_name_conflicts(const Statement& statement, std::size_t name_index,
                             FlowState& state) {
        const auto& name = statement.loop_names[name_index];
        const auto position = statement.loop_name_positions[name_index];
        if (find_local(state, name) != nullptr) {
            diagnose(position, "loop variable '" + name +
                                   "' conflicts with an existing local");
            return true;
        }
        for (std::size_t i = 0; i < name_index; ++i) {
            if (statement.loop_names[i] == name) {
                diagnose(position, "loop variable '" + name +
                                       "' conflicts with another loop variable");
                return true;
            }
        }
        if (capture_context_ != nullptr) {
            const auto captured = try_ensure_capture(*capture_context_, name,
                                                     position);
            if (captured.has_value()) {
                diagnose(position, "loop variable '" + name +
                                       "' conflicts with an existing capture");
                return true;
            }
        }
        return false;
    }

    void activate_loop_locals(Statement& statement, FlowState& state,
                              const std::vector<TypedValue>& values) {
        sync_locals(state);
        assert(statement.loop_local_indices.size() == values.size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            const auto index = statement.loop_local_indices[i];
            assert(index < state.locals.size());
            auto& local = state.locals[index];
            local.name = statement.loop_names[i];
            local.declared_type = values[i].type;
            local.initialized = true;
            local.value = values[i];
            local.declaration_position = statement.loop_name_positions[i];
            local.immutable_loop_variable = true;
        }
    }

    void deactivate_loop_locals(const Statement& statement, FlowState& state) {
        for (const auto index : statement.loop_local_indices) {
            assert(index < state.locals.size());
            auto& local = state.locals[index];
            local.name = hidden_loop_local_name(index);
            local.initialized = false;
            local.immutable_loop_variable = false;
        }
    }

    void check_for_in(Statement& statement, FlowState& state) {
        const auto iterable = check_expr(*statement.iterable, state);
        std::vector<TypeSpec> variable_types;
        std::vector<TypedValue> variable_values;
        if (statement.range_upper == nullptr && iterable.includes_nil &&
            (iterable.type.kind == TypeSpec::Kind::Array ||
             iterable.type.kind == TypeSpec::Kind::Map)) {
            diagnose(statement.iterable->position,
                     "for-in requires non-nil array or map");
            return;
        }
        if (statement.range_upper != nullptr) {
            const auto upper = check_expr(*statement.range_upper, state);
            if (!is_invalid(iterable.type) && iterable.type != int64_type()) {
                diagnose(statement.iterable->position,
                         "range lower bound must be i64");
            }
            if (!is_invalid(upper.type) && upper.type != int64_type()) {
                diagnose(statement.range_upper->position,
                         "range upper bound must be i64");
            }
            if (statement.loop_names.size() != 1) {
                const auto position = statement.loop_name_positions.size() > 1
                                          ? statement.loop_name_positions[1]
                                          : statement.position;
                diagnose(position,
                         "range iteration requires one loop variable");
                return;
            }
            variable_types.push_back(int64_type());
            variable_values.push_back(scalar_value(int64_type()));
        } else if (iterable.type.kind == TypeSpec::Kind::Array &&
                   iterable.type.element != nullptr) {
            if (statement.loop_names.size() != 1) {
                const auto position = statement.loop_name_positions.size() > 1
                                          ? statement.loop_name_positions[1]
                                          : statement.position;
                diagnose(position,
                         "array iteration requires one loop variable");
                return;
            }
            variable_types.push_back(*iterable.type.element);
            variable_values.push_back(
                load_array_element(iterable, statement.iterable->position));
        } else if (iterable.type.kind == TypeSpec::Kind::Map &&
                   iterable.type.key != nullptr && iterable.type.value != nullptr) {
            if (statement.loop_names.size() != 2) {
                const auto position = statement.loop_name_positions.empty()
                                          ? statement.position
                                          : statement.loop_name_positions.front();
                diagnose(position,
                         "map iteration requires two loop variables");
                return;
            }
            variable_types.push_back(*iterable.type.key);
            variable_types.push_back(*iterable.type.value);
            variable_values.push_back(value_from_type(*iterable.type.key));
            variable_values.push_back(value_from_type(*iterable.type.value));
        } else {
            if (!is_invalid(iterable.type)) {
                diagnose(statement.iterable->position,
                         "for-in requires array, map, or range");
            }
            return;
        }

        bool conflict = false;
        for (std::size_t i = 0; i < statement.loop_names.size(); ++i) {
            conflict = loop_name_conflicts(statement, i, state) || conflict;
        }
        if (conflict) {
            return;
        }

        if (!statement.loop_locals_allocated) {
            for (std::size_t i = 0; i < variable_types.size(); ++i) {
                const auto index = allocate_local(
                    state,
                    hidden_loop_local_name(static_cast<std::uint32_t>(
                        local_context_->prototypes.size())),
                    variable_types[i], variable_values[i],
                    statement.loop_name_positions[i]);
                state.locals[index].initialized = false;
                local_context_->prototypes[index].initialized = false;
                statement.loop_local_indices.push_back(index);
            }
            statement.loop_locals_allocated = true;
        } else {
            for (std::size_t i = 0; i < variable_types.size(); ++i) {
                register_existing_loop_local(statement.loop_local_indices[i],
                                             variable_types[i],
                                             statement.loop_name_positions[i]);
            }
            sync_locals(state);
        }

        FlowState head = state;
        for (std::size_t iteration = 0;
             iteration < max_loop_iterations(state); ++iteration) {
            auto body_output = head;
            activate_loop_locals(statement, body_output, variable_values);
            LoopFlowContext loop;
            loop_contexts_.push_back(&loop);
            const bool body_falls =
                check_block(statement.body, body_output, false);
            loop_contexts_.pop_back();
            if (body_falls) {
                deactivate_loop_locals(statement, body_output);
            }
            for (auto& continued : loop.continue_states) {
                deactivate_loop_locals(statement, continued);
            }
            for (auto& broken : loop.break_states) {
                deactivate_loop_locals(statement, broken);
            }
            sync_locals(head);
            sync_locals(body_output);
            for (auto& continued : loop.continue_states) {
                sync_locals(continued);
            }
            for (auto& broken : loop.break_states) {
                sync_locals(broken);
            }
            auto joined = head;
            if (body_falls) {
                joined = join_states(joined, body_output);
            }
            for (const auto& continued : loop.continue_states) {
                joined = join_states(joined, continued);
            }
            if (joined == head) {
                auto exit = head;
                for (const auto& broken : loop.break_states) {
                    exit = join_states(exit, broken);
                }
                state = std::move(exit);
                return;
            }
            head = std::move(joined);
        }
        diagnose(statement.position,
                 "for-in type state did not reach a fixed point");
        state = std::move(head);
    }

    void refine_is_nil_condition(const Expr& condition, FlowState& then_state,
                                 FlowState& else_state) {
        if (condition.kind != Expr::Kind::IsNil ||
            condition.receiver == nullptr ||
            condition.receiver->kind != Expr::Kind::Variable) {
            return;
        }
        auto* then_local = find_local(then_state, condition.receiver->name);
        auto* else_local = find_local(else_state, condition.receiver->name);
        if (then_local == nullptr || else_local == nullptr ||
            !is_object_type(then_local->declared_type)) {
            return;
        }

        then_local->value = value_as_declared_type(nil_value(),
                                                   then_local->declared_type);
        else_local->value.includes_nil = false;
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
        case Expr::Kind::StringLiteral:
            return annotate(expression, scalar_value(str_type()));
        case Expr::Kind::NilLiteral:
            return annotate(expression, nil_value());
        case Expr::Kind::Variable:
            return check_variable(expression, state);
        case Expr::Kind::PairLiteral:
            return check_pair_literal(expression, state);
        case Expr::Kind::RecordLiteral:
            return check_record_literal(expression, state);
        case Expr::Kind::VariantLiteral:
            return check_variant_literal(expression, state);
        case Expr::Kind::ArrayLiteral:
            return check_array_literal(expression, state);
        case Expr::Kind::ArraySized:
            return check_array_sized(expression, state);
        case Expr::Kind::ArrayIndex:
            return check_array_index(expression, state);
        case Expr::Kind::ArrayLen:
            return check_array_len(expression, state);
        case Expr::Kind::StrSub:
            return check_str_sub(expression, state);
        case Expr::Kind::Binary:
            return check_binary(expression, state);
        case Expr::Kind::Field:
            return check_field_access(expression, state);
        case Expr::Kind::Call:
            return check_call(expression, state);
        case Expr::Kind::Lambda:
            return check_lambda(expression, state);
        case Expr::Kind::IsNil:
            return check_is_nil(expression, state);
        case Expr::Kind::MapEmpty:
            return check_map_empty(expression);
        case Expr::Kind::MapHas:
            return check_map_has(expression, state);
        case Expr::Kind::WeakConstruct:
            return check_weak_construct(expression, state);
        case Expr::Kind::WeakGet:
            return check_weak_get(expression, state);
        case Expr::Kind::EphemeronConstruct:
            return check_ephemeron_construct(expression, state);
        case Expr::Kind::EphemeronKey:
        case Expr::Kind::EphemeronValue:
            return check_ephemeron_get(expression, state);
        case Expr::Kind::ToStr:
        case Expr::Kind::ToI64:
            return check_conversion(expression, state);
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
        if (local != nullptr) {
            expression.local_index = local->index;
            if (!local->initialized) {
                diagnose(expression.position,
                         "local '" + expression.name + "' may be uninitialized");
                return annotate(expression, invalid_value());
            }
            return annotate(expression, local->value);
        }

        if (capture_context_ != nullptr) {
            auto captured = try_ensure_capture(*capture_context_, expression.name,
                                               expression.position);
            if (captured.has_value()) {
                if (is_invalid(captured->type)) {
                    return annotate(expression, invalid_value());
                }
                const auto capture =
                    find_capture_index(*capture_context_->lambda,
                                       expression.name);
                assert(capture.has_value());
                expression.is_capture = true;
                expression.capture_index = *capture;
                return annotate(expression, *captured);
            }
        }

        if (const auto* function = find_function(expression.name);
            function != nullptr) {
            expression.is_function_reference = true;
            expression.callee_index = function->index;
            expression.closure_layout_index = function->closure_layout_index;
            return annotate(
                expression,
                value_from_type(function_type(function->parameters,
                                              function->return_type)));
        }

        diagnose(expression.position, "undefined variable '" + expression.name + "'");
        return annotate(expression, invalid_value());
    }

    TypedValue check_lambda(Expr& expression, FlowState& outer_state) {
        if (expression.lambda == nullptr) {
            diagnose(expression.position, "lambda metadata is missing");
            return annotate(expression, invalid_value());
        }
        auto& lambda = *expression.lambda;
        for (auto& parameter : lambda.parameters) {
            resolve_type(parameter.type);
        }
        resolve_type(lambda.return_type);
        for (auto& statement : lambda.statements) {
            resolve_statement_types(statement);
        }

        LocalContext local_context;
        auto* previous_local_context = local_context_;
        local_context_ = &local_context;
        FlowState state = initial_state();
        for (auto& parameter : lambda.parameters) {
            if (find_local(state, parameter.name) != nullptr) {
                diagnose(parameter.position,
                         "parameter '" + parameter.name + "' is already defined");
                continue;
            }
            parameter.local_index = allocate_local(
                state, parameter.name, parameter.type,
                value_from_type(parameter.type), parameter.position);
        }

        CaptureContext context{&outer_state, &lambda, capture_context_};
        auto* previous_context = capture_context_;
        capture_context_ = &context;
        auto outer_loop_contexts = std::move(loop_contexts_);
        loop_contexts_.clear();
        (void)check_block(lambda.statements, state, true);
        const auto result = check_expr(*lambda.result, state);
        loop_contexts_ = std::move(outer_loop_contexts);
        capture_context_ = previous_context;

        if (!is_invalid(result.type) &&
            !value_conforms_to_type(result, lambda.return_type, state)) {
            diagnose(lambda.result->position,
                     "lambda returns " + type_name(result.type) +
                         " but is declared " + type_name(lambda.return_type));
        }
        lambda.local_count =
            static_cast<std::uint32_t>(local_context.prototypes.size());
        local_context_ = previous_local_context;
        return annotate(
            expression,
            value_from_type(function_type(
                [&] {
                    std::vector<TypeSpec> parameters;
                    parameters.reserve(lambda.parameters.size());
                    for (const auto& parameter : lambda.parameters) {
                        parameters.push_back(parameter.type);
                    }
                    return parameters;
                }(),
                lambda.return_type)));
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

    std::optional<std::size_t> record_field_index(
        const RecordSymbol& record, const std::string& name) const {
        assert(record.declaration != nullptr);
        for (std::size_t i = 0; i < record.declaration->fields.size(); ++i) {
            if (record.declaration->fields[i].name == name) {
                return i;
            }
        }
        return std::nullopt;
    }

    TypedValue check_record_literal(Expr& expression, FlowState& state) {
        std::vector<TypedValue> initializers;
        initializers.reserve(expression.arguments.size());
        for (auto& argument : expression.arguments) {
            initializers.push_back(check_expr(*argument, state));
        }

        const auto* record = find_record(expression.name);
        if (record == nullptr || record->declaration == nullptr) {
            diagnose(expression.position,
                     "unknown record type '" + expression.name + "'");
            return annotate(expression, invalid_value());
        }
        expression.record_layout_index = record->index;
        const auto& fields = record->declaration->fields;
        bool valid = true;
        if (expression.field_names.size() != fields.size()) {
            diagnose(expression.position,
                     "record literal for '" + expression.name +
                         "' requires all " + std::to_string(fields.size()) +
                         " field(s) in declared order");
            valid = false;
        }

        std::set<std::string> seen;
        for (std::size_t i = 0; i < expression.field_names.size(); ++i) {
            const auto position = i < expression.field_positions.size()
                                      ? expression.field_positions[i]
                                      : expression.position;
            const auto& name = expression.field_names[i];
            if (!seen.insert(name).second) {
                diagnose(position,
                         "record initializer field '" + name +
                             "' is specified more than once");
                valid = false;
            }
            const auto actual_field = record_field_index(*record, name);
            if (!actual_field.has_value()) {
                diagnose(position,
                         "unknown field '" + name + "' on record '" +
                             expression.name + "'");
                valid = false;
            } else if (i >= fields.size() || fields[i].name != name) {
                const auto expected = i < fields.size() ? fields[i].name
                                                        : std::string("<none>");
                diagnose(position,
                         "record literal for '" + expression.name +
                             "' requires field '" + expected +
                             "' here (declared order)");
                valid = false;
            }
        }

        const auto checked_count = std::min(initializers.size(), fields.size());
        for (std::size_t i = 0; i < checked_count; ++i) {
            if (!is_invalid(initializers[i].type) &&
                !value_conforms_to_type(initializers[i], fields[i].type, state)) {
                diagnose(expression.arguments[i]->position,
                         "record field '" + fields[i].name + "' expects " +
                             type_name(fields[i].type) + " but got " +
                             type_name(initializers[i].type));
                valid = false;
            }
        }
        if (!valid) {
            return annotate(expression, invalid_value());
        }

        auto value = value_from_type(
            record_type(record->name, record->index, expression.position));
        value.includes_nil = false;
        return annotate(expression, std::move(value));
    }

    TypedValue check_variant_literal(Expr& expression, FlowState& state) {
        std::vector<TypedValue> arguments;
        arguments.reserve(expression.arguments.size());
        for (auto& argument : expression.arguments) {
            arguments.push_back(check_expr(*argument, state));
        }
        if (expression.variant_layout_index >= variants_.size()) {
            return annotate(expression, invalid_value());
        }
        const auto& variant = variants_[expression.variant_layout_index];
        if (variant.declaration == nullptr ||
            expression.variant_case_index >= variant.declaration->cases.size()) {
            return annotate(expression, invalid_value());
        }
        const auto& variant_case =
            variant.declaration->cases[expression.variant_case_index];
        bool valid = true;
        if (arguments.size() != variant_case.fields.size()) {
            diagnose(expression.position,
                     "variant case '" + variant.name + "." +
                         variant_case.name + "' expects " +
                         std::to_string(variant_case.fields.size()) +
                         " payload values but got " +
                         std::to_string(arguments.size()));
            valid = false;
        }
        const auto checked_count =
            std::min(arguments.size(), variant_case.fields.size());
        for (std::size_t i = 0; i < checked_count; ++i) {
            if (!is_invalid(arguments[i].type) &&
                !value_conforms_to_type(arguments[i], variant_case.fields[i],
                                        state)) {
                diagnose(expression.arguments[i]->position,
                         "variant payload " + std::to_string(i) + " expects " +
                             type_name(variant_case.fields[i]) + " but got " +
                             type_name(arguments[i].type));
                valid = false;
            }
        }
        if (!valid) {
            return annotate(expression, invalid_value());
        }
        auto result = value_from_type(variant_type(
            variant.name, variant.index, expression.position));
        result.includes_nil = false;
        return annotate(expression, std::move(result));
    }

    bool require_nonnil_ref_array_element(const TypedValue& value,
                                          const TypeSpec& element_type,
                                          SourcePosition position) {
        if (!is_reference_array_element_type(element_type)) {
            return true;
        }
        if (!is_known_nonnil_reference(value)) {
            diagnose(position, "reference array elements must be non-nil");
            return false;
        }
        return true;
    }

    TypedValue check_array_literal(Expr& expression, FlowState& state) {
        if (expression.arguments.empty()) {
            diagnose(expression.position,
                     "array literal requires at least one element; use array<T>(len, init) for sized construction");
            return annotate(expression, invalid_value());
        }

        std::vector<TypedValue> elements;
        elements.reserve(expression.arguments.size());
        for (auto& argument : expression.arguments) {
            elements.push_back(check_expr(*argument, state));
        }

        TypeSpec element_type = elements.front().type;
        TypedValue joined_element = elements.front();
        bool valid = !is_invalid(element_type);
        for (std::size_t i = 1; i < elements.size(); ++i) {
            if (is_invalid(elements[i].type)) {
                valid = false;
                continue;
            }
            if (elements[i].type != element_type) {
                diagnose(expression.arguments[i]->position,
                         "array literal elements must have one type");
                valid = false;
                continue;
            }
            joined_element = join_values(joined_element, elements[i]);
        }
        for (std::size_t i = 0; i < elements.size(); ++i) {
            valid = require_nonnil_ref_array_element(
                        elements[i], element_type, expression.arguments[i]->position) &&
                    valid;
        }
        if (!valid) {
            return annotate(expression, invalid_value());
        }

        auto inferred_type = array_type(element_type);
        return annotate(expression, array_value(std::move(inferred_type),
                                                std::move(joined_element)));
    }

    TypedValue check_array_sized(Expr& expression, FlowState& state) {
        resolve_type(expression.array_element_type);
        const auto length = check_expr(*expression.left, state);
        const auto initializer = check_expr(*expression.right, state);
        bool valid = true;
        if (!is_invalid(length.type) && length.type != int64_type()) {
            diagnose(expression.left->position, "array length must be i64");
            valid = false;
        }
        if (!is_invalid(initializer.type) &&
            !value_conforms_to_type(initializer, expression.array_element_type, state)) {
            diagnose(expression.right->position,
                     "array initializer expects " +
                         type_name(expression.array_element_type) + " but got " +
                         type_name(initializer.type));
            valid = false;
        }
        valid = require_nonnil_ref_array_element(
                    initializer, expression.array_element_type, expression.right->position) &&
                valid;
        if (!valid || is_invalid(expression.array_element_type)) {
            return annotate(expression, invalid_value());
        }

        auto coerced_element =
            value_as_declared_type(initializer, expression.array_element_type);
        if (is_reference_array_element_type(expression.array_element_type)) {
            coerced_element.includes_nil = false;
        }
        auto inferred_type = array_type(expression.array_element_type);
        return annotate(expression, array_value(std::move(inferred_type),
                                                std::move(coerced_element)));
    }

    TypedValue check_map_empty(Expr& expression) {
        resolve_type(expression.map_key_type);
        resolve_type(expression.map_value_type);
        if (is_invalid(expression.map_key_type) ||
            is_invalid(expression.map_value_type) ||
            !is_valid_map_key_type(expression.map_key_type)) {
            return annotate(expression, invalid_value());
        }
        return annotate(expression,
                        value_from_type(map_type(expression.map_key_type,
                                                 expression.map_value_type)));
    }

    TypedValue check_map_has(Expr& expression, FlowState& state) {
        const auto receiver = check_expr(*expression.receiver, state);
        const auto key = check_expr(*expression.left, state);
        if (is_invalid(receiver.type) || is_invalid(key.type)) {
            return annotate(expression, invalid_value());
        }
        if (receiver.type.kind != TypeSpec::Kind::Map ||
            receiver.type.key == nullptr || receiver.type.value == nullptr) {
            diagnose(expression.position, "has requires map");
            return annotate(expression, invalid_value());
        }
        if (receiver.includes_nil) {
            diagnose(expression.position,
                     "map operation requires non-nil value of type " +
                         type_name(receiver.type));
            return annotate(expression, invalid_value());
        }
        if (!value_conforms_to_type(key, *receiver.type.key, state)) {
            diagnose(expression.left->position,
                     "map key expects " + type_name(*receiver.type.key) +
                         " but got " + type_name(key.type));
            return annotate(expression, invalid_value());
        }
        return annotate(expression, scalar_value(bool_type()));
    }

    TypedValue check_weak_construct(Expr& expression, FlowState& state) {
        const auto target = check_expr(*expression.receiver, state);
        if (is_invalid(target.type)) {
            return annotate(expression, invalid_value());
        }
        if (!is_weak_target_type(target.type) || target.includes_nil) {
            diagnose(expression.receiver->position,
                     "weak() requires a non-nil object operand");
            return annotate(expression, invalid_value());
        }
        return annotate(expression,
                        value_from_type(weak_type(target.type)));
    }

    TypedValue check_weak_get(Expr& expression, FlowState& state) {
        const auto receiver = check_expr(*expression.receiver, state);
        if (is_invalid(receiver.type)) {
            return annotate(expression, invalid_value());
        }
        if (receiver.includes_nil ||
            receiver.type.kind != TypeSpec::Kind::Weak ||
            receiver.type.weak_target == nullptr) {
            diagnose(expression.position, "get requires weak");
            return annotate(expression, invalid_value());
        }
        auto target = value_from_type(*receiver.type.weak_target);
        target.includes_nil = true;
        return annotate(expression, std::move(target));
    }

    TypedValue check_ephemeron_construct(Expr& expression, FlowState& state) {
        const auto key = check_expr(*expression.left, state);
        const auto value = check_expr(*expression.right, state);
        if (is_invalid(key.type) || is_invalid(value.type))
            return annotate(expression, invalid_value());
        if (!is_weak_target_type(key.type) || key.includes_nil) {
            diagnose(expression.left->position,
                     "ephemeron() requires a non-nil object key");
            return annotate(expression, invalid_value());
        }
        if (value.type.kind == TypeSpec::Kind::Nil || value.includes_nil) {
            diagnose(expression.right->position,
                     "ephemeron() requires a non-nil initial value");
            return annotate(expression, invalid_value());
        }
        return annotate(expression, value_from_type(
            ephemeron_type(key.type, value.type)));
    }

    TypedValue check_ephemeron_get(Expr& expression, FlowState& state) {
        const auto receiver = check_expr(*expression.receiver, state);
        if (is_invalid(receiver.type)) return annotate(expression, invalid_value());
        if (receiver.includes_nil ||
            receiver.type.kind != TypeSpec::Kind::Ephemeron ||
            receiver.type.key == nullptr || receiver.type.value == nullptr) {
            diagnose(expression.position, "ephemeron getter requires ephemeron");
            return annotate(expression, invalid_value());
        }
        auto result = value_from_type(expression.kind == Expr::Kind::EphemeronKey
                                          ? *receiver.type.key
                                          : *receiver.type.value);
        if (expression.kind == Expr::Kind::EphemeronKey ||
            is_object_type(result.type)) result.includes_nil = true;
        return annotate(expression, std::move(result));
    }

    TypedValue check_conversion(Expr& expression, FlowState& state) {
        const auto operand = check_expr(*expression.receiver, state);
        if (is_invalid(operand.type)) {
            return annotate(expression, invalid_value());
        }
        if (expression.kind == Expr::Kind::ToStr) {
            if (operand.type != int64_type() && operand.type != bool_type()) {
                diagnose(expression.receiver->position,
                         "to_str expects i64 or bool");
                return annotate(expression, invalid_value());
            }
            return annotate(expression, scalar_value(str_type()));
        }
        if (operand.type != str_type() || operand.includes_nil) {
            diagnose(expression.receiver->position,
                     operand.type == str_type()
                         ? "to_i64 requires non-nil str"
                         : "to_i64 expects str");
            return annotate(expression, invalid_value());
        }
        return annotate(expression, scalar_value(int64_type()));
    }

    TypedValue load_array_element(const TypedValue& receiver, SourcePosition position) {
        if (is_invalid(receiver.type)) {
            return invalid_value();
        }
        if (receiver.type.kind != TypeSpec::Kind::Array ||
            receiver.type.element == nullptr) {
            diagnose(position, "indexing requires array");
            return invalid_value();
        }
        auto element = receiver.array_element != nullptr
                           ? *receiver.array_element
                           : value_from_type(*receiver.type.element);
        element.type = *receiver.type.element;
        if (is_reference_array_element_type(*receiver.type.element)) {
            element.includes_nil = false;
        }
        return element;
    }

    TypedValue check_array_index(Expr& expression, FlowState& state) {
        const auto receiver = check_expr(*expression.receiver, state);
        const auto index = check_expr(*expression.left, state);
        if (!is_invalid(receiver.type) && receiver.includes_nil) {
            diagnose(expression.position,
                     "indexing requires non-nil value of type " +
                         type_name(receiver.type));
            return annotate(expression, invalid_value());
        }
        if (receiver.type.kind == TypeSpec::Kind::Map &&
            receiver.type.key != nullptr && receiver.type.value != nullptr) {
            if (!is_invalid(index.type) &&
                !value_conforms_to_type(index, *receiver.type.key, state)) {
                diagnose(expression.left->position,
                         "map key expects " + type_name(*receiver.type.key) +
                             " but got " + type_name(index.type));
                return annotate(expression, invalid_value());
            }
            return annotate(expression,
                            value_from_type(*receiver.type.value));
        }
        if (!is_invalid(index.type) && index.type != int64_type()) {
            diagnose(expression.left->position,
                     receiver.type.kind == TypeSpec::Kind::Str
                         ? "string index must be i64"
                         : "array index must be i64");
            return annotate(expression, invalid_value());
        }
        if (receiver.type.kind == TypeSpec::Kind::Str) {
            return annotate(expression, scalar_value(int64_type()));
        }
        if (!is_invalid(receiver.type) &&
            receiver.type.kind != TypeSpec::Kind::Array) {
            diagnose(expression.position,
                     "indexing requires array or str or map");
            return annotate(expression, invalid_value());
        }
        return annotate(expression, load_array_element(receiver, expression.position));
    }

    TypedValue check_array_len(Expr& expression, FlowState& state) {
        const auto receiver = check_expr(*expression.receiver, state);
        if (!is_invalid(receiver.type) && receiver.includes_nil) {
            diagnose(expression.position,
                     "len requires non-nil value of type " +
                         type_name(receiver.type));
            return annotate(expression, invalid_value());
        }
        if (!is_invalid(receiver.type) &&
            receiver.type.kind != TypeSpec::Kind::Str &&
            receiver.type.kind != TypeSpec::Kind::Map &&
            (receiver.type.kind != TypeSpec::Kind::Array ||
             receiver.type.element == nullptr)) {
            diagnose(expression.position, "len requires array, str, or map");
            return annotate(expression, invalid_value());
        }
        return annotate(expression, scalar_value(int64_type()));
    }

    TypedValue check_binary(Expr& expression, FlowState& state) {
        const auto left = check_expr(*expression.left, state);
        const auto right = check_expr(*expression.right, state);
        if (expression.binary_op == '+') {
            if (left.type == int64_type() && right.type == int64_type()) {
                return annotate(expression, scalar_value(int64_type()));
            }
            if (left.type == str_type() && right.type == str_type() &&
                (left.includes_nil || right.includes_nil)) {
                diagnose(expression.operator_position,
                         "operator '+' requires non-nil str operands");
                return annotate(expression, invalid_value());
            }
            if (left.type == str_type() && right.type == str_type()) {
                return annotate(expression, scalar_value(str_type()));
            }
            if (left.type.kind == TypeSpec::Kind::Str ||
                right.type.kind == TypeSpec::Kind::Str) {
                diagnose(expression.operator_position,
                         "operator '+' requires str + str or i64 + i64");
                return annotate(expression, invalid_value());
            }
            if ((!is_invalid(left.type) && left.type != int64_type()) ||
                (!is_invalid(right.type) && right.type != int64_type())) {
                diagnose(expression.operator_position, "operator '+' requires i64 operands");
                return annotate(expression, invalid_value());
            }
            return annotate(expression, scalar_value(int64_type()));
        }
        if (expression.binary_op == '<' || expression.binary_op == 'L' ||
            expression.binary_op == '>' || expression.binary_op == 'G') {
            const std::string operation =
                expression.binary_op == '<' ? "<" :
                expression.binary_op == 'L' ? "<=" :
                expression.binary_op == '>' ? ">" : ">=";
            const bool both_i64 =
                left.type == int64_type() && right.type == int64_type();
            const bool both_str = left.type == str_type() && right.type == str_type();
            if (both_str && (left.includes_nil || right.includes_nil)) {
                diagnose(expression.operator_position,
                         "string ordering requires non-nil str operands");
                return annotate(expression, invalid_value());
            }
            if (both_str || (expression.binary_op == '<' && both_i64)) {
                return annotate(expression, scalar_value(bool_type()));
            }
            if (expression.binary_op == '<') {
                if (left.type == str_type() || right.type == str_type()) {
                    diagnose(expression.operator_position,
                             "operator '<' requires matching i64 or str operands");
                } else {
                    diagnose(expression.operator_position,
                             "operator '<' requires i64 operands");
                }
            } else {
                diagnose(expression.operator_position,
                         "operator '" + operation + "' requires str operands");
            }
            return annotate(expression, invalid_value());
        }
        if (expression.binary_op == '=' || expression.binary_op == '!') {
            if (left.type == str_type() && right.type == str_type() &&
                (left.includes_nil || right.includes_nil)) {
                diagnose(expression.operator_position,
                         "string comparison requires non-nil str operands");
                return annotate(expression, invalid_value());
            }
            if ((!is_invalid(left.type) && left.type != str_type()) ||
                (!is_invalid(right.type) && right.type != str_type())) {
                const std::string operation = expression.binary_op == '=' ? "==" : "!=";
                diagnose(expression.operator_position,
                         "operator '" + operation + "' requires str operands");
                return annotate(expression, invalid_value());
            }
            return annotate(expression, scalar_value(bool_type()));
        }
        diagnose(expression.operator_position, "unknown binary operator");
        return annotate(expression, invalid_value());
    }

    TypedValue check_str_sub(Expr& expression, FlowState& state) {
        const auto receiver = check_expr(*expression.receiver, state);
        if (expression.arguments.size() != 2) {
            diagnose(expression.position, "sub expects exactly 2 arguments");
            return annotate(expression, invalid_value());
        }
        const auto lo = check_expr(*expression.arguments[0], state);
        const auto hi = check_expr(*expression.arguments[1], state);
        bool valid = true;
        if (!is_invalid(receiver.type) && receiver.type != str_type()) {
            diagnose(expression.position, "sub requires str receiver");
            valid = false;
        } else if (receiver.type == str_type() && receiver.includes_nil) {
            diagnose(expression.position, "sub requires non-nil str receiver");
            valid = false;
        }
        if (!is_invalid(lo.type) && lo.type != int64_type()) {
            diagnose(expression.arguments[0]->position,
                     "sub argument expects i64");
            valid = false;
        }
        if (!is_invalid(hi.type) && hi.type != int64_type()) {
            diagnose(expression.arguments[1]->position,
                     "sub argument expects i64");
            valid = false;
        }
        return annotate(expression,
                        valid ? scalar_value(str_type()) : invalid_value());
    }

    TypedValue check_is_nil(Expr& expression, FlowState& state) {
        const auto value = check_expr(*expression.receiver, state);
        if (!is_invalid(value.type) && !is_pair(value.type) &&
            value.type.kind != TypeSpec::Kind::Record &&
            value.type.kind != TypeSpec::Kind::Variant &&
            !value.includes_nil && value.type.kind != TypeSpec::Kind::Nil) {
            diagnose(expression.position,
                     "is_nil requires record, pair, nil, or nil-able object operand");
            return annotate(expression, invalid_value());
        }
        return annotate(expression, scalar_value(bool_type()));
    }

    TypedValue check_field_access(Expr& expression, FlowState& state) {
        const auto receiver = check_expr(*expression.receiver, state);
        if (receiver.type.kind == TypeSpec::Kind::Record) {
            const auto field = load_record_field(
                receiver, expression.name, expression.position,
                &expression.record_layout_index,
                &expression.record_field_index);
            return annotate(expression, field);
        }
        if (expression.name == "len") {
            if (!is_invalid(receiver.type) && receiver.includes_nil) {
                diagnose(expression.position,
                         "len requires non-nil value of type " +
                             type_name(receiver.type));
                return annotate(expression, invalid_value());
            }
            if (!is_invalid(receiver.type) &&
                receiver.type.kind != TypeSpec::Kind::Str &&
                receiver.type.kind != TypeSpec::Kind::Map &&
                (receiver.type.kind != TypeSpec::Kind::Array ||
                 receiver.type.element == nullptr)) {
                diagnose(expression.position, "len requires array, str, or map");
                return annotate(expression, invalid_value());
            }
            return annotate(expression, scalar_value(int64_type()));
        }
        const auto field = load_field(receiver, expression.name, expression.position, state);
        return annotate(expression, field);
    }

    TypedValue check_call(Expr& expression, FlowState& state) {
        if (expression.receiver != nullptr &&
            expression.receiver->kind == Expr::Kind::Field &&
            expression.receiver->receiver != nullptr &&
            expression.receiver->receiver->kind == Expr::Kind::Variable) {
            const auto& variant_name = expression.receiver->receiver->name;
            if (auto* variant = find_variant(variant_name); variant != nullptr &&
                variant->declaration != nullptr) {
                const auto& case_name = expression.receiver->name;
                const auto& cases = variant->declaration->cases;
                const auto found = std::find_if(
                    cases.begin(), cases.end(), [&](const VariantCaseDecl& item) {
                        return item.name == case_name;
                    });
                if (found == cases.end()) {
                    for (auto& argument : expression.arguments) {
                        (void)check_expr(*argument, state);
                    }
                    diagnose(expression.receiver->position,
                             "unknown variant case '" + case_name + "'");
                    return annotate(expression, invalid_value());
                }
                const auto case_index = static_cast<std::size_t>(
                    std::distance(cases.begin(), found));
                expression.variant_layout_index = variant->index;
                expression.variant_case_index = case_index;
                expression.kind = Expr::Kind::VariantLiteral;
                return check_variant_literal(expression, state);
            }
        }
        std::vector<TypedValue> arguments;
        arguments.reserve(expression.arguments.size());
        for (auto& argument : expression.arguments) {
            arguments.push_back(check_expr(*argument, state));
        }

        assert(expression.receiver != nullptr && "call expression must retain callee");
        const auto callee = check_expr(*expression.receiver, state);
        if (is_invalid(callee.type)) {
            return annotate(expression, invalid_value());
        }
        if (callee.type.kind != TypeSpec::Kind::Function ||
            callee.type.function_return == nullptr) {
            if (expression.receiver->kind == Expr::Kind::Variable) {
                diagnose(expression.position,
                         "cannot call non-function name '" +
                             expression.receiver->name + "'");
            } else {
                diagnose(expression.position,
                         "cannot call non-function of type " +
                             type_name(callee.type));
            }
            return annotate(expression, invalid_value());
        }
        if (callee.includes_nil) {
            diagnose(expression.position,
                     "function call requires non-nil value of type " +
                         type_name(callee.type));
            return annotate(expression, invalid_value());
        }

        const FunctionSymbol* direct_function = nullptr;
        if (expression.receiver->kind == Expr::Kind::Variable &&
            expression.receiver->is_function_reference) {
            direct_function = find_function(expression.receiver->name);
            assert(direct_function != nullptr);
            expression.direct_call = true;
            expression.callee_index = direct_function->index;
        }

        const auto& parameters = callee.type.function_parameters;
        if (arguments.size() != parameters.size()) {
            const auto label = direct_function == nullptr
                                   ? "function value"
                                   : "function '" + direct_function->name + "'";
            diagnose(expression.position,
                     label + " expects " + std::to_string(parameters.size()) +
                         " argument(s) but got " + std::to_string(arguments.size()));
            return annotate(expression, invalid_value());
        }

        bool valid = true;
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            if (is_invalid(arguments[i].type)) {
                valid = false;
                continue;
            }
            if (!value_conforms_to_type(arguments[i], parameters[i], state)) {
                const auto label = direct_function == nullptr
                                       ? "function value"
                                       : "function '" + direct_function->name + "'";
                diagnose(expression.arguments[i]->position,
                         "argument " + std::to_string(i + 1) + " of " + label +
                             " expects " + type_name(parameters[i]) + " but got " +
                             type_name(arguments[i].type));
                valid = false;
            }
        }
        if (!valid) {
            return annotate(expression, invalid_value());
        }
        return annotate(expression,
                        value_from_type(*callee.type.function_return));
    }

    TypedValue check_lvalue_prefix(LValue& lvalue, FlowState& state,
                                   std::size_t step_count) {
        auto* local = find_local(state, lvalue.base_name);
        if (local == nullptr) {
            if (capture_context_ != nullptr) {
                const auto captured = try_ensure_capture(
                    *capture_context_, lvalue.base_name, lvalue.base_position);
                if (captured.has_value()) {
                    if (is_invalid(captured->type)) {
                        return invalid_value();
                    }
                    diagnose(lvalue.base_position,
                             "cannot assign through immutable capture '" +
                                 lvalue.base_name + "'");
                    return invalid_value();
                }
            }
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
        for (std::size_t i = 0; i < step_count; ++i) {
            auto& step = lvalue.steps[i];
            step.receiver_type = current.type;
            if (current.includes_nil) {
                diagnose(step.position,
                         "assignment requires non-nil value of type " +
                             type_name(current.type));
                return invalid_value();
            }
            if (step.kind == LValueStep::Kind::Field) {
                if (current.type.kind == TypeSpec::Kind::Record) {
                    current = load_record_field(
                        current, step.name, step.position,
                        &step.record_layout_index, &step.record_field_index);
                } else {
                    current = load_field(current, step.name, step.position, state);
                }
            } else {
                const auto index = check_expr(*step.index, state);
                if (current.type.kind == TypeSpec::Kind::Map &&
                    current.type.key != nullptr && current.type.value != nullptr) {
                    if (!is_invalid(index.type) &&
                        !value_conforms_to_type(index, *current.type.key, state)) {
                        diagnose(step.index->position,
                                 "map key expects " +
                                     type_name(*current.type.key) + " but got " +
                                     type_name(index.type));
                        return invalid_value();
                    }
                    current = value_from_type(*current.type.value);
                    step.element_type = current.type;
                    continue;
                }
                if (!is_invalid(index.type) && index.type != int64_type()) {
                    diagnose(step.index->position, "array index must be i64");
                    return invalid_value();
                }
                current = load_array_element(current, step.position);
                if (!is_invalid(current.type)) {
                    step.element_type = current.type;
                }
            }
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
        if (field != "left" && field != "right") {
            diagnose(position,
                     "pair field access requires 'left' or 'right'");
            return invalid_value();
        }
        if (receiver.includes_nil) {
            diagnose(position, "field access requires non-nil value of type " +
                                   type_name(receiver.type));
            return invalid_value();
        }
        const auto loaded_field = try_load_field(receiver, field, state);
        if (!loaded_field.has_value()) {
            diagnose(position, "pair field type is unknown");
            return invalid_value();
        }
        return *loaded_field;
    }

    TypedValue load_record_field(const TypedValue& receiver,
                                 const std::string& field,
                                 SourcePosition position,
                                 std::size_t* layout_index,
                                 std::size_t* field_index) {
        if (receiver.includes_nil) {
            diagnose(position,
                     "field access requires non-nil value of type " +
                         type_name(receiver.type));
            return invalid_value();
        }
        const auto* record = find_record(receiver.type);
        if (record == nullptr || record->declaration == nullptr) {
            diagnose(position, "record layout is unknown");
            return invalid_value();
        }
        const auto index = record_field_index(*record, field);
        if (!index.has_value()) {
            diagnose(position,
                     "unknown field '" + field + "' on record '" +
                         record->name + "'");
            return invalid_value();
        }
        *layout_index = record->index;
        *field_index = *index;
        return value_from_type(record->declaration->fields[*index].type);
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
    std::vector<TypeSymbol> types_;
    std::vector<RecordSymbol> records_;
    std::vector<VariantSymbol> variants_;
    std::vector<FunctionSymbol> functions_;
    CaptureContext* capture_context_{nullptr};
    LocalContext* local_context_{nullptr};
    std::vector<LoopFlowContext*> loop_contexts_;
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
