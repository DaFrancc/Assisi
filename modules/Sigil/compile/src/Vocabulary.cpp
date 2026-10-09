/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Sigil/Compile/Vocabulary.hpp>

#include <Assisi/Sigil/Compile/Syntax.hpp>

#include <algorithm>
#include <format>
#include <unordered_set>

namespace Assisi::Sigil::Compile
{

namespace
{

std::expected<void, std::string> Claim(std::unordered_set<std::string> &taken, const std::string &word)
{
    if (word.empty())
    {
        return std::unexpected("a word is empty");
    }
    if (Syntax::IsCoreWord(word))
    {
        return std::unexpected(std::format("\"{}\" is a core Sigil word", word));
    }
    if (!taken.insert(word).second)
    {
        return std::unexpected(std::format("\"{}\" is declared twice", word));
    }
    return {};
}

bool HasKind(const Vocabulary &vocabulary, const std::string &name)
{
    return std::ranges::any_of(vocabulary.blocks, [&name](const BlockKind &kind) { return kind.name == name; });
}

bool KnowsType(const Vocabulary &vocabulary, const std::string &name)
{
    if (name == TypeNames::kFloat || name == TypeNames::kInt || name == TypeNames::kBool ||
        name == TypeNames::kNumeric)
    {
        return true;
    }
    return std::ranges::any_of(vocabulary.types, [&name](const ValueType &type) { return type.name == name; });
}

std::expected<void, std::string> CheckClause(const Vocabulary &vocabulary, const ClauseSpec &clause)
{
    for (const std::string &block : clause.blocks)
    {
        if (!HasKind(vocabulary, block))
        {
            return std::unexpected(std::format("clause \"{}\" goes in \"{}\", which isn't a block kind", clause.word,
                                               block));
        }
    }
    for (const ArgumentSpec &argument : clause.arguments)
    {
        if (argument.kind == ArgumentKind::Value && !KnowsType(vocabulary, argument.type))
        {
            return std::unexpected(std::format("clause \"{}\" takes \"{}\", which isn't a type", clause.word,
                                               argument.type));
        }
    }
    return {};
}

/// A vocabulary function's value is written into the block by the engine, so
/// it can't depend on values the file passes it.
std::expected<void, std::string> CheckFunction(const FunctionSpec &function)
{
    if (!function.parameters.empty())
    {
        return std::unexpected(std::format("function \"{}\" takes values, but the engine supplies a vocabulary "
                                           "function's value, so it can take none",
                                           function.name));
    }
    if (function.result != TypeNames::kFloat && function.result != TypeNames::kInt &&
        function.result != TypeNames::kBool)
    {
        return std::unexpected(std::format("function \"{}\" gives \"{}\", but a vocabulary function gives a float, "
                                           "an int or a bool",
                                           function.name, function.result));
    }
    return {};
}

} // namespace

const std::vector<FunctionSpec> &CoreFunctions()
{
    static const std::vector<FunctionSpec> functions{
        FunctionSpec{.name = "abs", .parameters = {"numeric"}, .result = "numeric", .use = FunctionUse::Anywhere},
        FunctionSpec{.name = "min", .parameters = {"numeric", "numeric"}, .result = "numeric",
                     .use = FunctionUse::Anywhere},
        FunctionSpec{.name = "max", .parameters = {"numeric", "numeric"}, .result = "numeric",
                     .use = FunctionUse::Anywhere},
        FunctionSpec{.name = "clamp", .parameters = {"numeric", "numeric", "numeric"}, .result = "numeric",
                     .use = FunctionUse::Anywhere},
    };
    return functions;
}

std::expected<void, std::string> CheckVocabulary(const Vocabulary &vocabulary)
{
    std::unordered_set<std::string> taken;
    for (const FunctionSpec &function : CoreFunctions())
    {
        taken.insert(function.name);
    }
    for (const ValueType &type : vocabulary.types)
    {
        if (std::expected<void, std::string> claimed = Claim(taken, type.name); !claimed)
        {
            return claimed;
        }
    }
    for (const BlockKind &kind : vocabulary.blocks)
    {
        if (std::expected<void, std::string> claimed = Claim(taken, kind.name); !claimed)
        {
            return claimed;
        }
    }
    for (const BlockKind &kind : vocabulary.blocks)
    {
        for (const std::string &parent : kind.parents)
        {
            if (!HasKind(vocabulary, parent))
            {
                return std::unexpected(
                    std::format("block \"{}\" goes in \"{}\", which isn't a block kind", kind.name, parent));
            }
        }
    }
    for (const ClauseSpec &clause : vocabulary.clauses)
    {
        if (std::expected<void, std::string> claimed = Claim(taken, clause.word); !claimed)
        {
            return claimed;
        }
        if (std::expected<void, std::string> checked = CheckClause(vocabulary, clause); !checked)
        {
            return checked;
        }
    }
    for (const FunctionSpec &function : vocabulary.functions)
    {
        if (std::expected<void, std::string> claimed = Claim(taken, function.name); !claimed)
        {
            return claimed;
        }
        if (std::expected<void, std::string> checked = CheckFunction(function); !checked)
        {
            return checked;
        }
    }
    return {};
}

} // namespace Assisi::Sigil::Compile
