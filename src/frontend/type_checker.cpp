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
    Equal,
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

struct Expr {
    enum class Kind {
        IntLiteral,
        BoolLiteral,
        Variable,
        PairLiteral,
        Binary,
        Field,
    };

    Kind kind{Kind::IntLiteral};
    SourcePosition position;
    SourcePosition operator_position;
    std::int64_t int_value{0};
    bool bool_value{false};
    std::string name;
    char binary_op{0};
    std::size_t pair_site{0};
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
    std::unique_ptr<Expr> receiver;
    Type inferred_type{Type::Invalid};
    std::set<std::size_t> object_sites;
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
    Type declared_type{Type::Invalid};
    std::uint32_t local_index{0};
    std::unique_ptr<Expr> initializer;
    LValue target;
    std::unique_ptr<Expr> value;
    std::unique_ptr<Expr> condition;
    std::vector<Statement> then_branch;
    std::vector<Statement> else_branch;
    std::vector<Statement> body;
};

struct Program {
    std::vector<Statement> statements;
    std::unique_ptr<Expr> result;
    std::size_t pair_site_count{0};
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    std::optional<Program> parse() {
        Program program;
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

    Type parse_type() {
        if (match(TokenKind::I64)) {
            return Type::Int64;
        }
        if (match(TokenKind::Bool)) {
            return Type::Bool;
        }
        if (match(TokenKind::Pair)) {
            return Type::Pair;
        }
        add_diagnostic(diagnostics_, peek().position, "expected type 'i64', 'bool', or 'pair'");
        return Type::Invalid;
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
        while (match(TokenKind::Dot)) {
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
        node->inferred_type = Type::Invalid;
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
            if (check(TokenKind::Let) || check(TokenKind::If) || check(TokenKind::While) ||
                check(TokenKind::RBrace)) {
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
    Type type{Type::Invalid};
    std::set<std::size_t> object_sites;
};

struct FieldState {
    TypedValue left;
    TypedValue right;
};

struct LocalState {
    std::string name;
    Type declared_type{Type::Invalid};
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

TypedValue invalid_value() { return TypedValue{Type::Invalid, {}}; }
TypedValue scalar_value(Type type) { return TypedValue{type, {}}; }

TypedValue pair_value(std::size_t site) {
    TypedValue value;
    value.type = Type::Pair;
    value.object_sites.insert(site);
    return value;
}

TypedValue join_values(const TypedValue& lhs, const TypedValue& rhs) {
    if (lhs.type == Type::Invalid) {
        return rhs;
    }
    if (rhs.type == Type::Invalid) {
        return lhs;
    }
    if (lhs.type != rhs.type) {
        return invalid_value();
    }
    TypedValue result = lhs;
    if (result.type == Type::Pair) {
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

class TypeChecker {
public:
    explicit TypeChecker(std::size_t pair_site_count) {
        state_.fields_by_site.resize(pair_site_count);
    }

    Type check(Program& program) {
        for (auto& statement : program.statements) {
            check_statement(statement, state_, true);
        }
        const auto result = check_expr(*program.result, state_);
        return result.type;
    }

    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const {
        return diagnostics_;
    }

    [[nodiscard]] std::uint32_t local_count() const {
        return static_cast<std::uint32_t>(state_.locals.size());
    }

private:
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
        if (initializer.type != Type::Invalid &&
            initializer.type != statement.declared_type) {
            diagnose(statement.equals_position,
                     "cannot initialize local '" + statement.name + "' of type " +
                         std::string(type_name(statement.declared_type)) + " with " +
                         type_name(initializer.type));
            return;
        }

        const auto index = static_cast<std::uint32_t>(state.locals.size());
        statement.local_index = index;
        state.locals.push_back(LocalState{statement.name,
                                          statement.declared_type,
                                          index,
                                          true,
                                          initializer,
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
            if (assigned.type != Type::Invalid && assigned.type != local->declared_type) {
                diagnose(statement.equals_position,
                         "cannot assign " + std::string(type_name(assigned.type)) +
                             " to local '" + local->name + "' of type " +
                             type_name(local->declared_type));
                return;
            }
            local->initialized = true;
            local->value = assigned;
            return;
        }

        const auto receiver = check_lvalue_prefix(statement.target, state,
                                                 statement.target.fields.size() - 1);
        const auto& field = statement.target.fields.back();
        if (receiver.type == Type::Invalid || assigned.type == Type::Invalid) {
            return;
        }
        if (receiver.type != Type::Pair) {
            diagnose(field.position, "field assignment requires pair");
            return;
        }

        auto existing = load_field(receiver, field.name, field.position, state);
        if (existing.type == Type::Invalid) {
            return;
        }
        if (existing.type != assigned.type) {
            diagnose(statement.equals_position,
                     "cannot assign " + std::string(type_name(assigned.type)) +
                         " to field '" + field.name + "' of type " +
                         type_name(existing.type));
            return;
        }

        store_field(receiver, field.name, assigned, state);
    }

    void check_if(Statement& statement, FlowState& state) {
        const auto condition = check_expr(*statement.condition, state);
        if (condition.type != Type::Invalid && condition.type != Type::Bool) {
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
        for (std::size_t iteration = 0; iteration < max_loop_iterations(); ++iteration) {
            auto body_input = head;
            const auto condition = check_expr(*statement.condition, body_input);
            if (condition.type != Type::Invalid && condition.type != Type::Bool) {
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

    std::size_t max_loop_iterations() const {
        return state_.fields_by_site.size() + state_.locals.size() + 8;
    }

    TypedValue check_expr(Expr& expression, FlowState& state) {
        switch (expression.kind) {
        case Expr::Kind::IntLiteral:
            return annotate(expression, scalar_value(Type::Int64));
        case Expr::Kind::BoolLiteral:
            return annotate(expression, scalar_value(Type::Bool));
        case Expr::Kind::Variable:
            return check_variable(expression, state);
        case Expr::Kind::PairLiteral:
            return check_pair_literal(expression, state);
        case Expr::Kind::Binary:
            return check_binary(expression, state);
        case Expr::Kind::Field:
            return check_field_access(expression, state);
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
        return annotate(expression, pair_value(expression.pair_site));
    }

    TypedValue check_binary(Expr& expression, FlowState& state) {
        const auto left = check_expr(*expression.left, state);
        const auto right = check_expr(*expression.right, state);
        if (expression.binary_op == '+') {
            if ((left.type != Type::Invalid && left.type != Type::Int64) ||
                (right.type != Type::Invalid && right.type != Type::Int64)) {
                diagnose(expression.operator_position, "operator '+' requires i64 operands");
                return annotate(expression, invalid_value());
            }
            return annotate(expression, scalar_value(Type::Int64));
        }
        if (expression.binary_op == '<') {
            if ((left.type != Type::Invalid && left.type != Type::Int64) ||
                (right.type != Type::Invalid && right.type != Type::Int64)) {
                diagnose(expression.operator_position, "operator '<' requires i64 operands");
                return annotate(expression, invalid_value());
            }
            return annotate(expression, scalar_value(Type::Bool));
        }
        diagnose(expression.operator_position, "unknown binary operator");
        return annotate(expression, invalid_value());
    }

    TypedValue check_field_access(Expr& expression, FlowState& state) {
        const auto receiver = check_expr(*expression.receiver, state);
        const auto field = load_field(receiver, expression.name, expression.position, state);
        return annotate(expression, field);
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
            if (current.type == Type::Invalid) {
                return current;
            }
        }
        return current;
    }

    TypedValue load_field(const TypedValue& receiver, const std::string& field,
                          SourcePosition position, const FlowState& state) {
        if (receiver.type == Type::Invalid) {
            return invalid_value();
        }
        if (receiver.type != Type::Pair) {
            diagnose(position, "field access requires pair");
            return invalid_value();
        }
        if (receiver.object_sites.empty()) {
            diagnose(position, "pair field type is unknown");
            return invalid_value();
        }

        std::optional<TypedValue> loaded;
        for (const auto site : receiver.object_sites) {
            if (site >= state.fields_by_site.size() ||
                !state.fields_by_site[site].has_value()) {
                diagnose(position, "pair field type is unknown");
                return invalid_value();
            }
            const auto& fields = *state.fields_by_site[site];
            const auto& value = field == "left" ? fields.left : fields.right;
            if (!loaded.has_value()) {
                loaded = value;
            } else {
                loaded = join_values(*loaded, value);
            }
        }
        if (!loaded.has_value() || loaded->type == Type::Invalid) {
            diagnose(position,
                     "field '" + field +
                         "' has incompatible types across possible pair values");
            return invalid_value();
        }
        return *loaded;
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

    FlowState state_;
    std::vector<Diagnostic> diagnostics_;
};

class Compiler {
public:
    explicit Compiler(std::uint32_t local_count) {
        function_.local_count = local_count;
    }

    Function compile(const Program& program) {
        for (const auto& statement : program.statements) {
            compile_statement(statement);
        }
        compile_expr(*program.result);
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
        return CompileResult{std::nullopt, Type::Invalid, lex_diagnostics(lexer)};
    }

    Parser parser(std::move(tokens));
    auto program = parser.parse();
    if (!program.has_value()) {
        return CompileResult{std::nullopt,
                             Type::Invalid,
                             std::vector<Diagnostic>(parser.diagnostics().begin(),
                                                     parser.diagnostics().end())};
    }

    TypeChecker checker(program->pair_site_count);
    const auto result_type = checker.check(*program);
    if (!checker.diagnostics().empty()) {
        return CompileResult{std::nullopt,
                             result_type,
                             std::vector<Diagnostic>(checker.diagnostics().begin(),
                                                     checker.diagnostics().end())};
    }

    Compiler compiler(checker.local_count());
    auto function = compiler.compile(*program);

    auto verification = verify_with_stack_maps(function);
    assert(verification.has_value() &&
           "compiler bug: type-checked source emitted verifier-rejected bytecode");
    if (!verification.has_value()) {
        return CompileResult{
            std::nullopt,
            result_type,
            {Diagnostic{SourcePosition{}, "compiler emitted verifier-rejected bytecode"}}};
    }

    function.stack_maps = verification->stack_maps;
    assert(verify_with_stack_maps(function).has_value() &&
           "compiler bug: verifier-generated stack maps did not round-trip");
    return CompileResult{std::move(function), result_type, {}};
}

} // namespace lang::frontend
