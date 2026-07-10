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
    case TypeSpec::Kind::Named:
        return Type::Pair;
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
    case TypeSpec::Kind::Named:
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
    if (lhs.kind == TypeSpec::Kind::Nil && is_pair(rhs)) {
        return rhs;
    }
    if (rhs.kind == TypeSpec::Kind::Nil && is_pair(lhs)) {
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
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    std::optional<Program> parse() {
        Program program;
        while (check(TokenKind::Type) ||
               (check(TokenKind::Fn) && check_next(TokenKind::Identifier))) {
            if (match(TokenKind::Type)) {
                auto declaration = parse_type_declaration(previous());
                if (declaration.has_value()) {
                    program.types.push_back(std::move(*declaration));
                } else {
                    synchronize();
                }
            } else if (match(TokenKind::Fn)) {
                auto declaration = parse_function(previous());
                if (declaration.has_value()) {
                    program.functions.push_back(std::move(*declaration));
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

    std::optional<FunctionDecl> parse_function(const Token& fn_token) {
        FunctionDecl declaration;
        declaration.position = fn_token.position;

        const auto name = expect(TokenKind::Identifier, "expected function name after 'fn'");
        if (name.has_value()) {
            declaration.name = name->text;
        }
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
        return declaration;
    }

    [[nodiscard]] bool starts_statement() const {
        if (check(TokenKind::Let) || check(TokenKind::If) || check(TokenKind::While)) {
            return true;
        }
        return assignment_ahead();
    }

    [[nodiscard]] bool assignment_ahead() const {
        if (!check(TokenKind::Identifier)) {
            return false;
        }
        std::size_t index = current_ + 1;
        while (index < tokens_.size()) {
            if (index + 1 < tokens_.size() && tokens_[index].kind == TokenKind::Dot &&
                (tokens_[index + 1].kind == TokenKind::Left ||
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
            return named_type(previous().text, previous().position);
        }
        add_diagnostic(diagnostics_, peek().position,
                       "expected type 'i64', 'bool', 'str', 'pair', function type, array type, or named type");
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
        while (match(TokenKind::Dot)) {
            if (match(TokenKind::Left) || match(TokenKind::Right)) {
                LValueStep step;
                step.kind = LValueStep::Kind::Field;
                step.name = previous().text;
                step.position = previous().position;
                value.steps.push_back(std::move(step));
            } else {
                add_diagnostic(diagnostics_, peek().position,
                               "expected field name 'left' or 'right'");
                break;
            }
        }
        while (true) {
            if (match(TokenKind::Dot)) {
                if (match(TokenKind::Left) || match(TokenKind::Right)) {
                    LValueStep step;
                    step.kind = LValueStep::Kind::Field;
                    step.name = previous().text;
                    step.position = previous().position;
                    value.steps.push_back(std::move(step));
                } else {
                    add_diagnostic(diagnostics_, peek().position,
                                   "expected field name 'left' or 'right'");
                    break;
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
        while (match(TokenKind::Less)) {
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::Binary;
            node->position = expression->position;
            node->operator_position = previous().position;
            node->binary_op = '<';
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
            if (match(TokenKind::Dot)) {
                if (match(TokenKind::Left) || match(TokenKind::Right)) {
                    auto node = std::make_unique<Expr>();
                    node->kind = Expr::Kind::Field;
                    node->position = previous().position;
                    node->name = previous().text;
                    node->receiver = std::move(expression);
                    expression = std::move(node);
                } else if (check(TokenKind::Identifier) && peek().text == "len") {
                    auto node = std::make_unique<Expr>();
                    node->kind = Expr::Kind::ArrayLen;
                    node->position = peek().position;
                    node->receiver = std::move(expression);
                    ++current_;
                    expression = std::move(node);
                } else {
                    add_diagnostic(diagnostics_, peek().position,
                                   "expected field name 'left', 'right', or 'len'");
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
        if (match(TokenKind::Identifier)) {
            auto node = std::make_unique<Expr>();
            node->kind = Expr::Kind::Variable;
            node->position = previous().position;
            node->name = previous().text;
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
            if (check(TokenKind::Type) || check(TokenKind::Fn) ||
                check(TokenKind::Let) || check(TokenKind::If) ||
                check(TokenKind::While) || check(TokenKind::RBrace)) {
                return;
            }
            ++current_;
        }
    }

    std::vector<Token> tokens_;
    std::size_t current_{0};
    std::size_t next_pair_site_{0};
    std::vector<std::shared_ptr<LambdaExpr>> lambdas_;
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
