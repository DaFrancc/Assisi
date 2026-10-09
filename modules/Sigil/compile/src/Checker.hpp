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
    SourceLocation where{};
    uint32_t index = 0; ///< Into the Program list its kind names.
    int32_t import = -1; ///< Into Checker::imports when it came from one; -1 when declared here.
    SymbolKind kind = SymbolKind::Param;
    bool used = false;
};

struct ImportRecord
{
    std::string path;
    SourceLocation where;
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
    std::unordered_map<std::string, SourceLocation> declaredLater{};
    std::vector<ImportRecord> imports{};
    /// The core functions, then the vocabulary's; Program::functions in names.
    std::vector<FunctionSpec> functions{};
};

void Fail(Checker &checker, SourceLocation where, std::string message);
void Warn(Checker &checker, SourceLocation where, std::string message);

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

/// @brief @p value as @p wanted: as it is, widened from int, or a string
///        accepted by a vocabulary type. An Error-typed expression, reported,
///        when it can't be.
[[nodiscard]] Expr Coerce(Checker &checker, Expr value, Type wanted);

/// @brief Whether @p expr reads a trigger or a when-only function, directly or
///        through lets, so it may only decide transitions.
[[nodiscard]] bool ReadsWhenOnly(const Checker &checker, const Expr &expr);

/// @brief Adds to @p triggers each trigger param @p expr reads, through lets too.
void CollectTriggers(const Checker &checker, const Expr &expr, std::vector<uint32_t> &triggers);

/// @brief The value of @p expr, which reads only literals and core functions.
[[nodiscard]] std::expected<Constant, std::string> Evaluate(const Expr &expr, std::span<const std::string> functions);

/// @brief Brings a library's own enums and consts into the namespace.
void MergeLibrary(Checker &checker, const Program &library, const Syntax::Import &import);

void CheckDeclarations(Checker &checker, const Syntax::File &file);
void CheckBlocks(Checker &checker, const Syntax::File &file);
void WarnUnused(Checker &checker);

} // namespace Assisi::Sigil::Compile::Detail
