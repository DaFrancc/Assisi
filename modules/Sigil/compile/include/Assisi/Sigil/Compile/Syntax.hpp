/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Syntax.hpp
/// @brief A Sigil file as written: its statements and blocks, before any name
///        is looked up or any type worked out.
///
/// The parser builds this from the shape of the text alone and knows no
/// vocabulary, so a block kind or a clause word here is only a name. Every part
/// keeps where it was written, for the checker's errors.

#include <Assisi/Sigil/Compile/Diagnostic.hpp>
#include <Assisi/Sigil/Compile/Lexer.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Assisi::Sigil::Compile::Syntax
{

/// @brief Words reserved in every Sigil file, whatever its vocabulary.
inline constexpr std::array<std::string_view, 15> kCoreWords{
    "use",  "sigiltype", "import", "param", "const", "let",   "enum",   "when",
    "any",  "true",      "false",  "float", "int",   "bool",  "trigger"};

/// @brief Whether @p word is one of kCoreWords.
[[nodiscard]] bool IsCoreWord(std::string_view word);

/// @brief A name, and where it was written.
struct Named
{
    std::string name;
    SourceLocation where;

    /// The stretch of source the name covers.
    [[nodiscard]] Span Extent() const
    {
        return Span{.where = where, .length = static_cast<uint32_t>(name.empty() ? 1 : name.size())};
    }
};

enum class ExprKind : uint8_t
{
    Int,
    Float,
    String,
    Bool,
    Name,   ///< `text` is the name.
    Member, ///< `text.member`: an enum's value.
    Unary,  ///< `op` on operands[0].
    Binary, ///< operands[0] `op` operands[1].
    Call,   ///< `text(operands...)`.
    Count_,
};

struct Expr
{
    std::vector<Expr> operands;
    /// The literal as written (a string's contents), the name, the enum of a
    /// member, or the function called.
    std::string text;
    std::string member;
    /// The whole expression, operands and all.
    Span extent;
    /// The part an error about the expression itself points at: the operator
    /// of a unary or binary one, the name of a call, the literal or name of a leaf.
    Span anchor;
    ExprKind kind = ExprKind::Int;
    TokenKind op = TokenKind::End;
};

/// @brief `use <vocabulary>;`
struct Use
{
    Named vocabulary;
};

/// @brief `import "<path>";`
struct Import
{
    std::string path;
    SourceLocation where;
    /// The quoted path as written.
    Span pathSpan;
};

/// @brief `enum Name { a, b }`
struct EnumDecl
{
    Named name;
    std::vector<Named> values;
};

/// @brief `param name: type;`
struct ParamDecl
{
    Named name;
    Named type;
};

/// @brief `const name = value;` or `let name = value;`, with an optional type.
struct ValueDecl
{
    Named name;
    std::optional<Named> type;
    Expr value;
    bool isConst = false;
};

/// @brief `word arguments...;` in a block or after a transition.
struct Clause
{
    Named word;
    std::vector<Expr> arguments;
};

/// @brief One `+ state` or `- state` after a transition's first source.
struct SourceStep
{
    Named state;
    bool remove = false;
};

/// @brief `sources -> target when condition { clauses };`
struct Transition
{
    /// The first source, `any` or a state, then each `+` or `-` step.
    Named first;
    std::vector<SourceStep> steps;
    std::vector<Clause> clauses;
    Named target;
    Expr condition;
    bool fromAny = false;
};

/// @brief `kind name { ... }`. The whole file is a block with no kind or name.
///
/// Child blocks and transitions are kept apart: the order among blocks says
/// which state comes first, and the order among transitions which wins, but
/// nothing depends on the order of one against the other.
struct Block
{
    Named kind;
    Named name;
    std::vector<Block> blocks;
    std::vector<Clause> clauses;
    std::vector<Transition> transitions;
};

using Declaration = std::variant<Import, EnumDecl, ParamDecl, ValueDecl>;

struct File
{
    Use use;
    /// `sigiltype <kind>;`, when the file has one.
    std::optional<Named> sigilType;
    /// In the order written, which is the order consts and lets may use each other.
    std::vector<Declaration> declarations;
    Block root;
};

} // namespace Assisi::Sigil::Compile::Syntax
