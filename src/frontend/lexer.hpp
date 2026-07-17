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
    String,
    Let,
    Type,
    Record,
    Variant,
    Match,
    Try,
    Catch,
    Throw,
    Fn,
    If,
    Else,
    While,
    For,
    In,
    Break,
    Continue,
    True,
    False,
    Nil,
    IsNil,
    Array,
    Map,
    Weak,
    Ephemeron,
    Print,
    ToStr,
    ToI64,
    I64,
    Bool,
    Str,
    Pair,
    Left,
    Right,
    Plus,
    Less,
    Greater,
    LessEqual,
    GreaterEqual,
    Equal,
    EqualEqual,
    BangEqual,
    Arrow,
    FatArrow,
    Colon,
    Semicolon,
    Comma,
    Dot,
    DotDot,
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
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
