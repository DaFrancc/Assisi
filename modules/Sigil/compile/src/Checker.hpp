/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Checker.hpp
/// @brief What the checker's passes share while they turn a Syntax::File into
///        a Program: the names in scope, and the diagnostics so far.

#include <Assisi/Sigil/Compile/Diagnostic.hpp>
#include <Assisi/Sigil/Compile/Program.hpp>
#include <Assisi/Sigil/Compile/Syntax.hpp>
#include <Assisi/Sigil/Compile/Vocabulary.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Assisi::Sigil::Compile::Detail
{

enum class SymbolKind : uint8_t
{
    Param,
    Const,
    Let,
    Enum,
    Function,
    Count_,
};

/// @brief A name in the file's one namespace.
struct Symbol
{
    /// Its name where it was declared, or the path of the import it came from.
    Span span{};
    uint32_t index = 0; ///< Into the Program list its kind names.
    int32_t from = -1; ///< Into Checker::imports when it came from one; -1 when declared here.
    SymbolKind kind = SymbolKind::Param;
    bool used = false;
};

struct ImportRecord
{
    std::string path;
    Span span; ///< The path as written.
    bool used = false;
};

/// @brief What an expression may read.
enum class ExprMode : uint8_t
{
    Constant, ///< A const's value: literals, enum values, other consts and core functions.
    Formula,  ///< Anything declared above it.
    Count_,
};

struct Checker
{
    const Vocabulary &vocabulary;
    std::string file{};
    Program program{};
    Diagnostics diagnostics{};
    std::unordered_map<std::string, Symbol> symbols{};
    /// Consts and lets declared further down, so using one too early says so
    /// rather than calling it unknown.
    std::unordered_map<std::string, Span> declaredLater{};
    std::vector<ImportRecord> imports{};
    /// The core functions, then the vocabulary's; Program::functions in names.
    std::vector<FunctionSpec> functions{};
    /// Names whose declaration was refused. Using one says nothing more: the
    /// refusal already explained it.
    std::unordered_set<std::string> refused{};
    /// The source, a line per entry, for suggestions that quote it.
    std::vector<std::string_view> lines{};
    /// The const or let whose value is being checked, if one is.
    const Syntax::ValueDecl *declaring = nullptr;
};

/// @brief The text of line @p line of the source, or nothing past its end.
[[nodiscard]] std::string_view LineOf(const Checker &checker, uint32_t line);

/// @brief A suggestion that turns the `const` or `let` keyword of @p declaration
///        into @p keyword.
[[nodiscard]] Suggestion SwapKeyword(const Checker &checker, const Syntax::ValueDecl &declaration,
                                     std::string_view keyword);

/// @brief Reports an error about @p span, labelled @p label under it. The
///        result is the diagnostic as stored, to add help or related places
///        to before anything else is reported.
Diagnostic &Fail(Checker &checker, Span span, std::string message, std::string label = {});

/// @brief @p diagnostic with @p span in the same file labelled @p label too.
void Relate(Diagnostic &diagnostic, Span span, std::string label);

/// @brief Whether @p name is a reserved word, which can't name anything,
///        reporting it if so: a core word, or one of the vocabulary's.
bool IsReserved(Checker &checker, const Syntax::Named &name);

/// @brief Adds @p name to the namespace, or reports why it can't be: a reserved
///        word, or a name already taken. False when refused.
bool Declare(Checker &checker, const Syntax::Named &name, Symbol symbol);

/// @brief Every name in the namespace, for "did you mean".
[[nodiscard]] std::vector<std::string_view> SymbolNames(const Checker &checker);

/// @brief The type @p name names: a core type, an enum or a vocabulary type.
std::optional<Type> ResolveType(Checker &checker, const Syntax::Named &name);

/// @brief How @p type is written, for messages.
[[nodiscard]] std::string TypeName(const Checker &checker, Type type);

[[nodiscard]] Expr CheckExpression(Checker &checker, const Syntax::Expr &syntax, ExprMode mode);

/// @brief Why a value must have a type: a place to point at, and what to say there.
struct Reason
{
    Span span;
    std::string label;
};

/// @brief @p value as @p wanted: as it is, widened from int, or a string
///        accepted by a vocabulary type. An Error-typed expression, reported
///        with @p because pointed at when there is one, when it can't be.
[[nodiscard]] Expr Coerce(Checker &checker, Expr value, Type wanted, const std::optional<Reason> &because);

/// @brief Whether @p expr reads a trigger or a when-only function, directly or
///        through lets, so it may only decide transitions.
[[nodiscard]] bool ReadsWhenOnly(const Checker &checker, const Expr &expr);

/// @brief Adds to @p triggers each trigger param @p expr reads, through lets too.
void CollectTriggers(const Checker &checker, const Expr &expr, std::vector<uint32_t> &triggers);

/// @brief The value of @p expr, which reads only literals and core functions.
[[nodiscard]] std::expected<Constant, std::string> Evaluate(const Expr &expr, std::span<const std::string> functions);

/// @brief Brings a library's own enums and consts into the namespace.
void MergeLibrary(Checker &checker, const Program &library, const Syntax::Import &statement);

void CheckDeclarations(Checker &checker, const Syntax::File &file);
void CheckBlocks(Checker &checker, const Syntax::File &file);
void WarnUnused(Checker &checker);

} // namespace Assisi::Sigil::Compile::Detail
