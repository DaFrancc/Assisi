/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Program.hpp
/// @brief A Sigil file after checking: every name resolved, every expression
///        typed, consts worked out, and the states of each block indexed.
///
/// What the rest of the engine builds on. Consts appear in expressions as their
/// values, and an int used where a float is wanted is wrapped in a Widen node,
/// so nothing downstream has to repeat the type rules.

#include <Assisi/Sigil/Compile/Diagnostic.hpp>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace Assisi::Sigil::Compile
{

enum class TypeKind : uint8_t
{
    Float,
    Int,
    Bool,
    Trigger,
    String,     ///< A string not yet given a vocabulary type; never written in a file.
    Enum,       ///< `index` is into Program::enums.
    Vocabulary, ///< `index` is into the vocabulary's types.
    Error,      ///< An expression that failed to check; its errors are already reported.
    Count_,
};

struct Type
{
    uint32_t index = 0;
    TypeKind kind = TypeKind::Error;

    bool operator==(const Type &) const = default;
};

/// @brief A value known when the file compiles. An enum value is its index.
using Constant = std::variant<int32_t, float, bool, std::string>;

enum class Operator : uint8_t
{
    Or,
    And,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Add,
    Subtract,
    Multiply,
    Divide,    ///< Int by int rounds toward zero.
    Remainder, ///< Ints only.
    Not,
    Negate,
    Count_,
};

enum class ExprKind : uint8_t
{
    Literal, ///< `literal`, of `type`: a number, bool, string, enum value or const.
    Param,   ///< Program::params[index].
    Let,     ///< Program::lets[index].
    Unary,   ///< `op` on operands[0].
    Binary,  ///< operands[0] `op` operands[1].
    Call,    ///< Program::functions[index] on the operands.
    Widen,   ///< operands[0], an int, as a float.
    Count_,
};

struct Expr
{
    std::vector<Expr> operands;
    Constant literal;
    SourceLocation where;
    Type type;
    uint32_t index = 0;
    ExprKind kind = ExprKind::Literal;
    Operator op = Operator::Count_;
};

struct Param
{
    std::string name;
    SourceLocation where;
    Type type;
};

struct Enum
{
    std::string name;
    std::vector<std::string> values;
    /// The file it is declared in, which tells apart two enums of one name
    /// that reach a file through different imports.
    std::string origin;
};

struct Const
{
    std::string name;
    Constant value;
    /// The file it is declared in: this one, or a library it imports.
    std::string origin;
    Type type;
};

struct Let
{
    std::string name;
    Expr value;
    Type type;
    /// Whether it reads a trigger or a when-only function, so it may only
    /// decide transitions.
    bool whenOnly = false;
};

/// @brief A clause argument: a value, or for a State argument the index of
///        the state it names among the block's states.
struct Argument
{
    Expr value;
    int32_t state = -1;
};

struct Clause
{
    std::vector<Argument> arguments;
    SourceLocation where;
    uint32_t spec = 0; ///< Into the vocabulary's clauses.
};

struct Transition
{
    std::vector<uint32_t> sources; ///< States it leaves, in the order of their block's states.
    /// Params of type trigger its condition reads, directly or through lets;
    /// firing uses them up.
    std::vector<uint32_t> triggersRead;
    std::vector<Clause> clauses;
    Expr condition;
    SourceLocation where;
    uint32_t target = 0;
};

/// @brief A block. Its child blocks are its states when its kind holds states,
///        and the first is where it starts.
struct Block
{
    std::string name;
    std::vector<Block> children;
    std::vector<Clause> clauses;
    std::vector<Transition> transitions;
    SourceLocation where;
    uint32_t kind = 0; ///< Into the vocabulary's blocks; unused for the file itself.
};

struct Program
{
    std::string vocabulary;
    std::vector<Param> params;
    std::vector<Enum> enums;
    std::vector<Const> consts;
    std::vector<Let> lets;
    /// Every function a Call can name: the core ones, then the vocabulary's.
    std::vector<std::string> functions;
    /// The file itself: its children are the top-level blocks.
    Block root;
    Diagnostics warnings;
    bool library = false;
};

} // namespace Assisi::Sigil::Compile
