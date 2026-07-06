#include "lang/frontend/type_checker.hpp"

#include <cassert>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace lang::frontend {

namespace {

enum class TokenKind {
    End,
    Identifier,
    Integer,
    Let,
    Fn,
    If,
    Else,
    While,
    True,
    False,
    I64,
    Bool,
    Pair,
    Left,
    Right,
    Plus,
    Less,
    Greater,
    Equal,
    Arrow,
    Colon,
    Semicolon,
    Comma,
    Dot,
    LParen,
    RParen,
    LBrace,
    RBrace,
};

struct Token {
    TokenKind kind{TokenKind::End};
    SourcePosition position;
    std::string text;
    std::int64_t integer{0};
};

void add_diagnostic(std::vector<Diagnostic>& diagnostics, SourcePosition position,
                    std::string message) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.position.offset == position.offset &&
            diagnostic.message == message) {
            return;
        }
    }
    diagnostics.push_back(Diagnostic{position, std::move(message)});
}

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}

    std::vector<Token> lex() {
        std::vector<Token> tokens;
        while (!at_end()) {
            skip_ignored();
            if (at_end()) {
                break;
            }

            const auto start = position();
            const char c = peek();
            if (is_identifier_start(c)) {
                tokens.push_back(identifier(start));
                continue;
            }
            if (c == '-' && offset_ + 1 < source_.size() && source_[offset_ + 1] == '>') {
                advance();
                advance();
                tokens.push_back(simple(TokenKind::Arrow, start, "->"));
                continue;
            }
            if (std::isdigit(static_cast<unsigned char>(c)) ||
                (c == '-' && offset_ + 1 < source_.size() &&
                 std::isdigit(static_cast<unsigned char>(source_[offset_ + 1])))) {
                tokens.push_back(integer(start));
                continue;
            }

            advance();
            switch (c) {
            case '+':
                tokens.push_back(simple(TokenKind::Plus, start, "+"));
                break;
            case '<':
                tokens.push_back(simple(TokenKind::Less, start, "<"));
                break;
            case '>':
                tokens.push_back(simple(TokenKind::Greater, start, ">"));
                break;
            case '=':
                tokens.push_back(simple(TokenKind::Equal, start, "="));
                break;
            case ':':
                tokens.push_back(simple(TokenKind::Colon, start, ":"));
                break;
            case ';':
                tokens.push_back(simple(TokenKind::Semicolon, start, ";"));
                break;
            case ',':
                tokens.push_back(simple(TokenKind::Comma, start, ","));
                break;
            case '.':
                tokens.push_back(simple(TokenKind::Dot, start, "."));
                break;
            case '(':
                tokens.push_back(simple(TokenKind::LParen, start, "("));
                break;
            case ')':
                tokens.push_back(simple(TokenKind::RParen, start, ")"));
                break;
            case '{':
                tokens.push_back(simple(TokenKind::LBrace, start, "{"));
                break;
            case '}':
                tokens.push_back(simple(TokenKind::RBrace, start, "}"));
                break;
            default:
                add_diagnostic(diagnostics_, start,
                               std::string("unexpected character '") + c + "'");
                break;
            }
        }
        tokens.push_back(Token{TokenKind::End, position(), "", 0});
        return tokens;
    }

    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const {
        return diagnostics_;
    }

private:
    [[nodiscard]] bool at_end() const { return offset_ >= source_.size(); }
    [[nodiscard]] char peek() const { return source_[offset_]; }

    [[nodiscard]] SourcePosition position() const {
        return SourcePosition{offset_, line_, column_};
    }

    static bool is_identifier_start(char c) {
        return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
    }

    static bool is_identifier_continue(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    }

    void advance() {
        if (at_end()) {
            return;
        }
        const char c = source_[offset_++];
        if (c == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
    }

    void skip_ignored() {
        bool progressed = true;
        while (progressed && !at_end()) {
            progressed = false;
            while (!at_end() && std::isspace(static_cast<unsigned char>(peek()))) {
                advance();
                progressed = true;
            }
            if (!at_end() && peek() == '/' && offset_ + 1 < source_.size() &&
                source_[offset_ + 1] == '/') {
                while (!at_end() && peek() != '\n') {
                    advance();
                }
                progressed = true;
            }
        }
    }

    static Token simple(TokenKind kind, SourcePosition position, std::string text) {
        return Token{kind, position, std::move(text), 0};
    }

    Token identifier(SourcePosition start) {
        const auto begin = offset_;
        while (!at_end() && is_identifier_continue(peek())) {
            advance();
        }
        std::string text(source_.substr(begin, offset_ - begin));
        TokenKind kind = TokenKind::Identifier;
        if (text == "let") {
            kind = TokenKind::Let;
        } else if (text == "fn") {
            kind = TokenKind::Fn;
        } else if (text == "if") {
            kind = TokenKind::If;
        } else if (text == "else") {
            kind = TokenKind::Else;
        } else if (text == "while") {
            kind = TokenKind::While;
        } else if (text == "true") {
            kind = TokenKind::True;
        } else if (text == "false") {
            kind = TokenKind::False;
        } else if (text == "i64") {
            kind = TokenKind::I64;
        } else if (text == "bool") {
            kind = TokenKind::Bool;
        } else if (text == "pair") {
            kind = TokenKind::Pair;
        } else if (text == "left") {
            kind = TokenKind::Left;
        } else if (text == "right") {
            kind = TokenKind::Right;
        }
        return Token{kind, start, std::move(text), 0};
    }

    Token integer(SourcePosition start) {
        const auto begin = offset_;
        if (peek() == '-') {
            advance();
        }
        while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) {
            advance();
        }

        const auto view = source_.substr(begin, offset_ - begin);
        std::int64_t value = 0;
        const auto* first = view.data();
        const auto* last = view.data() + view.size();
        const auto parsed = std::from_chars(first, last, value);
        if (parsed.ec != std::errc{} || parsed.ptr != last) {
            add_diagnostic(diagnostics_, start, "integer literal is outside i64 range");
        }
        return Token{TokenKind::Integer, start, std::string(view), value};
    }

    std::string_view source_;
    std::size_t offset_{0};
    std::size_t line_{1};
    std::size_t column_{1};
    std::vector<Diagnostic> diagnostics_;
};

struct TypeSpec {
    enum class Kind {
        Int64,
        Bool,
        Pair,
        Invalid,
    };

    Kind kind{Kind::Invalid};
    std::shared_ptr<TypeSpec> left;
    std::shared_ptr<TypeSpec> right;

    [[nodiscard]] bool has_pair_fields() const {
        return kind == Kind::Pair && left != nullptr && right != nullptr;
    }
};

TypeSpec int64_type() { return TypeSpec{TypeSpec::Kind::Int64, nullptr, nullptr}; }
TypeSpec bool_type() { return TypeSpec{TypeSpec::Kind::Bool, nullptr, nullptr}; }
TypeSpec pair_type() { return TypeSpec{TypeSpec::Kind::Pair, nullptr, nullptr}; }
TypeSpec invalid_type() { return TypeSpec{TypeSpec::Kind::Invalid, nullptr, nullptr}; }

TypeSpec pair_type(TypeSpec left, TypeSpec right) {
    TypeSpec type;
    type.kind = TypeSpec::Kind::Pair;
    type.left = std::make_shared<TypeSpec>(std::move(left));
    type.right = std::make_shared<TypeSpec>(std::move(right));
    return type;
}

bool operator==(const TypeSpec& lhs, const TypeSpec& rhs) {
    if (lhs.kind != rhs.kind) {
        return false;
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
    return type.kind == TypeSpec::Kind::Pair;
}

Type public_type(const TypeSpec& type) {
    switch (type.kind) {
    case TypeSpec::Kind::Int64:
        return Type::Int64;
    case TypeSpec::Kind::Bool:
        return Type::Bool;
    case TypeSpec::Kind::Pair:
        return Type::Pair;
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
    case TypeSpec::Kind::Pair:
        if (type.has_pair_fields()) {
            return "pair<" + type_name(*type.left) + ", " + type_name(*type.right) + ">";
        }
        return "pair";
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
    if (is_pair(lhs) && is_pair(rhs)) {
        return pair_type();
    }
    return invalid_type();
}

struct Expr {
    enum class Kind {
        IntLiteral,
        BoolLiteral,
        Variable,
        PairLiteral,
        Binary,
        Field,
        Call,
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

struct Program {
    std::vector<FunctionDecl> functions;
    std::vector<Statement> statements;
    std::unique_ptr<Expr> result;
    std::uint32_t entry_local_count{0};
    std::size_t pair_site_count{0};
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    std::optional<Program> parse() {
        Program program;
        while (match(TokenKind::Fn)) {
            auto declaration = parse_function(previous());
            if (declaration.has_value()) {
                program.functions.push_back(std::move(*declaration));
            } else {
                synchronize();
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
        while (index + 1 < tokens_.size() && tokens_[index].kind == TokenKind::Dot &&
               (tokens_[index + 1].kind == TokenKind::Left ||
                tokens_[index + 1].kind == TokenKind::Right)) {
            index += 2;
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
        if (match(TokenKind::I64)) {
            return int64_type();
        }
        if (match(TokenKind::Bool)) {
            return bool_type();
        }
        if (match(TokenKind::Pair)) {
            if (match(TokenKind::Less)) {
                auto left = parse_type();
                expect(TokenKind::Comma, "expected ',' between pair field types");
                auto right = parse_type();
                expect(TokenKind::Greater, "expected '>' after pair field types");
                return pair_type(std::move(left), std::move(right));
            }
            return pair_type();
        }
        add_diagnostic(diagnostics_, peek().position,
                       "expected type 'i64', 'bool', or 'pair'");
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
                value.fields.push_back(FieldStep{previous().text, previous().position});
            } else {
                add_diagnostic(diagnostics_, peek().position,
                               "expected field name 'left' or 'right'");
                break;
            }
        }
        return value;
    }

    std::unique_ptr<Expr> parse_expression() { return parse_comparison(); }

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
                } else {
                    add_diagnostic(diagnostics_, peek().position,
                                   "expected field name 'left' or 'right'");
                    break;
                }
            } else if (match(TokenKind::LParen)) {
                auto node = std::make_unique<Expr>();
                node->kind = Expr::Kind::Call;
                node->position = expression->position;
                if (expression->kind == Expr::Kind::Variable) {
                    node->name = expression->name;
                } else {
                    add_diagnostic(diagnostics_, node->position,
                                   "call target must be a function name");
                }
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
            if (check(TokenKind::Fn) || check(TokenKind::Let) || check(TokenKind::If) ||
                check(TokenKind::While) || check(TokenKind::RBrace)) {
                return;
            }
            ++current_;
        }
    }

    std::vector<Token> tokens_;
    std::size_t current_{0};
    std::size_t next_pair_site_{0};
    std::vector<Diagnostic> diagnostics_;
};

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

ValueKind bytecode_kind(const TypeSpec& type) {
    switch (type.kind) {
    case TypeSpec::Kind::Int64:
        return ValueKind::Int64;
    case TypeSpec::Kind::Bool:
        return ValueKind::Bool;
    case TypeSpec::Kind::Pair:
        return ValueKind::Object;
    case TypeSpec::Kind::Invalid:
        return ValueKind::Nil;
    }
    return ValueKind::Nil;
}

SignatureValue signature_value_from_type(const TypeSpec& type) {
    if (type.has_pair_fields()) {
        return pair_signature(signature_value_from_type(*type.left),
                              signature_value_from_type(*type.right));
    }
    return signature_value(bytecode_kind(type));
}

FunctionSignature signature_from_types(const std::vector<Parameter>& parameters,
                                       const TypeSpec& return_type) {
    FunctionSignature signature;
    signature.parameters.reserve(parameters.size());
    signature.parameter_types.reserve(parameters.size());
    for (const auto& parameter : parameters) {
        signature.parameters.push_back(bytecode_kind(parameter.type));
        signature.parameter_types.push_back(signature_value_from_type(parameter.type));
    }
    signature.return_type = bytecode_kind(return_type);
    signature.return_type_detail = signature_value_from_type(return_type);
    assert(signature.parameter_types.size() == signature.parameters.size());
    assert(signature.return_type_detail->kind == signature.return_type);
    return signature;
}

class Compiler {
public:
    Compiler(std::uint32_t local_count, FunctionSignature signature) {
        function_.signature = std::move(signature);
        function_.local_count = local_count;
    }

    Function compile(const std::vector<Statement>& statements, const Expr& result) {
        for (const auto& statement : statements) {
            compile_statement(statement);
        }
        compile_expr(result);
        // Verifier accommodation: source programs always end in a final expression, and
        // the compiler emits an explicit Return immediately after it so bytecode cannot
        // fall off the end.
        emit(OpCode::Return, 0);
        return function_;
    }

private:
    std::size_t emit(OpCode op, std::int64_t operand) {
        function_.code.push_back(Instruction{op, operand});
        return function_.code.size() - 1;
    }

    void patch(std::size_t instruction, std::size_t target) {
        assert(instruction < function_.code.size());
        assert(target <= static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()));
        function_.code[instruction].operand = static_cast<std::int64_t>(target);
    }

    std::size_t pc() const { return function_.code.size(); }

    void compile_statement(const Statement& statement) {
        switch (statement.kind) {
        case Statement::Kind::Let:
            // Verifier accommodation: every source local is introduced by a top-level
            // let with an initializer, and the initializer is stored before any later
            // LoadLocal can be emitted for that local.
            compile_expr(*statement.initializer);
            emit(OpCode::StoreLocal, statement.local_index);
            break;
        case Statement::Kind::Assign:
            compile_assignment(statement);
            break;
        case Statement::Kind::If:
            compile_if(statement);
            break;
        case Statement::Kind::While:
            compile_while(statement);
            break;
        }
    }

    void compile_assignment(const Statement& statement) {
        if (statement.target.fields.empty()) {
            compile_expr(*statement.value);
            emit(OpCode::StoreLocal, statement.target.local_index);
            return;
        }

        // Verifier accommodation: SetLeft/SetRight consumes receiver then value and leaves
        // no stack result, so field-assignment statements compile as stack-neutral blocks.
        compile_lvalue_receiver(statement.target);
        compile_expr(*statement.value);
        const auto& field = statement.target.fields.back();
        emit(field.name == "left" ? OpCode::SetLeft : OpCode::SetRight, 0);
    }

    void compile_if(const Statement& statement) {
        compile_expr(*statement.condition);
        const auto jump_to_else = emit(OpCode::JumpIfFalse, -1);
        for (const auto& inner : statement.then_branch) {
            compile_statement(inner);
        }
        // Verifier accommodation: both branches must enter the merge with the same stack
        // height. Source blocks are statements only, and this jump prevents then fallthrough
        // from executing else bytecode while still making every emitted pc reachable.
        const auto jump_to_end = emit(OpCode::Jump, -1);
        const auto else_pc = pc();
        patch(jump_to_else, else_pc);
        for (const auto& inner : statement.else_branch) {
            compile_statement(inner);
        }
        patch(jump_to_end, pc());
    }

    void compile_while(const Statement& statement) {
        const auto header = pc();
        compile_expr(*statement.condition);
        const auto jump_to_exit = emit(OpCode::JumpIfFalse, -1);
        for (const auto& inner : statement.body) {
            compile_statement(inner);
        }
        // Verifier accommodation: loop bodies are stack-neutral and locals keep their
        // declared source type, matching the verifier's strict merge at the backedge.
        emit(OpCode::Jump, static_cast<std::int64_t>(header));
        patch(jump_to_exit, pc());
    }

    void compile_expr(const Expr& expression) {
        switch (expression.kind) {
        case Expr::Kind::IntLiteral:
            emit(OpCode::ConstantI64, expression.int_value);
            break;
        case Expr::Kind::BoolLiteral:
            compile_bool_literal(expression.bool_value);
            break;
        case Expr::Kind::Variable:
            emit(OpCode::LoadLocal, expression.local_index);
            break;
        case Expr::Kind::PairLiteral:
            compile_expr(*expression.left);
            compile_expr(*expression.right);
            emit(OpCode::AllocPair, 0);
            break;
        case Expr::Kind::Binary:
            compile_expr(*expression.left);
            compile_expr(*expression.right);
            emit(expression.binary_op == '+' ? OpCode::AddI64 : OpCode::LessI64, 0);
            break;
        case Expr::Kind::Field:
            compile_expr(*expression.receiver);
            emit(expression.name == "left" ? OpCode::GetLeft : OpCode::GetRight, 0);
            break;
        case Expr::Kind::Call:
            for (const auto& argument : expression.arguments) {
                compile_expr(*argument);
            }
            emit(OpCode::Call, static_cast<std::int64_t>(expression.callee_index));
            break;
        }
    }

    void compile_bool_literal(bool value) {
        // Verifier accommodation: the VM has no Bool literal opcode. Emitting a constant
        // comparison lets the verifier prove the result kind is Bool before JumpIfFalse or
        // StoreLocal consumes it.
        emit(OpCode::ConstantI64, value ? 0 : 1);
        emit(OpCode::ConstantI64, value ? 1 : 0);
        emit(OpCode::LessI64, 0);
    }

    void compile_lvalue_receiver(const LValue& lvalue) {
        emit(OpCode::LoadLocal, lvalue.local_index);
        for (std::size_t i = 0; i + 1 < lvalue.fields.size(); ++i) {
            emit(lvalue.fields[i].name == "left" ? OpCode::GetLeft : OpCode::GetRight, 0);
        }
    }

    Function function_;
};

std::vector<Diagnostic> lex_diagnostics(const Lexer& lexer) {
    return std::vector<Diagnostic>(lexer.diagnostics().begin(), lexer.diagnostics().end());
}

} // namespace

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
    Lexer lexer(source);
    auto tokens = lexer.lex();
    if (!lexer.diagnostics().empty()) {
        CompileResult result;
        result.result_type = Type::Invalid;
        result.diagnostics = lex_diagnostics(lexer);
        return result;
    }

    Parser parser(std::move(tokens));
    auto program = parser.parse();
    if (!program.has_value()) {
        CompileResult result;
        result.result_type = Type::Invalid;
        result.diagnostics = std::vector<Diagnostic>(parser.diagnostics().begin(),
                                                     parser.diagnostics().end());
        return result;
    }

    TypeChecker checker(program->pair_site_count);
    const auto result_type = checker.check(*program);
    const auto coarse_result_type = public_type(result_type);
    if (!checker.diagnostics().empty()) {
        CompileResult result;
        result.result_type = coarse_result_type;
        result.diagnostics = std::vector<Diagnostic>(checker.diagnostics().begin(),
                                                     checker.diagnostics().end());
        return result;
    }

    Module module;
    module.entry_function = 0;
    module.functions.reserve(program->functions.size() + 1);

    FunctionSignature entry_signature;
    entry_signature.return_type = bytecode_kind(result_type);
    entry_signature.return_type_detail = signature_value_from_type(result_type);
    Compiler entry_compiler(program->entry_local_count, std::move(entry_signature));
    module.functions.push_back(entry_compiler.compile(program->statements, *program->result));

    for (const auto& declaration : program->functions) {
        Compiler function_compiler(declaration.local_count,
                                   signature_from_types(declaration.parameters,
                                                        declaration.return_type));
        module.functions.push_back(
            function_compiler.compile(declaration.statements, *declaration.result));
    }

    auto verification = verify_with_stack_maps(module);
    assert(verification.has_value() &&
           "compiler bug: type-checked source emitted verifier-rejected module");
    if (!verification.has_value()) {
        CompileResult result;
        result.result_type = coarse_result_type;
        result.diagnostics = {
            Diagnostic{SourcePosition{}, "compiler emitted verifier-rejected module"}};
        return result;
    }

    for (std::size_t i = 0; i < module.functions.size(); ++i) {
        module.functions[i].stack_maps = verification->functions[i].stack_maps;
    }
    assert(verify_with_stack_maps(module).has_value() &&
           "compiler bug: verifier-generated stack maps did not round-trip");

    CompileResult result;
    result.result_type = coarse_result_type;
    result.module = std::move(module);
    result.function = result.module->functions.at(result.module->entry_function);
    return result;
}

} // namespace lang::frontend
