#include "lexer.hpp"

#include "diagnostics.hpp"

#include <charconv>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace lang::frontend::detail {
namespace {

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
            if (c == '"') {
                tokens.push_back(string_literal(start));
                continue;
            }
            if (c == '-' && offset_ + 1 < source_.size() && source_[offset_ + 1] == '>') {
                advance();
                advance();
                tokens.push_back(simple(TokenKind::Arrow, start, "->"));
                continue;
            }
            if (c == '=' && offset_ + 1 < source_.size() &&
                source_[offset_ + 1] == '>') {
                advance();
                advance();
                tokens.push_back(simple(TokenKind::FatArrow, start, "=>"));
                continue;
            }
            if (c == '=' && offset_ + 1 < source_.size() &&
                source_[offset_ + 1] == '=') {
                advance();
                advance();
                tokens.push_back(simple(TokenKind::EqualEqual, start, "=="));
                continue;
            }
            if (c == '!' && offset_ + 1 < source_.size() &&
                source_[offset_ + 1] == '=') {
                advance();
                advance();
                tokens.push_back(simple(TokenKind::BangEqual, start, "!="));
                continue;
            }
            if (c == '<' && offset_ + 1 < source_.size() &&
                source_[offset_ + 1] == '=') {
                advance();
                advance();
                tokens.push_back(simple(TokenKind::LessEqual, start, "<="));
                continue;
            }
            if (c == '>' && offset_ + 1 < source_.size() &&
                source_[offset_ + 1] == '=') {
                advance();
                advance();
                tokens.push_back(simple(TokenKind::GreaterEqual, start, ">="));
                continue;
            }
            if (c == '.' && offset_ + 1 < source_.size() &&
                source_[offset_ + 1] == '.') {
                advance();
                advance();
                tokens.push_back(simple(TokenKind::DotDot, start, ".."));
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
            case '[':
                tokens.push_back(simple(TokenKind::LBracket, start, "["));
                break;
            case ']':
                tokens.push_back(simple(TokenKind::RBracket, start, "]"));
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
        } else if (text == "type") {
            kind = TokenKind::Type;
        } else if (text == "record") {
            kind = TokenKind::Record;
        } else if (text == "variant") {
            kind = TokenKind::Variant;
        } else if (text == "match") {
            kind = TokenKind::Match;
        } else if (text == "try") {
            kind = TokenKind::Try;
        } else if (text == "catch") {
            kind = TokenKind::Catch;
        } else if (text == "throw") {
            kind = TokenKind::Throw;
        } else if (text == "return") {
            kind = TokenKind::Return;
        } else if (text == "fn") {
            kind = TokenKind::Fn;
        } else if (text == "if") {
            kind = TokenKind::If;
        } else if (text == "else") {
            kind = TokenKind::Else;
        } else if (text == "while") {
            kind = TokenKind::While;
        } else if (text == "for") {
            kind = TokenKind::For;
        } else if (text == "in") {
            kind = TokenKind::In;
        } else if (text == "break") {
            kind = TokenKind::Break;
        } else if (text == "continue") {
            kind = TokenKind::Continue;
        } else if (text == "true") {
            kind = TokenKind::True;
        } else if (text == "false") {
            kind = TokenKind::False;
        } else if (text == "nil") {
            kind = TokenKind::Nil;
        } else if (text == "is_nil") {
            kind = TokenKind::IsNil;
        } else if (text == "array") {
            kind = TokenKind::Array;
        } else if (text == "map") {
            kind = TokenKind::Map;
        } else if (text == "weak") {
            kind = TokenKind::Weak;
        } else if (text == "ephemeron") {
            kind = TokenKind::Ephemeron;
        } else if (text == "builder") {
            kind = TokenKind::Builder;
        } else if (text == "print") {
            kind = TokenKind::Print;
        } else if (text == "to_str") {
            kind = TokenKind::ToStr;
        } else if (text == "to_i64") {
            kind = TokenKind::ToI64;
        } else if (text == "intern") {
            kind = TokenKind::Intern;
        } else if (text == "abs") {
            kind = TokenKind::Abs;
        } else if (text == "min") {
            kind = TokenKind::Min;
        } else if (text == "max") {
            kind = TokenKind::Max;
        } else if (text == "i64") {
            kind = TokenKind::I64;
        } else if (text == "bool") {
            kind = TokenKind::Bool;
        } else if (text == "str") {
            kind = TokenKind::Str;
        } else if (text == "pair") {
            kind = TokenKind::Pair;
        } else if (text == "left") {
            kind = TokenKind::Left;
        } else if (text == "right") {
            kind = TokenKind::Right;
        }
        return Token{kind, start, std::move(text), 0};
    }

    Token string_literal(SourcePosition start) {
        advance();
        std::string decoded;
        bool terminated = false;
        while (!at_end()) {
            if (peek() == '"') {
                advance();
                terminated = true;
                break;
            }
            if (peek() != '\\') {
                decoded.push_back(peek());
                advance();
                continue;
            }

            const auto escape_position = position();
            advance();
            if (at_end()) {
                break;
            }
            const char escaped = peek();
            advance();
            switch (escaped) {
            case 'n':
                decoded.push_back('\n');
                break;
            case 't':
                decoded.push_back('\t');
                break;
            case '\\':
                decoded.push_back('\\');
                break;
            case '"':
                decoded.push_back('"');
                break;
            default:
                add_diagnostic(diagnostics_, escape_position,
                               std::string("unsupported string escape '\\") +
                                   escaped + "'");
                break;
            }
        }
        if (!terminated) {
            add_diagnostic(diagnostics_, start, "unterminated string literal");
        }
        return Token{TokenKind::String, start, std::move(decoded), 0};
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

} // namespace

LexResult lex_source(std::string_view source) {
    Lexer lexer(source);
    auto tokens = lexer.lex();
    return LexResult{std::move(tokens),
                     std::vector<Diagnostic>(lexer.diagnostics().begin(),
                                             lexer.diagnostics().end())};
}

} // namespace lang::frontend::detail
