#pragma once

#include "lexer.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace lang::frontend::detail {

struct TypeSpec {
    enum class Kind {
        Int64,
        Bool,
        Str,
        Pair,
        Array,
        Function,
        Map,
        Weak,
        Ephemeron,
        TypeParameter,
        Named,
        Record,
        Variant,
        Nil,
        Invalid,
    };

    Kind kind{Kind::Invalid};
    std::shared_ptr<TypeSpec> left;
    std::shared_ptr<TypeSpec> right;
    std::shared_ptr<TypeSpec> element;
    std::shared_ptr<TypeSpec> key;
    std::shared_ptr<TypeSpec> value;
    std::shared_ptr<TypeSpec> weak_target;
    std::vector<TypeSpec> function_parameters;
    std::shared_ptr<TypeSpec> function_return;
    std::string name;
    SourcePosition position;
    std::optional<std::size_t> named_type_index;
    std::optional<std::size_t> record_layout_index;
    std::optional<std::size_t> variant_layout_index;
    std::optional<std::size_t> type_parameter_index;

    [[nodiscard]] bool has_pair_fields() const {
        return kind == Kind::Pair && left != nullptr && right != nullptr;
    }

    [[nodiscard]] bool has_function_signature() const {
        return kind == Kind::Function && function_return != nullptr;
    }

    [[nodiscard]] bool has_map_entries() const {
        return kind == Kind::Map && key != nullptr && value != nullptr;
    }

    [[nodiscard]] bool has_weak_target() const {
        return kind == Kind::Weak && weak_target != nullptr;
    }
    [[nodiscard]] bool has_ephemeron_entries() const {
        return kind == Kind::Ephemeron && key != nullptr && value != nullptr;
    }
};

TypeSpec int64_type();
TypeSpec bool_type();
TypeSpec str_type();
TypeSpec pair_type();
TypeSpec named_type(std::string name, SourcePosition position);
TypeSpec record_type(std::string name, std::size_t layout_index,
                     SourcePosition position = {});
TypeSpec variant_type(std::string name, std::size_t layout_index,
                      SourcePosition position = {});
TypeSpec nil_type();
TypeSpec invalid_type();
TypeSpec pair_type(TypeSpec left, TypeSpec right);
TypeSpec array_type(TypeSpec element);
TypeSpec function_type(std::vector<TypeSpec> parameters, TypeSpec result);
TypeSpec map_type(TypeSpec key, TypeSpec value);
TypeSpec weak_type(TypeSpec target);
TypeSpec ephemeron_type(TypeSpec key, TypeSpec value);
TypeSpec type_parameter_type(std::string name, std::size_t index,
                             SourcePosition position);

bool operator==(const TypeSpec& lhs, const TypeSpec& rhs);
bool operator!=(const TypeSpec& lhs, const TypeSpec& rhs);
bool is_invalid(const TypeSpec& type);
bool is_pair(const TypeSpec& type);
Type public_type(const TypeSpec& type);
std::string type_name(const TypeSpec& type);
TypeSpec join_types(const TypeSpec& lhs, const TypeSpec& rhs);

struct LambdaExpr;

struct Expr {
    enum class Kind {
        IntLiteral,
        BoolLiteral,
        StringLiteral,
        NilLiteral,
        Variable,
        PairLiteral,
        RecordLiteral,
        VariantLiteral,
        ArrayLiteral,
        ArraySized,
        ArrayIndex,
        ArrayLen,
        StrSub,
        Binary,
        Field,
        Call,
        Lambda,
        IsNil,
        MapEmpty,
        MapHas,
        WeakConstruct,
        WeakGet,
        EphemeronConstruct,
        EphemeronKey,
        EphemeronValue,
        ToStr,
        ToI64,
    };

    Kind kind{Kind::IntLiteral};
    SourcePosition position;
    SourcePosition operator_position;
    std::int64_t int_value{0};
    bool bool_value{false};
    std::string string_value;
    std::string name;
    char binary_op{0};
    std::size_t pair_site{0};
    std::size_t callee_index{0};
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
    std::unique_ptr<Expr> receiver;
    TypeSpec array_element_type{invalid_type()};
    TypeSpec map_key_type{invalid_type()};
    TypeSpec map_value_type{invalid_type()};
    std::vector<std::unique_ptr<Expr>> arguments;
    std::vector<TypeSpec> explicit_type_arguments;
    SourcePosition type_arguments_position;
    std::vector<std::string> field_names;
    std::vector<SourcePosition> field_positions;
    TypeSpec inferred_type{invalid_type()};
    std::set<std::size_t> object_sites;
    std::uint32_t local_index{0};
    std::size_t closure_layout_index{0};
    std::size_t capture_index{0};
    bool is_capture{false};
    bool is_function_reference{false};
    bool direct_call{false};
    std::size_t record_layout_index{static_cast<std::size_t>(-1)};
    std::size_t record_field_index{static_cast<std::size_t>(-1)};
    std::size_t variant_layout_index{static_cast<std::size_t>(-1)};
    std::size_t variant_case_index{static_cast<std::size_t>(-1)};
    std::shared_ptr<LambdaExpr> lambda;
};

struct Parameter {
    std::string name;
    SourcePosition position;
    TypeSpec type{invalid_type()};
    std::uint32_t local_index{0};
};

struct LValueStep {
    enum class Kind {
        Field,
        Index,
    };

    Kind kind{Kind::Field};
    std::string name;
    SourcePosition position;
    std::unique_ptr<Expr> index;
    TypeSpec receiver_type{invalid_type()};
    TypeSpec element_type{invalid_type()};
    std::size_t record_layout_index{static_cast<std::size_t>(-1)};
    std::size_t record_field_index{static_cast<std::size_t>(-1)};
};

struct LValue {
    std::string base_name;
    SourcePosition base_position;
    std::uint32_t local_index{0};
    std::vector<LValueStep> steps;
    TypeSpec receiver_type{invalid_type()};
    TypeSpec element_type{invalid_type()};
};

struct Statement;

struct MatchBinding {
    std::string name;
    SourcePosition position;
    std::uint32_t local_index{0};
};

struct MatchArm {
    std::string case_name;
    SourcePosition position;
    std::vector<MatchBinding> bindings;
    std::vector<Statement> body;
    std::size_t case_index{static_cast<std::size_t>(-1)};
};

struct Statement {
    enum class Kind {
        Let,
        Assign,
        If,
        While,
        ForIn,
        Match,
        TryCatch,
        Throw,
        Break,
        Continue,
        Print,
        EphemeronSet,
        TailCall,
    };

    Kind kind{Kind::Let};
    SourcePosition position;
    SourcePosition equals_position;
    std::string name;
    TypeSpec declared_type{invalid_type()};
    std::uint32_t local_index{0};
    std::unique_ptr<Expr> initializer;
    LValue target;
    std::unique_ptr<Expr> value;
    std::unique_ptr<Expr> condition;
    std::vector<Statement> then_branch;
    std::vector<Statement> else_branch;
    std::vector<Statement> body;
    std::vector<std::string> loop_names;
    std::vector<SourcePosition> loop_name_positions;
    std::vector<std::uint32_t> loop_local_indices;
    std::unique_ptr<Expr> iterable;
    std::unique_ptr<Expr> range_upper;
    bool loop_locals_allocated{false};
    bool match_locals_allocated{false};
    std::vector<MatchArm> match_arms;
    std::size_t match_variant_layout_index{static_cast<std::size_t>(-1)};
    std::vector<Statement> catch_body;
    std::string catch_name;
    SourcePosition catch_position;
    TypeSpec catch_type{invalid_type()};
    std::uint32_t catch_local_index{0};
    bool catch_local_allocated{false};
};

struct CaptureSpec {
    std::string name;
    SourcePosition position;
    TypeSpec type{invalid_type()};
    std::uint32_t source_index{0};
    bool source_is_capture{false};
};

struct LambdaExpr {
    SourcePosition position;
    std::vector<Parameter> parameters;
    TypeSpec return_type{invalid_type()};
    std::vector<Statement> statements;
    std::unique_ptr<Expr> result;
    std::vector<CaptureSpec> captures;
    std::size_t function_index{0};
    std::size_t closure_layout_index{0};
    std::uint32_t local_count{0};
};

struct TypeParameterDecl {
    std::string name;
    SourcePosition position;
    std::size_t index{0};
};

struct FunctionDecl {
    std::string name;
    SourcePosition position;
    std::vector<TypeParameterDecl> type_parameters;
    std::vector<Parameter> parameters;
    TypeSpec return_type{invalid_type()};
    std::vector<Statement> statements;
    std::unique_ptr<Expr> result;
    std::size_t function_index{0};
    std::size_t closure_layout_index{0};
    std::uint32_t local_count{0};
    std::size_t declaration_order{0};
};

struct TypeDecl {
    std::string name;
    SourcePosition position;
    SourcePosition body_position;
    TypeSpec body{invalid_type()};
    std::size_t type_index{static_cast<std::size_t>(-1)};
};

struct RecordFieldDecl {
    std::string name;
    SourcePosition position;
    TypeSpec type{invalid_type()};
};

struct RecordDecl {
    std::string name;
    SourcePosition position;
    std::vector<RecordFieldDecl> fields;
    std::size_t layout_index{static_cast<std::size_t>(-1)};
};

struct VariantCaseDecl {
    std::string name;
    SourcePosition position;
    std::vector<TypeSpec> fields;
};

struct VariantDecl {
    std::string name;
    SourcePosition position;
    std::vector<VariantCaseDecl> cases;
    std::size_t layout_index{static_cast<std::size_t>(-1)};
};

struct Program {
    std::vector<TypeDecl> types;
    std::vector<RecordDecl> records;
    std::vector<VariantDecl> variants;
    std::vector<FunctionDecl> functions;
    std::vector<FunctionDecl> generic_functions;
    std::vector<std::shared_ptr<LambdaExpr>> lambdas;
    std::vector<Statement> statements;
    std::unique_ptr<Expr> result;
    std::uint32_t entry_local_count{0};
    std::size_t pair_site_count{0};
};

struct ParseResult {
    std::optional<Program> program;
    std::vector<Diagnostic> diagnostics;
};

ParseResult parse_tokens(std::vector<Token> tokens);

} // namespace lang::frontend::detail
