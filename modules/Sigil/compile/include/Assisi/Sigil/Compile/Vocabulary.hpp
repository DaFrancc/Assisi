/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Vocabulary.hpp
/// @brief The words a domain adds to Sigil, as data the compiler checks files
///        against: block kinds, clauses, value types and functions.
///
/// A vocabulary brings no parsing code. Every file has the same shape, and the
/// vocabulary only says which kinds and words may fill it, so files for
/// different domains read alike and the errors in them read alike too.

#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Sigil::Compile
{

/// @brief A type name in a clause or function: one of these, or the name of a
///        value type the vocabulary declares.
namespace TypeNames
{
inline constexpr std::string_view kFloat = "float";
inline constexpr std::string_view kInt = "int";
inline constexpr std::string_view kBool = "bool";
/// Either number type. Where a function takes several, they are worked out as
/// one: int if all are ints, float otherwise; a `numeric` result is that type.
inline constexpr std::string_view kNumeric = "numeric";
} // namespace TypeNames

/// @brief A type the vocabulary adds, such as a clip. Its values are strings
///        written in the file, which `validate` accepts or explains the refusal
///        of; a param of the type is filled in from outside and checked there.
struct ValueType
{
    std::string name;
    std::function<std::expected<void, std::string>(std::string_view value)> validate;
};

/// @brief A kind of block, such as `state`.
struct BlockKind
{
    std::string name;
    /// The kinds this may be written inside.
    std::vector<std::string> parents;
    /// Whether it may be written at the top level of the file.
    bool topLevel = false;
    /// Whether the blocks inside it are states joined by transitions, which
    /// is what allows transitions in it and runs the graph checks on it.
    bool holdsStates = false;
};

enum class ArgumentKind : uint8_t
{
    Value, ///< An expression of the argument's type.
    State, ///< A bare name of a state: a sibling of the block the clause is in,
           ///< or one of a transition's own states.
    Count_,
};

struct ArgumentSpec
{
    std::string type; ///< Unused for a State argument.
    ArgumentKind kind = ArgumentKind::Value;
};

enum class Cardinality : uint8_t
{
    AtMostOnce,
    ExactlyOnce,
    Any,
    Count_,
};

/// @brief A clause, such as `play clip;`.
struct ClauseSpec
{
    std::string word;
    std::vector<ArgumentSpec> arguments;
    /// The block kinds it may be written in.
    std::vector<std::string> blocks;
    /// Whether it may be written in a transition's braces.
    bool onTransition = false;
    Cardinality cardinality = Cardinality::AtMostOnce;
};

enum class FunctionUse : uint8_t
{
    Anywhere,
    /// Only where a transition is decided, like a trigger: its value only
    /// means something at the moment a transition is chosen.
    WhenOnly,
    Count_,
};

/// @brief A function a file can call. Every function only reads.
struct FunctionSpec
{
    std::string name;
    std::vector<std::string> parameters;
    std::string result;
    FunctionUse use = FunctionUse::Anywhere;
};

struct Vocabulary
{
    std::string name;
    std::vector<ValueType> types;
    std::vector<BlockKind> blocks;
    std::vector<ClauseSpec> clauses;
    std::vector<FunctionSpec> functions;
};

/// @brief The functions every file has: abs, min, max and clamp.
[[nodiscard]] const std::vector<FunctionSpec> &CoreFunctions();

/// @brief Why @p vocabulary can't be used, if it can't: a word that is also a
///        core word or another of its words, or a kind or type it names but
///        doesn't declare.
[[nodiscard]] std::expected<void, std::string> CheckVocabulary(const Vocabulary &vocabulary);

} // namespace Assisi::Sigil::Compile
