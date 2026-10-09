/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Sigil/Compile/Lexer.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <format>
#include <optional>
#include <system_error>
#include <utility>

namespace Assisi::Sigil::Compile
{

namespace
{

/// How a token kind is written, by TokenKind, for messages.
constexpr std::array<std::string_view, static_cast<std::size_t>(TokenKind::Count_)> kSpellings{
    "a name", "a whole number", "a number", "a string", "'{'", "'}'", "'('", "')'", "';'", "':'",
    "','",    "'.'",            "'->'",     "'='",      "'+'", "'-'", "'*'", "'/'", "'%'", "'!'",
    "'&&'",   "'||'",           "'=='",     "'!='",     "'<'", "'<='", "'>'", "'>='", "the end of the file"};

/// A punctuation token: its text and kind. Longer ones come first, so `->` is
/// found before `-` and `<=` before `<`.
struct Punctuation
{
    std::string_view text;
    TokenKind kind;
};

constexpr std::array kPunctuation{
    Punctuation{"->", TokenKind::Arrow},    Punctuation{"&&", TokenKind::And},
    Punctuation{"||", TokenKind::Or},       Punctuation{"==", TokenKind::Equal},
    Punctuation{"!=", TokenKind::NotEqual}, Punctuation{"<=", TokenKind::LessEqual},
    Punctuation{">=", TokenKind::GreaterEqual}, Punctuation{"{", TokenKind::LeftBrace},
    Punctuation{"}", TokenKind::RightBrace}, Punctuation{"(", TokenKind::LeftParen},
    Punctuation{")", TokenKind::RightParen}, Punctuation{";", TokenKind::Semicolon},
    Punctuation{":", TokenKind::Colon},     Punctuation{",", TokenKind::Comma},
    Punctuation{".", TokenKind::Dot},       Punctuation{"=", TokenKind::Assign},
    Punctuation{"+", TokenKind::Plus},      Punctuation{"-", TokenKind::Minus},
    Punctuation{"*", TokenKind::Star},      Punctuation{"/", TokenKind::Slash},
    Punctuation{"%", TokenKind::Percent},   Punctuation{"!", TokenKind::Not},
    Punctuation{"<", TokenKind::Less},      Punctuation{">", TokenKind::Greater},
};

bool IsNameStart(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool IsDigit(char c)
{
    return c >= '0' && c <= '9';
}

bool IsNamePart(char c)
{
    return IsNameStart(c) || IsDigit(c);
}

/// Walks the source a character at a time, keeping the line and column.
class Cursor
{
  public:
    Cursor(std::string_view source, std::string_view file) : _source(source), _file(file) {}

    [[nodiscard]] bool AtEnd() const { return _offset >= _source.size(); }
    [[nodiscard]] char Peek(std::size_t ahead = 0) const
    {
        return _offset + ahead < _source.size() ? _source[_offset + ahead] : '\0';
    }
    [[nodiscard]] bool StartsWith(std::string_view text) const { return _source.substr(_offset).starts_with(text); }
    [[nodiscard]] SourceLocation Where() const { return _where; }
    [[nodiscard]] std::size_t Offset() const { return _offset; }
    [[nodiscard]] std::string_view Since(std::size_t start) const { return _source.substr(start, _offset - start); }

    void Advance(std::size_t count = 1)
    {
        for (std::size_t i = 0; i < count && !AtEnd(); ++i)
        {
            if (_source[_offset] == '\n')
            {
                ++_where.line;
                _where.column = 1;
            }
            else
            {
                ++_where.column;
            }
            ++_offset;
        }
    }

    void Fail(SourceLocation where, std::string message)
    {
        _errors.push_back(Diagnostic{.message = std::move(message), .file = std::string{_file}, .where = where});
    }

    [[nodiscard]] Diagnostics &Errors() { return _errors; }

  private:
    std::string_view _source;
    std::string_view _file;
    Diagnostics _errors;
    std::size_t _offset = 0;
    SourceLocation _where;
};

/// Skips whitespace and comments, reporting a block comment that never ends.
void SkipSpace(Cursor &cursor)
{
    while (!cursor.AtEnd())
    {
        const char c = cursor.Peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            cursor.Advance();
        }
        else if (cursor.StartsWith("//"))
        {
            while (!cursor.AtEnd() && cursor.Peek() != '\n')
            {
                cursor.Advance();
            }
        }
        else if (cursor.StartsWith("/*"))
        {
            const SourceLocation start = cursor.Where();
            cursor.Advance(2);
            while (!cursor.AtEnd() && !cursor.StartsWith("*/"))
            {
                cursor.Advance();
            }
            if (cursor.AtEnd())
            {
                cursor.Fail(start, "this comment is never closed with */");
                return;
            }
            cursor.Advance(2);
        }
        else
        {
            return;
        }
    }
}

Token LexString(Cursor &cursor)
{
    Token token{.text = {}, .where = cursor.Where(), .kind = TokenKind::String};
    cursor.Advance();
    while (!cursor.AtEnd() && cursor.Peek() != '"' && cursor.Peek() != '\n')
    {
        if (cursor.Peek() != '\\')
        {
            token.text += cursor.Peek();
            cursor.Advance();
            continue;
        }
        const char escaped = cursor.Peek(1);
        if (escaped == '"' || escaped == '\\')
        {
            token.text += escaped;
        }
        else
        {
            cursor.Fail(cursor.Where(), "the only escapes in a string are \\\" and \\\\");
        }
        cursor.Advance(2);
    }
    if (cursor.Peek() != '"')
    {
        cursor.Fail(token.where, "this string is never closed with \"");
        return token;
    }
    cursor.Advance();
    return token;
}

Token LexNumber(Cursor &cursor)
{
    Token token{.text = {}, .where = cursor.Where(), .kind = TokenKind::Int};
    const std::size_t start = cursor.Offset();
    while (IsDigit(cursor.Peek()))
    {
        cursor.Advance();
    }
    if (cursor.Peek() == '.' && IsDigit(cursor.Peek(1)))
    {
        token.kind = TokenKind::Float;
        cursor.Advance();
        while (IsDigit(cursor.Peek()))
        {
            cursor.Advance();
        }
    }
    if (IsNameStart(cursor.Peek()))
    {
        while (IsNamePart(cursor.Peek()))
        {
            cursor.Advance();
        }
        cursor.Fail(token.where, std::format("\"{}\": a name can't start with a digit", cursor.Since(start)));
    }
    token.text = std::string{cursor.Since(start)};
    if (token.kind == TokenKind::Int)
    {
        int32_t value = 0;
        const std::from_chars_result read =
            std::from_chars(token.text.data(), token.text.data() + token.text.size(), value);
        if (read.ec == std::errc::result_out_of_range)
        {
            cursor.Fail(token.where, std::format("{} is too large for an int", token.text));
        }
    }
    return token;
}

std::optional<Token> LexPunctuation(Cursor &cursor)
{
    for (const Punctuation &punctuation : kPunctuation)
    {
        if (cursor.StartsWith(punctuation.text))
        {
            Token token{.text = std::string{punctuation.text}, .where = cursor.Where(), .kind = punctuation.kind};
            cursor.Advance(punctuation.text.size());
            return token;
        }
    }
    return std::nullopt;
}

} // namespace

std::string_view Describe(TokenKind kind)
{
    return kSpellings[static_cast<std::size_t>(kind)];
}

std::expected<std::vector<Token>, Diagnostics> Lex(std::string_view source, std::string_view file)
{
    Cursor cursor{source, file};
    std::vector<Token> tokens;
    while (true)
    {
        SkipSpace(cursor);
        if (cursor.AtEnd())
        {
            break;
        }
        const char c = cursor.Peek();
        if (c == '"')
        {
            tokens.push_back(LexString(cursor));
        }
        else if (IsDigit(c))
        {
            tokens.push_back(LexNumber(cursor));
        }
        else if (IsNameStart(c))
        {
            const SourceLocation where = cursor.Where();
            const std::size_t start = cursor.Offset();
            while (IsNamePart(cursor.Peek()))
            {
                cursor.Advance();
            }
            tokens.push_back(Token{.text = std::string{cursor.Since(start)}, .where = where, .kind = TokenKind::Name});
        }
        else if (std::optional<Token> punctuation = LexPunctuation(cursor); punctuation.has_value())
        {
            tokens.push_back(std::move(*punctuation));
        }
        else
        {
            cursor.Fail(cursor.Where(), std::format("'{}' isn't part of Sigil", c));
            cursor.Advance();
        }
    }
    tokens.push_back(Token{.text = {}, .where = cursor.Where(), .kind = TokenKind::End});
    if (!cursor.Errors().empty())
    {
        return std::unexpected(std::move(cursor.Errors()));
    }
    return tokens;
}

} // namespace Assisi::Sigil::Compile
