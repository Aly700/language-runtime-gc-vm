#include "parser.hpp"

#include "diagnostics.hpp"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace lang::frontend::detail {

TypeSpec type_with_kind(TypeSpec::Kind kind) {
    TypeSpec type;
    type.kind = kind;
    return type;
}

TypeSpec int64_type() { return type_with_kind(TypeSpec::Kind::Int64); }
TypeSpec bool_type() { return type_with_kind(TypeSpec::Kind::Bool); }
TypeSpec str_type() { return type_with_kind(TypeSpec::Kind::Str); }
TypeSpec pair_type() { return type_with_kind(TypeSpec::Kind::Pair); }
TypeSpec nil_type() { return type_with_kind(TypeSpec::Kind::Nil); }
TypeSpec invalid_type() { return type_with_kind(TypeSpec::Kind::Invalid); }

TypeSpec named_type(std::string name, SourcePosition position) {
    TypeSpec type;
    type.kind = TypeSpec::Kind::Named;
    type.name = std::move(name);
    type.position = position;
    return type;
}

TypeSpec record_type(std::string name, std::size_t layout_index,
                     SourcePosition position) {
    TypeSpec type;
    type.kind = TypeSpec::Kind::Record;
    type.name = std::move(name);
    type.position = position;
    type.record_layout_index = layout_index;
    return type;
}

TypeSpec variant_type(std::string name, std::size_t layout_index,
                      SourcePosition position) {
    TypeSpec type;
    type.kind = TypeSpec::Kind::Variant;
    type.name = std::move(name);
    type.position = position;
    type.variant_layout_index = layout_index;
    return type;
}

TypeSpec pair_type(TypeSpec left, TypeSpec right) {
    TypeSpec type;
    type.kind = TypeSpec::Kind::Pair;
    type.left = std::make_shared<TypeSpec>(std::move(left));
    type.right = std::make_shared<TypeSpec>(std::move(right));
    return type;
}

TypeSpec array_type(TypeSpec element) {
    TypeSpec type;
    type.kind = TypeSpec::Kind::Array;
    type.element = std::make_shared<TypeSpec>(std::move(element));
    return type;
}

TypeSpec function_type(std::vector<TypeSpec> parameters, TypeSpec result) {
    TypeSpec type;
    type.kind = TypeSpec::Kind::Function;
    type.function_parameters = std::move(parameters);
    type.function_return = std::make_shared<TypeSpec>(std::move(result));
    return type;
}

TypeSpec map_type(TypeSpec key, TypeSpec value) {
    TypeSpec type;
    type.kind = TypeSpec::Kind::Map;
    type.key = std::make_shared<TypeSpec>(std::move(key));
    type.value = std::make_shared<TypeSpec>(std::move(value));
    return type;
}

TypeSpec weak_type(TypeSpec target) {
    TypeSpec type;
    type.kind = TypeSpec::Kind::Weak;
    type.weak_target = std::make_shared<TypeSpec>(std::move(target));
    return type;
}

TypeSpec ephemeron_type(TypeSpec key, TypeSpec value) {
    TypeSpec type;
    type.kind = TypeSpec::Kind::Ephemeron;
    type.key = std::make_shared<TypeSpec>(std::move(key));
    type.value = std::make_shared<TypeSpec>(std::move(value));
    return type;
}

TypeSpec type_parameter_type(std::string name, std::size_t index,
                             SourcePosition position) {
    TypeSpec type;
    type.kind = TypeSpec::Kind::TypeParameter;
    type.name = std::move(name);
    type.position = position;
    type.type_parameter_index = index;
    return type;
}

bool operator==(const TypeSpec& lhs, const TypeSpec& rhs) {
    if (lhs.kind != rhs.kind) {
        return false;
    }
    if (lhs.kind == TypeSpec::Kind::Named) {
        if (lhs.named_type_index.has_value() && rhs.named_type_index.has_value()) {
            return lhs.named_type_index == rhs.named_type_index;
        }
        return lhs.name == rhs.name;
    }
    if (lhs.kind == TypeSpec::Kind::TypeParameter) {
        return lhs.type_parameter_index == rhs.type_parameter_index &&
               lhs.name == rhs.name;
    }
    if (lhs.kind == TypeSpec::Kind::Record) {
        if (lhs.record_layout_index.has_value() &&
            rhs.record_layout_index.has_value()) {
            return lhs.record_layout_index == rhs.record_layout_index;
        }
        return lhs.name == rhs.name;
    }
    if (lhs.kind == TypeSpec::Kind::Variant) {
        if (lhs.variant_layout_index.has_value() &&
            rhs.variant_layout_index.has_value()) {
            return lhs.variant_layout_index == rhs.variant_layout_index;
        }
        return lhs.name == rhs.name;
    }
    if (lhs.kind == TypeSpec::Kind::Array) {
        if (lhs.element == nullptr || rhs.element == nullptr) {
            return lhs.element == rhs.element;
        }
        return *lhs.element == *rhs.element;
    }
    if (lhs.kind == TypeSpec::Kind::Function) {
        if (lhs.function_parameters.size() != rhs.function_parameters.size() ||
            lhs.function_return == nullptr || rhs.function_return == nullptr ||
            *lhs.function_return != *rhs.function_return) {
            return false;
        }
        for (std::size_t i = 0; i < lhs.function_parameters.size(); ++i) {
            if (lhs.function_parameters[i] != rhs.function_parameters[i]) {
                return false;
            }
        }
        return true;
    }
    if (lhs.kind == TypeSpec::Kind::Map) {
        if (lhs.key == nullptr || rhs.key == nullptr || lhs.value == nullptr ||
            rhs.value == nullptr) {
            return lhs.key == rhs.key && lhs.value == rhs.value;
        }
        return *lhs.key == *rhs.key && *lhs.value == *rhs.value;
    }
    if (lhs.kind == TypeSpec::Kind::Weak) {
        if (lhs.weak_target == nullptr || rhs.weak_target == nullptr) {
            return lhs.weak_target == rhs.weak_target;
        }
        return *lhs.weak_target == *rhs.weak_target;
    }
    if (lhs.kind == TypeSpec::Kind::Ephemeron) {
        if (lhs.key == nullptr || rhs.key == nullptr || lhs.value == nullptr ||
            rhs.value == nullptr) return lhs.key == rhs.key && lhs.value == rhs.value;
        return *lhs.key == *rhs.key && *lhs.value == *rhs.value;
    }
    if (!lhs.has_pair_fields() && !rhs.has_pair_fields()) {
        return true;
    }
    if (lhs.has_pair_fields() != rhs.has_pair_fields()) {
        return false;
    }
    return *lhs.left == *rhs.left && *lhs.right == *rhs.right;
}

bool operator!=(const TypeSpec& lhs, const TypeSpec& rhs) {
    return !(lhs == rhs);
}

bool is_invalid(const TypeSpec& type) {
    return type.kind == TypeSpec::Kind::Invalid;
}

bool is_pair(const TypeSpec& type) {
    return type.kind == TypeSpec::Kind::Pair || type.kind == TypeSpec::Kind::Named;
}

Type public_type(const TypeSpec& type) {
    switch (type.kind) {
    case TypeSpec::Kind::Int64:
        return Type::Int64;
    case TypeSpec::Kind::Bool:
        return Type::Bool;
    case TypeSpec::Kind::Str:
        return Type::Str;
    case TypeSpec::Kind::Pair:
        return Type::Pair;
    case TypeSpec::Kind::Array:
        return Type::Array;
    case TypeSpec::Kind::Function:
        return Type::Function;
    case TypeSpec::Kind::Map:
        return Type::Map;
    case TypeSpec::Kind::Weak:
        return Type::Weak;
    case TypeSpec::Kind::Ephemeron:
        return Type::Ephemeron;
    case TypeSpec::Kind::TypeParameter:
        return Type::Invalid;
    case TypeSpec::Kind::Named:
        return Type::Pair;
    case TypeSpec::Kind::Record:
        return Type::Record;
    case TypeSpec::Kind::Variant:
        return Type::Variant;
    case TypeSpec::Kind::Nil:
        return Type::Invalid;
    case TypeSpec::Kind::Invalid:
        return Type::Invalid;
    }
    return Type::Invalid;
}

std::string type_name(const TypeSpec& type) {
    switch (type.kind) {
    case TypeSpec::Kind::Int64:
        return "i64";
    case TypeSpec::Kind::Bool:
        return "bool";
    case TypeSpec::Kind::Str:
        return "str";
    case TypeSpec::Kind::Pair:
        if (type.has_pair_fields()) {
            return "pair<" + type_name(*type.left) + ", " + type_name(*type.right) + ">";
        }
        return "pair";
    case TypeSpec::Kind::Array:
        if (type.element != nullptr) {
            return "[" + type_name(*type.element) + "]";
        }
        return "[invalid]";
    case TypeSpec::Kind::Function: {
        std::string rendered = "fn(";
        for (std::size_t i = 0; i < type.function_parameters.size(); ++i) {
            if (i != 0) {
                rendered += ", ";
            }
            rendered += type_name(type.function_parameters[i]);
        }
        rendered += ") -> ";
        rendered += type.function_return == nullptr
                        ? "invalid"
                        : type_name(*type.function_return);
        return rendered;
    }
    case TypeSpec::Kind::Map:
        if (type.key != nullptr && type.value != nullptr) {
            return "map<" + type_name(*type.key) + ", " +
                   type_name(*type.value) + ">";
        }
        return "map<invalid, invalid>";
    case TypeSpec::Kind::Weak:
        return type.weak_target == nullptr
                   ? "weak<invalid>"
                   : "weak<" + type_name(*type.weak_target) + ">";
    case TypeSpec::Kind::Ephemeron:
        return type.key == nullptr || type.value == nullptr
                   ? "ephemeron<invalid, invalid>"
                   : "ephemeron<" + type_name(*type.key) + ", " +
                         type_name(*type.value) + ">";
    case TypeSpec::Kind::TypeParameter:
        return type.name;
    case TypeSpec::Kind::Named:
        return type.name;
    case TypeSpec::Kind::Record:
        return type.name;
    case TypeSpec::Kind::Variant:
        return type.name;
    case TypeSpec::Kind::Nil:
        return "nil";
    case TypeSpec::Kind::Invalid:
        return "invalid";
    }
    return "invalid";
}

TypeSpec join_types(const TypeSpec& lhs, const TypeSpec& rhs) {
    if (is_invalid(lhs)) {
        return rhs;
    }
    if (is_invalid(rhs)) {
        return lhs;
    }
    if (lhs == rhs) {
        return lhs;
    }
    if (lhs.kind == TypeSpec::Kind::Nil &&
        (is_pair(rhs) || rhs.kind == TypeSpec::Kind::Record ||
         rhs.kind == TypeSpec::Kind::Variant)) {
        return rhs;
    }
    if (rhs.kind == TypeSpec::Kind::Nil &&
        (is_pair(lhs) || lhs.kind == TypeSpec::Kind::Record ||
         lhs.kind == TypeSpec::Kind::Variant)) {
        return lhs;
    }
    if (lhs.kind == TypeSpec::Kind::Array && rhs.kind == TypeSpec::Kind::Array &&
        lhs.element != nullptr && rhs.element != nullptr && *lhs.element == *rhs.element) {
        return lhs;
    }
    if (is_pair(lhs) && is_pair(rhs)) {
        return pair_type();
    }
    return invalid_type();
}

namespace {

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {
        std::size_t brace_depth = 0;
        for (std::size_t i = 0; i + 2 < tokens_.size(); ++i) {
            if (brace_depth == 0 && tokens_[i].kind == TokenKind::Fn &&
                tokens_[i + 1].kind == TokenKind::Identifier &&
                tokens_[i + 2].kind == TokenKind::Less) {
                generic_function_names_.insert(tokens_[i + 1].text);
            }
            if (tokens_[i].kind == TokenKind::LBrace) {
                ++brace_depth;
            } else if (tokens_[i].kind == TokenKind::RBrace &&
                       brace_depth != 0) {
                --brace_depth;
            }
        }
    }

    std::optional<Program> parse() {
        Program program;
        while (check(TokenKind::Type) || check(TokenKind::Record) ||
               check(TokenKind::Variant) ||
               (check(TokenKind::Fn) && check_next(TokenKind::Identifier))) {
            if (match(TokenKind::Type)) {
                auto declaration = parse_type_declaration(previous());
                if (declaration.has_value()) {
                    program.types.push_back(std::move(*declaration));
                } else {
                    synchronize();
                }
            } else if (match(TokenKind::Record)) {
                auto declaration = parse_record_declaration(previous());
                if (declaration.has_value()) {
                    program.records.push_back(std::move(*declaration));
                } else {
                    synchronize();
                }
            } else if (match(TokenKind::Variant)) {
                auto declaration = parse_variant_declaration(previous());
                if (declaration.has_value()) {
                    program.variants.push_back(std::move(*declaration));
                } else {
                    synchronize();
                }
            } else if (match(TokenKind::Fn)) {
                auto declaration = parse_function(previous());
                if (declaration.has_value()) {
                    if (declaration->type_parameters.empty()) {
                        program.functions.push_back(std::move(*declaration));
                    } else {
                        program.generic_functions.push_back(
                            std::move(*declaration));
                    }
                } else {
                    synchronize();
                }
            } else {
                break;
            }
        }

        while (!check(TokenKind::End)) {
            if (starts_statement()) {
                auto statement = parse_statement();
                if (statement.has_value()) {
                    program.statements.push_back(std::move(*statement));
                } else {
                    synchronize();
                }
                continue;
            }

            auto result = parse_expression();
            if (!result) {
                synchronize();
                break;
            }
            program.result = std::move(result);
            break;
        }

        if (!program.result && diagnostics_.empty()) {
            add_diagnostic(diagnostics_, peek().position, "program must end with an expression");
        }
        if (program.result) {
            expect(TokenKind::End, "expected end of input after final expression");
        }
        program.pair_site_count = next_pair_site_;
        program.lambdas = lambdas_;
        if (!diagnostics_.empty()) {
            return std::nullopt;
        }
        return program;
    }

    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const {
        return diagnostics_;
    }

private:
    [[nodiscard]] const Token& peek() const { return tokens_[current_]; }
    [[nodiscard]] const Token& previous() const { return tokens_[current_ - 1]; }
    [[nodiscard]] bool check(TokenKind kind) const { return peek().kind == kind; }
    [[nodiscard]] bool check_next(TokenKind kind) const {
        return current_ + 1 < tokens_.size() &&
               tokens_[current_ + 1].kind == kind;
    }
    [[nodiscard]] bool at_end() const { return check(TokenKind::End); }

    bool match(TokenKind kind) {
        if (!check(kind)) {
            return false;
        }
        ++current_;
        return true;
    }

    std::optional<Token> expect(TokenKind kind, const std::string& message) {
        if (match(kind)) {
            return previous();
        }
        add_diagnostic(diagnostics_, peek().position, message);
        return std::nullopt;
    }

    std::optional<Token> expect_field_name(const std::string& message) {
        if (match(TokenKind::Identifier) || match(TokenKind::Left) ||
            match(TokenKind::Right)) {
            return previous();
        }
        add_diagnostic(diagnostics_, peek().position, message);
        return std::nullopt;
    }

    bool looks_like_record_literal(const Token& identifier) const {
        if (!check(TokenKind::LBrace)) {
            return false;
        }
        if (record_names_.contains(identifier.text)) {
            return true;
        }
        if (current_ + 2 >= tokens_.size()) {
            return false;
        }
        const auto field_kind = tokens_[current_ + 1].kind;
        return (field_kind == TokenKind::Identifier ||
                field_kind == TokenKind::Left ||
                field_kind == TokenKind::Right) &&
               tokens_[current_ + 2].kind == TokenKind::Colon;
    }

    std::optional<TypeDecl> parse_type_declaration(const Token& type_token) {
        TypeDecl declaration;
        declaration.position = type_token.position;

        const auto name = expect(TokenKind::Identifier, "expected type name after 'type'");
        if (name.has_value()) {
            declaration.name = name->text;
            declaration.position = name->position;
        }
        expect(TokenKind::Equal, "expected '=' in type declaration");
        declaration.body_position = peek().position;
        declaration.body = parse_type();
        expect(TokenKind::Semicolon, "expected ';' after type declaration");
        return declaration;
    }

    std::optional<RecordDecl> parse_record_declaration(
        const Token& record_token) {
        RecordDecl declaration;
        declaration.position = record_token.position;
        const auto name =
            expect(TokenKind::Identifier, "expected record name after 'record'");
        if (name.has_value()) {
            declaration.name = name->text;
            declaration.position = name->position;
            record_names_.insert(name->text);
        }
        expect(TokenKind::LBrace, "expected '{' before record fields");
        if (!check(TokenKind::RBrace)) {
            do {
                RecordFieldDecl field;
                const auto field_name =
                    expect_field_name("expected record field name");
                if (field_name.has_value()) {
                    field.name = field_name->text;
                    field.position = field_name->position;
                }
                expect(TokenKind::Colon, "expected ':' after record field name");
                field.type = parse_type();
                declaration.fields.push_back(std::move(field));
            } while (match(TokenKind::Comma) && !check(TokenKind::RBrace));
        }
        expect(TokenKind::RBrace, "expected '}' after record fields");
        return declaration;
    }

    std::optional<VariantDecl> parse_variant_declaration(
        const Token& variant_token) {
        VariantDecl declaration;
        declaration.position = variant_token.position;
        const auto name = expect(TokenKind::Identifier,
                                 "expected variant name after 'variant'");
        if (name.has_value()) {
            declaration.name = name->text;
            declaration.position = name->position;
        }
        expect(TokenKind::LBrace, "expected '{' before variant cases");
        if (!check(TokenKind::RBrace)) {
            do {
                VariantCaseDecl variant_case;
                const auto case_name =
                    expect(TokenKind::Identifier, "expected variant case name");
                if (case_name.has_value()) {
                    variant_case.name = case_name->text;
                    variant_case.position = case_name->position;
                }
                expect(TokenKind::LParen,
                       "expected '(' after variant case name");
                if (!check(TokenKind::RParen)) {
                    do {
                        variant_case.fields.push_back(parse_type());
                    } while (match(TokenKind::Comma));
                }
                expect(TokenKind::RParen,
                       "expected ')' after variant case payload");
                declaration.cases.push_back(std::move(variant_case));
            } while (match(TokenKind::Comma) && !check(TokenKind::RBrace));
        }
        expect(TokenKind::RBrace, "expected '}' after variant cases");
        return declaration;
    }

    std::optional<FunctionDecl> parse_function(const Token& fn_token) {
        FunctionDecl declaration;
        declaration.position = fn_token.position;
        declaration.declaration_order = next_function_declaration_order_++;

        const auto name = expect(TokenKind::Identifier, "expected function name after 'fn'");
        if (name.has_value()) {
            declaration.name = name->text;
        }
        if (match(TokenKind::Less)) {
            if (check(TokenKind::Greater)) {
                add_diagnostic(diagnostics_, peek().position,
                               "generic function requires at least one type parameter");
            } else {
                do {
                    const auto parameter = expect(
                        TokenKind::Identifier,
                        "expected type parameter name");
                    if (parameter.has_value()) {
                        declaration.type_parameters.push_back(TypeParameterDecl{
                            parameter->text, parameter->position,
                            declaration.type_parameters.size()});
                    }
                } while (match(TokenKind::Comma));
            }
            expect(TokenKind::Greater,
                   "expected '>' after type parameters");
        }

        const auto previous_type_parameters = active_type_parameters_;
        active_type_parameters_ = declaration.type_parameters;
        expect(TokenKind::LParen, "expected '(' after function name");
        if (!check(TokenKind::RParen)) {
            do {
                Parameter parameter;
                const auto parameter_name =
                    expect(TokenKind::Identifier, "expected parameter name");
                if (parameter_name.has_value()) {
                    parameter.name = parameter_name->text;
                    parameter.position = parameter_name->position;
                }
                expect(TokenKind::Colon, "expected ':' after parameter name");
                parameter.type = parse_type();
                declaration.parameters.push_back(std::move(parameter));
            } while (match(TokenKind::Comma));
        }
        expect(TokenKind::RParen, "expected ')' after parameters");
        expect(TokenKind::Arrow, "expected '->' before function return type");
        declaration.return_type = parse_type();
        expect(TokenKind::LBrace, "expected '{' before function body");

        while (!check(TokenKind::RBrace) && !check(TokenKind::End)) {
            if (starts_statement()) {
                auto statement = parse_statement();
                if (statement.has_value()) {
                    declaration.statements.push_back(std::move(*statement));
                } else {
                    synchronize();
                }
                continue;
            }

            declaration.result = parse_expression();
            break;
        }

        if (!declaration.result && diagnostics_.empty()) {
            add_diagnostic(diagnostics_, peek().position,
                           "function body must end with an expression");
        }
        expect(TokenKind::RBrace, "expected '}' after function body");
        active_type_parameters_ = previous_type_parameters;
        return declaration;
    }

    [[nodiscard]] bool starts_statement() const {
        if (check(TokenKind::Let) || check(TokenKind::If) || check(TokenKind::While) ||
            check(TokenKind::For) || check(TokenKind::Match) || check(TokenKind::Try) ||
            check(TokenKind::Throw) ||
            check(TokenKind::Return) ||
            check(TokenKind::Break) ||
            check(TokenKind::Continue) || check(TokenKind::Print)) {
            return true;
        }
        return assignment_ahead() || ephemeron_set_ahead();
    }

    [[nodiscard]] bool ephemeron_set_ahead() const {
        return current_ + 3 < tokens_.size() &&
               tokens_[current_].kind == TokenKind::Identifier &&
               tokens_[current_ + 1].kind == TokenKind::Dot &&
               tokens_[current_ + 2].kind == TokenKind::Identifier &&
               tokens_[current_ + 2].text == "set_value" &&
               tokens_[current_ + 3].kind == TokenKind::LParen;
    }

    [[nodiscard]] bool assignment_ahead() const {
        if (!check(TokenKind::Identifier)) {
            return false;
        }
        std::size_t index = current_ + 1;
        while (index < tokens_.size()) {
            if (index + 1 < tokens_.size() && tokens_[index].kind == TokenKind::Dot &&
                (tokens_[index + 1].kind == TokenKind::Identifier ||
                 tokens_[index + 1].kind == TokenKind::Left ||
                 tokens_[index + 1].kind == TokenKind::Right)) {
                index += 2;
                continue;
            }
            if (tokens_[index].kind == TokenKind::LBracket) {
                std::size_t depth = 1;
                ++index;
                while (index < tokens_.size() && depth != 0) {
                    if (tokens_[index].kind == TokenKind::LBracket) {
                        ++depth;
                    } else if (tokens_[index].kind == TokenKind::RBracket) {
                        --depth;
                    }
                    ++index;
                }
                continue;
            }
            break;
        }
        return index < tokens_.size() && tokens_[index].kind == TokenKind::Equal;
    }

    std::optional<Statement> parse_statement() {
        if (match(TokenKind::Let)) {
            return parse_let(previous());
        }
        if (match(TokenKind::If)) {
            return parse_if(previous());
        }
        if (match(TokenKind::While)) {
            return parse_while(previous());
        }
        if (match(TokenKind::For)) {
            return parse_for_in(previous());
        }
        if (match(TokenKind::Match)) {
            return parse_match(previous());
        }
        if (match(TokenKind::Try)) {
            return parse_try(previous());
        }
        if (match(TokenKind::Throw)) {
            Statement statement;
            statement.kind = Statement::Kind::Throw;
            statement.position = previous().position;
            statement.value = parse_expression();
            expect(TokenKind::Semicolon, "expected ';' after throw expression");
            return statement;
        }
        if (match(TokenKind::Return)) {
            Statement statement;
            statement.kind = Statement::Kind::TailCall;
            statement.position = previous().position;
            const auto tail =
                expect(TokenKind::Identifier,
                       "expected contextual keyword 'tail' after 'return'");
            if (tail.has_value() && tail->text != "tail") {
                add_diagnostic(
                    diagnostics_, tail->position,
                    "expected contextual keyword 'tail' after 'return'");
            }
            statement.value = parse_expression();
            expect(TokenKind::Semicolon,
                   "expected ';' after tail call");
            return statement;
        }
        if (match(TokenKind::Break)) {
            return parse_loop_control(previous(), Statement::Kind::Break,
                                      "expected ';' after 'break'");
        }
        if (match(TokenKind::Continue)) {
            return parse_loop_control(previous(), Statement::Kind::Continue,
                                      "expected ';' after 'continue'");
        }
        if (match(TokenKind::Print)) {
            return parse_print(previous());
        }
        if (ephemeron_set_ahead()) {
            Statement statement;
            statement.kind = Statement::Kind::EphemeronSet;
            statement.position = peek().position;
            auto receiver = std::make_unique<Expr>();
            receiver->kind = Expr::Kind::Variable;
            receiver->position = peek().position;
            receiver->name = peek().text;
            ++current_;
            statement.initializer = std::move(receiver);
            expect(TokenKind::Dot, "expected '.' before set_value");
            ++current_;
            expect(TokenKind::LParen, "expected '(' after set_value");
            statement.value = parse_expression();
            expect(TokenKind::RParen, "expected ')' after ephemeron value");
            expect(TokenKind::Semicolon, "expected ';' after set_value");
            return statement;
        }
        if (assignment_ahead()) {
            return parse_assignment();
        }
        add_diagnostic(diagnostics_, peek().position, "expected statement");
        return std::nullopt;
    }

    std::optional<Statement> parse_let(const Token& let_token) {
        Statement statement;
        statement.kind = Statement::Kind::Let;
        statement.position = let_token.position;

        const auto name = expect(TokenKind::Identifier, "expected local name after 'let'");
        if (name.has_value()) {
            statement.name = name->text;
        }
        expect(TokenKind::Colon, "expected ':' after local name");
        statement.declared_type = parse_type();
        const auto equals = expect(TokenKind::Equal, "let declarations require an initializer");
        if (equals.has_value()) {
            statement.equals_position = equals->position;
        }
        statement.initializer = parse_expression();
        expect(TokenKind::Semicolon, "expected ';' after let declaration");
        return statement;
    }

    TypeSpec parse_type() {
        if (match(TokenKind::LBracket)) {
            const auto position = previous().position;
            auto element = parse_type();
            expect(TokenKind::RBracket, "expected ']' after array element type");
            auto type = array_type(std::move(element));
            type.position = position;
            return type;
        }
        if (match(TokenKind::I64)) {
            auto type = int64_type();
            type.position = previous().position;
            return type;
        }
        if (match(TokenKind::Bool)) {
            auto type = bool_type();
            type.position = previous().position;
            return type;
        }
        if (match(TokenKind::Str)) {
            auto type = str_type();
            type.position = previous().position;
            return type;
        }
        if (match(TokenKind::Map)) {
            const auto position = previous().position;
            expect(TokenKind::Less, "expected '<' after 'map'");
            auto key = parse_type();
            expect(TokenKind::Comma, "expected ',' between map key and value types");
            auto value = parse_type();
            expect(TokenKind::Greater, "expected '>' after map value type");
            auto type = map_type(std::move(key), std::move(value));
            type.position = position;
            return type;
        }
        if (match(TokenKind::Weak)) {
            const auto position = previous().position;
            expect(TokenKind::Less, "expected '<' after 'weak'");
            auto target = parse_type();
            expect(TokenKind::Greater, "expected '>' after weak target type");
            auto type = weak_type(std::move(target));
            type.position = position;
            return type;
        }
        if (match(TokenKind::Ephemeron)) {
            const auto position = previous().position;
            expect(TokenKind::Less, "expected '<' after 'ephemeron'");
            auto key = parse_type();
            expect(TokenKind::Comma, "expected ',' between ephemeron key and value types");
            auto value = parse_type();
            expect(TokenKind::Greater, "expected '>' after ephemeron value type");
            auto type = ephemeron_type(std::move(key), std::move(value));
            type.position = position;
            return type;
        }
        if (match(TokenKind::Fn)) {
            const auto position = previous().position;
            expect(TokenKind::LParen, "expected '(' after 'fn' in function type");
            std::vector<TypeSpec> parameters;
            if (!check(TokenKind::RParen)) {
                do {
                    parameters.push_back(parse_type());
                } while (match(TokenKind::Comma));
            }
            expect(TokenKind::RParen, "expected ')' after function parameter types");
            expect(TokenKind::Arrow, "expected '->' in function type");
            auto result = parse_type();
            auto type = function_type(std::move(parameters), std::move(result));
            type.position = position;
            return type;
        }
        if (match(TokenKind::Pair)) {
            const auto position = previous().position;
            if (match(TokenKind::Less)) {
                auto left = parse_type();
                expect(TokenKind::Comma, "expected ',' between pair field types");
                auto right = parse_type();
                expect(TokenKind::Greater, "expected '>' after pair field types");
                auto type = pair_type(std::move(left), std::move(right));
                type.position = position;
                return type;
            }
            auto type = pair_type();
            type.position = position;
            return type;
        }
        if (match(TokenKind::Identifier)) {
            for (const auto& parameter : active_type_parameters_) {
                if (parameter.name == previous().text) {
                    return type_parameter_type(
                        parameter.name, parameter.index,
                        previous().position);
                }
            }
            return named_type(previous().text, previous().position);
        }
        add_diagnostic(diagnostics_, peek().position,
                       "expected type 'i64', 'bool', 'str', 'pair', 'map', function type, array type, or named type");
        return invalid_type();
    }

    std::optional<Statement> parse_assignment() {
        Statement statement;
        statement.kind = Statement::Kind::Assign;
        statement.position = peek().position;
        statement.target = parse_lvalue();
        const auto equals = expect(TokenKind::Equal, "expected '=' in assignment");
        if (equals.has_value()) {
            statement.equals_position = equals->position;
        }
        statement.value = parse_expression();
        expect(TokenKind::Semicolon, "expected ';' after assignment");
        return statement;
    }

    std::optional<Statement> parse_if(const Token& if_token) {
        Statement statement;
        statement.kind = Statement::Kind::If;
        statement.position = if_token.position;
        statement.condition = parse_expression();
        statement.then_branch = parse_block("expected '{' before if body");
        if (!match(TokenKind::Else)) {
            add_diagnostic(diagnostics_, peek().position, "if statements require an else branch");
        } else {
            statement.else_branch = parse_block("expected '{' before else body");
        }
        return statement;
    }

    std::optional<Statement> parse_while(const Token& while_token) {
        Statement statement;
        statement.kind = Statement::Kind::While;
        statement.position = while_token.position;
        statement.condition = parse_expression();
        statement.body = parse_block("expected '{' before while body");
        return statement;
    }

    std::optional<Statement> parse_for_in(const Token& for_token) {
        Statement statement;
        statement.kind = Statement::Kind::ForIn;
        statement.position = for_token.position;

        const auto first =
            expect(TokenKind::Identifier, "expected loop variable after 'for'");
        if (first.has_value()) {
            statement.loop_names.push_back(first->text);
            statement.loop_name_positions.push_back(first->position);
        }
        if (match(TokenKind::Comma)) {
            const auto second = expect(TokenKind::Identifier,
                                       "expected loop variable after ','");
            if (second.has_value()) {
                statement.loop_names.push_back(second->text);
                statement.loop_name_positions.push_back(second->position);
            }
        }
        expect(TokenKind::In, "expected 'in' after loop variable");
        statement.iterable = parse_expression();
        if (match(TokenKind::DotDot)) {
            statement.range_upper = parse_expression();
        }
        statement.body = parse_block("expected '{' before for-in body");
        return statement;
    }

    std::optional<Statement> parse_print(const Token& print_token) {
        Statement statement;
        statement.kind = Statement::Kind::Print;
        statement.position = print_token.position;
        expect(TokenKind::LParen, "expected '(' after 'print'");
        statement.value = parse_expression();
        expect(TokenKind::RParen, "expected ')' after print operand");
        expect(TokenKind::Semicolon, "expected ';' after print statement");
        return statement;
    }

    std::optional<Statement> parse_try(const Token& try_token) {
        Statement statement;
        statement.kind = Statement::Kind::TryCatch;
        statement.position = try_token.position;
        statement.body = parse_block("expected '{' after 'try'");
        expect(TokenKind::Catch, "expected 'catch' after try body");
        expect(TokenKind::LParen, "expected '(' after 'catch'");
        const auto name = expect(TokenKind::Identifier, "expected catch binding name");
        if (name.has_value()) {
            statement.catch_name = name->text;
            statement.catch_position = name->position;
        }
        expect(TokenKind::Colon, "expected ':' after catch binding");
        statement.catch_type = parse_type();
        expect(TokenKind::RParen, "expected ')' after catch type");
        statement.catch_body = parse_block("expected '{' before catch body");
        return statement;
    }

    std::optional<Statement> parse_match(const Token& match_token) {
        Statement statement;
        statement.kind = Statement::Kind::Match;
        statement.position = match_token.position;
        const bool previous_record_literal_mode = allow_record_literal_;
        allow_record_literal_ = false;
        statement.condition = parse_expression();
        allow_record_literal_ = previous_record_literal_mode;
        if (!expect(TokenKind::LBrace, "expected '{' before match arms").has_value()) {
            return statement;
        }
        while (!check(TokenKind::RBrace) && !check(TokenKind::End)) {
            MatchArm arm;
            const auto case_name = expect(TokenKind::Identifier,
                                          "expected variant case name in match arm");
            if (case_name.has_value()) {
                arm.case_name = case_name->text;
                arm.position = case_name->position;
            }
            if (match(TokenKind::LParen)) {
                if (!check(TokenKind::RParen)) {
                    do {
                        const auto binding =
                            expect_field_name("expected match binding name");
                        if (binding.has_value()) {
                            arm.bindings.push_back(MatchBinding{
                                binding->text, binding->position, 0});
                        }
                    } while (match(TokenKind::Comma));
                }
                expect(TokenKind::RParen, "expected ')' after match bindings");
            }
            expect(TokenKind::FatArrow, "expected '=>' after match arm pattern");
            arm.body = parse_block("expected '{' before match arm body");
            statement.match_arms.push_back(std::move(arm));
            if (!match(TokenKind::Comma) && !check(TokenKind::RBrace)) {
                add_diagnostic(diagnostics_, peek().position,
                               "expected ',' between match arms");
                synchronize();
            }
        }
        expect(TokenKind::RBrace, "expected '}' after match arms");
        return statement;
    }

    std::optional<Statement> parse_loop_control(
        const Token& keyword, Statement::Kind kind,
        const std::string& semicolon_message) {
        Statement statement;
        statement.kind = kind;
        statement.position = keyword.position;
        expect(TokenKind::Semicolon, semicolon_message);
        return statement;
    }

    std::vector<Statement> parse_block(const std::string& open_message) {
        std::vector<Statement> statements;
        if (!expect(TokenKind::LBrace, open_message).has_value()) {
            return statements;
        }
        while (!check(TokenKind::RBrace) && !check(TokenKind::End)) {
            auto statement = parse_statement();
            if (statement.has_value()) {
                statements.push_back(std::move(*statement));
            } else {
                synchronize();
            }
        }
        expect(TokenKind::RBrace, "expected '}' after block");
        return statements;
    }

    LValue parse_lvalue() {
        LValue value;
        const auto name = expect(TokenKind::Identifier, "expected assignment target");
        if (name.has_value()) {
            value.base_name = name->text;
            value.base_position = name->position;
        }
        while (true) {
            if (match(TokenKind::Dot)) {
                const auto field = expect_field_name("expected field name");
                if (field.has_value()) {
                    LValueStep step;
                    step.kind = LValueStep::Kind::Field;
                    step.name = field->text;
                    step.position = field->position;
                    value.steps.push_back(std::move(step));
                }
                continue;
            }
            if (match(TokenKind::LBracket)) {
                LValueStep step;
                step.kind = LValueStep::Kind::Index;
                step.position = previous().position;
                step.index = parse_expression();
                expect(TokenKind::RBracket, "expected ']' after array index");
                value.steps.push_back(std::move(step));
                continue;
            }
            break;
        }
        return value;
    }

    std::unique_ptr<Expr> parse_expression() { return parse_equality(); }

    std::unique_ptr<Expr> parse_equality() {
        auto expression = parse_comparison();
        while (match(TokenKind::EqualEqual) || match(TokenKind::BangEqual)) {
            const auto operation = previous();
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::Binary;
            node->position = expression->position;
            node->operator_position = operation.position;
            node->binary_op = operation.kind == TokenKind::EqualEqual ? '=' : '!';
            node->left = std::move(expression);
            node->right = parse_comparison();
            expression = std::move(node);
        }
        return expression;
    }

    std::unique_ptr<Expr> parse_comparison() {
        auto expression = parse_addition();
        while (match(TokenKind::Less) || match(TokenKind::LessEqual) ||
               match(TokenKind::Greater) || match(TokenKind::GreaterEqual)) {
            const auto operation = previous();
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::Binary;
            node->position = expression->position;
            node->operator_position = operation.position;
            switch (operation.kind) {
            case TokenKind::Less:
                node->binary_op = '<';
                break;
            case TokenKind::LessEqual:
                node->binary_op = 'L';
                break;
            case TokenKind::Greater:
                node->binary_op = '>';
                break;
            case TokenKind::GreaterEqual:
                node->binary_op = 'G';
                break;
            default:
                throw std::logic_error("comparison parser matched non-comparison token");
            }
            node->left = std::move(expression);
            node->right = parse_addition();
            expression = std::move(node);
        }
        return expression;
    }

    std::unique_ptr<Expr> parse_addition() {
        auto expression = parse_postfix();
        while (match(TokenKind::Plus)) {
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::Binary;
            node->position = expression->position;
            node->operator_position = previous().position;
            node->binary_op = '+';
            node->left = std::move(expression);
            node->right = parse_postfix();
            expression = std::move(node);
        }
        return expression;
    }

    std::unique_ptr<Expr> parse_postfix() {
        auto expression = parse_primary();
        bool keep_parsing = true;
        while (keep_parsing) {
            if (expression->kind == Expr::Kind::Variable &&
                generic_function_names_.contains(expression->name) &&
                match(TokenKind::Less)) {
                auto node = std::make_unique<Expr>();
                node->kind = Expr::Kind::Call;
                node->position = expression->position;
                node->type_arguments_position = previous().position;
                node->receiver = std::move(expression);
                if (check(TokenKind::Greater)) {
                    add_diagnostic(
                        diagnostics_, peek().position,
                        "generic call requires at least one type argument");
                } else {
                    do {
                        node->explicit_type_arguments.push_back(parse_type());
                    } while (match(TokenKind::Comma));
                }
                expect(TokenKind::Greater,
                       "expected '>' after explicit type arguments");
                expect(TokenKind::LParen,
                       "expected '(' after explicit type arguments");
                if (!check(TokenKind::RParen)) {
                    do {
                        node->arguments.push_back(parse_expression());
                    } while (match(TokenKind::Comma));
                }
                expect(TokenKind::RParen,
                       "expected ')' after call arguments");
                expression = std::move(node);
            } else if (match(TokenKind::Dot)) {
                if (match(TokenKind::Left) || match(TokenKind::Right)) {
                    auto node = std::make_unique<Expr>();
                    node->kind = Expr::Kind::Field;
                    node->position = previous().position;
                    node->name = previous().text;
                    node->receiver = std::move(expression);
                    expression = std::move(node);
                } else if (check(TokenKind::Identifier) && peek().text == "has" &&
                           check_next(TokenKind::LParen)) {
                    auto node = std::make_unique<Expr>();
                    node->kind = Expr::Kind::MapHas;
                    node->position = peek().position;
                    node->receiver = std::move(expression);
                    ++current_;
                    expect(TokenKind::LParen, "expected '(' after 'has'");
                    node->left = parse_expression();
                    expect(TokenKind::RParen, "expected ')' after map key");
                    expression = std::move(node);
                } else if (check(TokenKind::Identifier) && peek().text == "get" &&
                           check_next(TokenKind::LParen)) {
                    auto node = std::make_unique<Expr>();
                    node->kind = Expr::Kind::WeakGet;
                    node->position = peek().position;
                    node->receiver = std::move(expression);
                    ++current_;
                    expect(TokenKind::LParen, "expected '(' after 'get'");
                    expect(TokenKind::RParen, "weak get takes no arguments");
                    expression = std::move(node);
                } else if (check(TokenKind::Identifier) &&
                           (peek().text == "key" || peek().text == "value") &&
                           check_next(TokenKind::LParen)) {
                    auto node = std::make_unique<Expr>();
                    node->kind = peek().text == "key" ? Expr::Kind::EphemeronKey
                                                       : Expr::Kind::EphemeronValue;
                    node->position = peek().position;
                    node->receiver = std::move(expression);
                    ++current_;
                    expect(TokenKind::LParen, "expected '(' after ephemeron getter");
                    expect(TokenKind::RParen, "ephemeron getter takes no arguments");
                    expression = std::move(node);
                } else if (check(TokenKind::Identifier) && peek().text == "sub" &&
                           check_next(TokenKind::LParen)) {
                    auto node = std::make_unique<Expr>();
                    node->kind = Expr::Kind::StrSub;
                    node->position = peek().position;
                    node->receiver = std::move(expression);
                    ++current_;
                    expect(TokenKind::LParen, "expected '(' after 'sub'");
                    if (!check(TokenKind::RParen)) {
                        do {
                            node->arguments.push_back(parse_expression());
                        } while (match(TokenKind::Comma));
                    }
                    expect(TokenKind::RParen, "expected ')' after sub arguments");
                    expression = std::move(node);
                } else if (match(TokenKind::Identifier)) {
                    auto node = std::make_unique<Expr>();
                    node->kind = Expr::Kind::Field;
                    node->position = previous().position;
                    node->name = previous().text;
                    node->receiver = std::move(expression);
                    expression = std::move(node);
                } else {
                    add_diagnostic(diagnostics_, peek().position,
                                   "expected field name or method");
                    break;
                }
            } else if (match(TokenKind::LBracket)) {
                auto node = std::make_unique<Expr>();
                node->kind = Expr::Kind::ArrayIndex;
                node->position = previous().position;
                node->receiver = std::move(expression);
                node->left = parse_expression();
                expect(TokenKind::RBracket, "expected ']' after array index");
                expression = std::move(node);
            } else if (match(TokenKind::LParen)) {
                auto node = std::make_unique<Expr>();
                node->kind = Expr::Kind::Call;
                node->position = expression->position;
                node->receiver = std::move(expression);
                if (!check(TokenKind::RParen)) {
                    do {
                        node->arguments.push_back(parse_expression());
                    } while (match(TokenKind::Comma));
                }
                expect(TokenKind::RParen, "expected ')' after call arguments");
                expression = std::move(node);
            } else {
                keep_parsing = false;
            }
        }
        return expression;
    }

    std::unique_ptr<Expr> parse_primary() {
        if (match(TokenKind::Fn)) {
            const auto fn_token = previous();
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::Lambda;
            node->position = fn_token.position;
            node->lambda = std::make_shared<LambdaExpr>();
            node->lambda->position = fn_token.position;

            expect(TokenKind::LParen, "expected '(' after 'fn' in lambda");
            if (!check(TokenKind::RParen)) {
                do {
                    Parameter parameter;
                    const auto name =
                        expect(TokenKind::Identifier, "expected lambda parameter name");
                    if (name.has_value()) {
                        parameter.name = name->text;
                        parameter.position = name->position;
                    }
                    expect(TokenKind::Colon,
                           "expected ':' after lambda parameter name");
                    parameter.type = parse_type();
                    node->lambda->parameters.push_back(std::move(parameter));
                } while (match(TokenKind::Comma));
            }
            expect(TokenKind::RParen, "expected ')' after lambda parameters");
            expect(TokenKind::Arrow, "expected '->' before lambda return type");
            node->lambda->return_type = parse_type();
            expect(TokenKind::LBrace, "expected '{' before lambda body");
            while (!check(TokenKind::RBrace) && !check(TokenKind::End)) {
                if (starts_statement()) {
                    auto statement = parse_statement();
                    if (statement.has_value()) {
                        node->lambda->statements.push_back(std::move(*statement));
                    } else {
                        synchronize();
                    }
                    continue;
                }
                node->lambda->result = parse_expression();
                break;
            }
            if (!node->lambda->result && diagnostics_.empty()) {
                add_diagnostic(diagnostics_, peek().position,
                               "lambda body must end with an expression");
            }
            expect(TokenKind::RBrace, "expected '}' after lambda body");
            lambdas_.push_back(node->lambda);
            return node;
        }
        if (match(TokenKind::Integer)) {
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::IntLiteral;
            node->position = previous().position;
            node->int_value = previous().integer;
            return node;
        }
        if (match(TokenKind::True) || match(TokenKind::False)) {
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::BoolLiteral;
            node->position = previous().position;
            node->bool_value = previous().kind == TokenKind::True;
            return node;
        }
        if (match(TokenKind::String)) {
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::StringLiteral;
            node->position = previous().position;
            node->string_value = previous().text;
            return node;
        }
        if (match(TokenKind::Nil)) {
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::NilLiteral;
            node->position = previous().position;
            return node;
        }
        if (match(TokenKind::IsNil)) {
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::IsNil;
            node->position = previous().position;
            expect(TokenKind::LParen, "expected '(' after 'is_nil'");
            node->receiver = parse_expression();
            expect(TokenKind::RParen, "expected ')' after is_nil operand");
            return node;
        }
        if (match(TokenKind::Weak)) {
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::WeakConstruct;
            node->position = previous().position;
            expect(TokenKind::LParen, "expected '(' after 'weak'");
            node->receiver = parse_expression();
            expect(TokenKind::RParen, "expected ')' after weak target");
            return node;
        }
        if (match(TokenKind::Ephemeron)) {
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::EphemeronConstruct;
            node->position = previous().position;
            expect(TokenKind::LParen, "expected '(' after 'ephemeron'");
            node->left = parse_expression();
            expect(TokenKind::Comma, "expected ',' between ephemeron key and value");
            node->right = parse_expression();
            expect(TokenKind::RParen, "expected ')' after ephemeron value");
            return node;
        }
        if (match(TokenKind::ToStr) || match(TokenKind::ToI64)) {
            const auto token = previous();
            auto node = std::make_unique<Expr>();
            node->kind = token.kind == TokenKind::ToStr ? Expr::Kind::ToStr
                                                        : Expr::Kind::ToI64;
            node->position = token.position;
            expect(TokenKind::LParen,
                   token.kind == TokenKind::ToStr
                       ? "expected '(' after 'to_str'"
                       : "expected '(' after 'to_i64'");
            node->receiver = parse_expression();
            expect(TokenKind::RParen,
                   token.kind == TokenKind::ToStr
                       ? "expected ')' after to_str operand"
                       : "expected ')' after to_i64 operand");
            return node;
        }
        if (match(TokenKind::Identifier) || match(TokenKind::Left) ||
            match(TokenKind::Right)) {
            const auto identifier = previous();
            if (identifier.kind == TokenKind::Identifier &&
                allow_record_literal_ && looks_like_record_literal(identifier) &&
                match(TokenKind::LBrace)) {
                auto node = std::make_unique<Expr>();
                node->kind = Expr::Kind::RecordLiteral;
                node->position = identifier.position;
                node->name = identifier.text;
                if (!check(TokenKind::RBrace)) {
                    do {
                        const auto field =
                            expect_field_name("expected record initializer field name");
                        if (field.has_value()) {
                            node->field_names.push_back(field->text);
                            node->field_positions.push_back(field->position);
                        }
                        expect(TokenKind::Colon,
                               "expected ':' after record initializer field name");
                        node->arguments.push_back(parse_expression());
                    } while (match(TokenKind::Comma) &&
                             !check(TokenKind::RBrace));
                }
                expect(TokenKind::RBrace,
                       "expected '}' after record initializer fields");
                return node;
            }
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::Variable;
            node->position = identifier.position;
            node->name = identifier.text;
            return node;
        }
        if (match(TokenKind::Pair)) {
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::PairLiteral;
            node->position = previous().position;
            node->pair_site = next_pair_site_++;
            expect(TokenKind::LParen, "expected '(' after 'pair'");
            node->left = parse_expression();
            expect(TokenKind::Comma, "expected ',' between pair fields");
            node->right = parse_expression();
            expect(TokenKind::RParen, "expected ')' after pair fields");
            return node;
        }
        if (match(TokenKind::Array)) {
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::ArraySized;
            node->position = previous().position;
            expect(TokenKind::Less, "expected '<' after 'array'");
            node->array_element_type = parse_type();
            expect(TokenKind::Greater, "expected '>' after array element type");
            expect(TokenKind::LParen, "expected '(' after array element type");
            node->left = parse_expression();
            expect(TokenKind::Comma, "expected ',' between array length and initializer");
            node->right = parse_expression();
            expect(TokenKind::RParen, "expected ')' after array initializer");
            return node;
        }
        if (match(TokenKind::Map)) {
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::MapEmpty;
            node->position = previous().position;
            expect(TokenKind::Less, "expected '<' after 'map'");
            node->map_key_type = parse_type();
            expect(TokenKind::Comma,
                   "expected ',' between map key and value types");
            node->map_value_type = parse_type();
            expect(TokenKind::Greater, "expected '>' after map value type");
            expect(TokenKind::LParen, "expected '(' after map type");
            expect(TokenKind::RParen, "empty map constructor takes no arguments");
            return node;
        }
        if (match(TokenKind::LBracket)) {
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::ArrayLiteral;
            node->position = previous().position;
            if (!check(TokenKind::RBracket)) {
                do {
                    node->arguments.push_back(parse_expression());
                } while (match(TokenKind::Comma));
            }
            expect(TokenKind::RBracket, "expected ']' after array literal");
            return node;
        }
        if (match(TokenKind::LParen)) {
            auto expression = parse_expression();
            expect(TokenKind::RParen, "expected ')' after expression");
            return expression;
        }

        add_diagnostic(diagnostics_, peek().position, "expected expression");
        auto node = std::make_unique<Expr>();
        node->kind = Expr::Kind::IntLiteral;
        node->position = peek().position;
        node->inferred_type = invalid_type();
        if (!at_end()) {
            ++current_;
        }
        return node;
    }

    void synchronize() {
        while (!at_end()) {
            if (current_ > 0 && previous().kind == TokenKind::Semicolon) {
                return;
            }
            if (check(TokenKind::Type) || check(TokenKind::Record) ||
                check(TokenKind::Variant) ||
                check(TokenKind::Fn) ||
                check(TokenKind::Let) || check(TokenKind::If) ||
                check(TokenKind::While) || check(TokenKind::For) ||
                check(TokenKind::Print) || check(TokenKind::Return) ||
                check(TokenKind::RBrace)) {
                return;
            }
            ++current_;
        }
    }

    std::vector<Token> tokens_;
    std::size_t current_{0};
    std::size_t next_pair_site_{0};
    std::size_t next_function_declaration_order_{0};
    std::set<std::string> record_names_;
    std::set<std::string> generic_function_names_;
    std::vector<TypeParameterDecl> active_type_parameters_;
    std::vector<std::shared_ptr<LambdaExpr>> lambdas_;
    bool allow_record_literal_{true};
    std::vector<Diagnostic> diagnostics_;
};

} // namespace

ParseResult parse_tokens(std::vector<Token> tokens) {
    Parser parser(std::move(tokens));
    auto program = parser.parse();
    ParseResult result;
    if (program.has_value()) {
        result.program = std::move(*program);
    }
    result.diagnostics = std::vector<Diagnostic>(parser.diagnostics().begin(),
                                                 parser.diagnostics().end());
    return result;
}

} // namespace lang::frontend::detail
