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
        Pair,
        Named,
        Nil,
        Invalid,
    };

    Kind kind{Kind::Invalid};
    std::shared_ptr<TypeSpec> left;
    std::shared_ptr<TypeSpec> right;
    std::string name;
    SourcePosition position;
    std::optional<std::size_t> named_type_index;

    [[nodiscard]] bool has_pair_fields() const {
        return kind == Kind::Pair && left != nullptr && right != nullptr;
    }
};

TypeSpec int64_type();
TypeSpec bool_type();
TypeSpec pair_type();
TypeSpec named_type(std::string name, SourcePosition position);
TypeSpec nil_type();
TypeSpec invalid_type();
TypeSpec pair_type(TypeSpec left, TypeSpec right);

bool operator==(const TypeSpec& lhs, const TypeSpec& rhs);
bool operator!=(const TypeSpec& lhs, const TypeSpec& rhs);
bool is_invalid(const TypeSpec& type);
bool is_pair(const TypeSpec& type);
Type public_type(const TypeSpec& type);
std::string type_name(const TypeSpec& type);
TypeSpec join_types(const TypeSpec& lhs, const TypeSpec& rhs);

struct Expr {
    enum class Kind {
        IntLiteral,
        BoolLiteral,
        NilLiteral,
        Variable,
        PairLiteral,
        Binary,
        Field,
        Call,
        IsNil,
    };

    Kind kind{Kind::IntLiteral};
    SourcePosition position;
    SourcePosition operator_position;
    std::int64_t int_value{0};
    bool bool_value{false};
    std::string name;
    char binary_op{0};
    std::size_t pair_site{0};
    std::size_t callee_index{0};
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
    std::unique_ptr<Expr> receiver;
    std::vector<std::unique_ptr<Expr>> arguments;
    TypeSpec inferred_type{invalid_type()};
    std::set<std::size_t> object_sites;
    std::uint32_t local_index{0};
};

struct Parameter {
    std::string name;
    SourcePosition position;
    TypeSpec type{invalid_type()};
    std::uint32_t local_index{0};
};

struct FieldStep {
    std::string name;
    SourcePosition position;
};

struct LValue {
    std::string base_name;
    SourcePosition base_position;
    std::uint32_t local_index{0};
    std::vector<FieldStep> fields;
};

struct Statement {
    enum class Kind {
        Let,
        Assign,
        If,
        While,
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
};

struct FunctionDecl {
    std::string name;
    SourcePosition position;
    std::vector<Parameter> parameters;
    TypeSpec return_type{invalid_type()};
    std::vector<Statement> statements;
    std::unique_ptr<Expr> result;
    std::size_t function_index{0};
    std::uint32_t local_count{0};
};

struct TypeDecl {
    std::string name;
    SourcePosition position;
    SourcePosition body_position;
    TypeSpec body{invalid_type()};
    std::size_t type_index{static_cast<std::size_t>(-1)};
};

struct Program {
    std::vector<TypeDecl> types;
    std::vector<FunctionDecl> functions;
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
