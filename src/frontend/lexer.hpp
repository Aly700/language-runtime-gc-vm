#pragma once

#include "lang/frontend/type_checker.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lang::frontend::detail {

enum class TokenKind {
    End,
    Identifier,
    Integer,
    Let,
    Type,
    Fn,
    If,
    Else,
    While,
    True,
    False,
    Nil,
    IsNil,
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

struct LexResult {
    std::vector<Token> tokens;
    std::vector<Diagnostic> diagnostics;
};

LexResult lex_source(std::string_view source);

} // namespace lang::frontend::detail
