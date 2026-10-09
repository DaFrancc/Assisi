/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "Checker.hpp"

#include <Assisi/Sigil/Compile/Suggest.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <utility>
#include <variant>

namespace Assisi::Sigil::Compile::Detail
{

namespace
{

constexpr std::string_view kLibraryHolds = "a library can only hold imports, enums and consts";
constexpr std::string_view kNotALibrary = "remove \"sigiltype library;\" to make this an ordinary file";

bool IsVocabularyWord(const Vocabulary &vocabulary, std::string_view word)
{
    return std::ranges::any_of(vocabulary.types, [word](const ValueType &type) { return type.name == word; }) ||
           std::ranges::any_of(vocabulary.blocks, [word](const BlockKind &kind) { return kind.name == word; }) ||
           std::ranges::any_of(vocabulary.clauses, [word](const ClauseSpec &clause) { return clause.word == word; }) ||
           std::ranges::any_of(vocabulary.functions,
                               [word](const FunctionSpec &function) { return function.name == word; });
}

std::string_view KindName(SymbolKind kind)
{
    switch (kind)
    {
    case SymbolKind::Param:
        return "param";
    case SymbolKind::Const:
        return "const";
    case SymbolKind::Let:
        return "let";
    case SymbolKind::Enum:
        return "enum";
    default:
        return "function";
    }
}

/// Where an existing name came from, for "already ..." messages.
std::string Origin(const Checker &checker, const Symbol &symbol)
{
    if (symbol.kind == SymbolKind::Function)
    {
        return "the name of a function";
    }
    if (symbol.import >= 0)
    {
        return std::format("imported from \"{}\"", checker.imports[static_cast<std::size_t>(symbol.import)].path);
    }
    return std::format("declared on line {}", symbol.where.line);
}

void DeclareEnum(Checker &checker, const Syntax::EnumDecl &declaration)
{
    Enum declared{.name = declaration.name.name, .values = {}, .origin = checker.file};
    for (const Syntax::Named &value : declaration.values)
    {
        if (std::ranges::find(declared.values, value.name) != declared.values.end())
        {
            Fail(checker, value.where, std::format("{} already has a value \"{}\"", declared.name, value.name));
            continue;
        }
        declared.values.push_back(value.name);
    }
    if (declared.values.empty())
    {
        Fail(checker, declaration.name.where, std::format("enum {} has no values", declared.name));
        return;
    }
    const Symbol symbol{.where = declaration.name.where,
                        .index = static_cast<uint32_t>(checker.program.enums.size()),
                        .import = -1,
                        .kind = SymbolKind::Enum,
                        .used = false};
    if (Declare(checker, declaration.name, symbol))
    {
        checker.program.enums.push_back(std::move(declared));
    }
}

void DeclareParam(Checker &checker, const Syntax::ParamDecl &declaration)
{
    if (checker.program.library)
    {
        Fail(checker, declaration.name.where, std::format("{}, not params", kLibraryHolds), std::string{kNotALibrary});
        return;
    }
    const std::optional<Type> type = ResolveType(checker, declaration.type);
    if (!type.has_value())
    {
        return;
    }
    const Symbol symbol{.where = declaration.name.where,
                        .index = static_cast<uint32_t>(checker.program.params.size()),
                        .import = -1,
                        .kind = SymbolKind::Param,
                        .used = false};
    if (Declare(checker, declaration.name, symbol))
    {
        checker.program.params.push_back(
            Param{.name = declaration.name.name, .where = declaration.name.where, .type = *type});
    }
}

/// Whether @p type may be the type of a let: anything but strings and the
/// vocabulary's own types, which only consts and params hold.
bool LetCanHold(Type type)
{
    return type.kind != TypeKind::String && type.kind != TypeKind::Vocabulary;
}

void DeclareConst(Checker &checker, const Syntax::ValueDecl &declaration, Expr value)
{
    Constant constant{int32_t{0}};
    if (value.type.kind != TypeKind::Error)
    {
        std::expected<Constant, std::string> evaluated = Evaluate(value, checker.program.functions);
        if (!evaluated)
        {
            Fail(checker, value.where, std::move(evaluated.error()));
            value.type = Type{};
        }
        else
        {
            constant = std::move(*evaluated);
        }
    }
    const Symbol symbol{.where = declaration.name.where,
                        .index = static_cast<uint32_t>(checker.program.consts.size()),
                        .import = -1,
                        .kind = SymbolKind::Const,
                        .used = false};
    if (Declare(checker, declaration.name, symbol))
    {
        checker.program.consts.push_back(Const{.name = declaration.name.name,
                                               .value = std::move(constant),
                                               .origin = checker.file,
                                               .type = value.type});
    }
}

void DeclareLet(Checker &checker, const Syntax::ValueDecl &declaration, Expr value)
{
    if (!LetCanHold(value.type))
    {
        Fail(checker, declaration.value.where, std::format("a let can't hold {}", TypeName(checker, value.type)),
             std::format("only consts hold strings; make \"{}\" a const", declaration.name.name));
        value.type = Type{};
    }
    const Symbol symbol{.where = declaration.name.where,
                        .index = static_cast<uint32_t>(checker.program.lets.size()),
                        .import = -1,
                        .kind = SymbolKind::Let,
                        .used = false};
    if (Declare(checker, declaration.name, symbol))
    {
        const bool whenOnly = ReadsWhenOnly(checker, value);
        const Type type = value.type;
        checker.program.lets.push_back(
            Let{.name = declaration.name.name, .value = std::move(value), .type = type, .whenOnly = whenOnly});
    }
}

void DeclareValue(Checker &checker, const Syntax::ValueDecl &declaration)
{
    checker.declaredLater.erase(declaration.name.name);
    if (checker.program.library && !declaration.isConst)
    {
        Fail(checker, declaration.name.where, std::format("{}, not lets", kLibraryHolds), std::string{kNotALibrary});
        return;
    }
    const ExprMode mode = declaration.isConst ? ExprMode::Constant : ExprMode::Formula;
    Expr value = CheckExpression(checker, declaration.value, mode);
    if (declaration.type.has_value())
    {
        const std::optional<Type> wanted = ResolveType(checker, *declaration.type);
        value = Coerce(checker, std::move(value), wanted.value_or(Type{}));
    }
    if (declaration.isConst)
    {
        DeclareConst(checker, declaration, std::move(value));
    }
    else
    {
        DeclareLet(checker, declaration, std::move(value));
    }
}

/// Where an imported enum sits in this program, adding it if this is the first
/// import that brings it.
uint32_t PlaceEnum(Checker &checker, const Enum &enumeration)
{
    for (std::size_t i = 0; i < checker.program.enums.size(); ++i)
    {
        const Enum &existing = checker.program.enums[i];
        if (existing.name == enumeration.name && existing.origin == enumeration.origin)
        {
            return static_cast<uint32_t>(i);
        }
    }
    checker.program.enums.push_back(enumeration);
    return static_cast<uint32_t>(checker.program.enums.size() - 1);
}

} // namespace

void Fail(Checker &checker, SourceLocation where, std::string message, std::string help)
{
    checker.diagnostics.push_back(Diagnostic{.message = std::move(message),
                                             .help = std::move(help),
                                             .file = checker.file,
                                             .where = where,
                                             .severity = Severity::Error});
}

void Warn(Checker &checker, SourceLocation where, std::string message)
{
    checker.diagnostics.push_back(
        Diagnostic{.message = std::move(message), .file = checker.file, .where = where, .severity = Severity::Warning});
}

bool IsReserved(Checker &checker, const Syntax::Named &name)
{
    if (Syntax::IsCoreWord(name.name))
    {
        Fail(checker, name.where, std::format("\"{}\" is a reserved word, so it can't be a name", name.name),
             "pick another name");
        return true;
    }
    if (IsVocabularyWord(checker.vocabulary, name.name))
    {
        Fail(checker, name.where,
             std::format("\"{}\" is a word of the {} vocabulary, so it can't be a name", name.name,
                         checker.vocabulary.name),
             "pick another name");
        return true;
    }
    return false;
}

bool Declare(Checker &checker, const Syntax::Named &name, Symbol symbol)
{
    if (IsReserved(checker, name))
    {
        checker.refused.insert(name.name);
        return false;
    }
    const std::unordered_map<std::string, Symbol>::const_iterator existing = checker.symbols.find(name.name);
    if (existing != checker.symbols.end())
    {
        Fail(checker, name.where, std::format("\"{}\" is already {}", name.name, Origin(checker, existing->second)),
             "two things in one file can't share a name; rename one of them");
        return false;
    }
    checker.symbols.emplace(name.name, symbol);
    return true;
}

std::vector<std::string_view> SymbolNames(const Checker &checker)
{
    std::vector<std::string_view> names;
    for (const std::pair<const std::string, Symbol> &entry : checker.symbols)
    {
        names.push_back(entry.first);
    }
    // The table's order isn't stable, and the first of two equally close names wins.
    std::ranges::sort(names);
    return names;
}

std::optional<Type> ResolveType(Checker &checker, const Syntax::Named &name)
{
    constexpr std::array<std::pair<std::string_view, TypeKind>, 4> kCoreTypes{
        std::pair{std::string_view{"float"}, TypeKind::Float}, std::pair{std::string_view{"int"}, TypeKind::Int},
        std::pair{std::string_view{"bool"}, TypeKind::Bool}, std::pair{std::string_view{"trigger"}, TypeKind::Trigger}};
    std::vector<std::string_view> names;
    for (const std::pair<std::string_view, TypeKind> &core : kCoreTypes)
    {
        if (core.first == name.name)
        {
            return Type{.index = 0, .kind = core.second};
        }
        names.push_back(core.first);
    }
    for (std::size_t i = 0; i < checker.vocabulary.types.size(); ++i)
    {
        if (checker.vocabulary.types[i].name == name.name)
        {
            return Type{.index = static_cast<uint32_t>(i), .kind = TypeKind::Vocabulary};
        }
        names.push_back(checker.vocabulary.types[i].name);
    }
    const std::unordered_map<std::string, Symbol>::iterator found = checker.symbols.find(name.name);
    if (found != checker.symbols.end() && found->second.kind == SymbolKind::Enum)
    {
        found->second.used = true;
        if (found->second.import >= 0)
        {
            checker.imports[static_cast<std::size_t>(found->second.import)].used = true;
        }
        return Type{.index = found->second.index, .kind = TypeKind::Enum};
    }
    for (const std::pair<const std::string, Symbol> &entry : checker.symbols)
    {
        if (entry.second.kind == SymbolKind::Enum)
        {
            names.push_back(entry.first);
        }
    }
    std::ranges::sort(names);
    Fail(checker, name.where, std::format("unknown type \"{}\"{}", name.name, DidYouMean(names, name.name)));
    return std::nullopt;
}

std::string TypeName(const Checker &checker, Type type)
{
    switch (type.kind)
    {
    case TypeKind::Float:
        return "a float";
    case TypeKind::Int:
        return "an int";
    case TypeKind::Bool:
        return "a bool";
    case TypeKind::Trigger:
        return "a trigger";
    case TypeKind::String:
        return "a string";
    case TypeKind::Enum:
        return std::format("a {}", checker.program.enums[type.index].name);
    case TypeKind::Vocabulary:
        return std::format("a {}", checker.vocabulary.types[type.index].name);
    default:
        return "an error";
    }
}

void MergeLibrary(Checker &checker, const Program &library, const Syntax::Import &import)
{
    const int32_t importIndex = static_cast<int32_t>(checker.imports.size());
    checker.imports.push_back(ImportRecord{.path = import.path, .where = import.where, .used = false});
    std::vector<uint32_t> enumIndex;
    for (const Enum &enumeration : library.enums)
    {
        const uint32_t placed = PlaceEnum(checker, enumeration);
        enumIndex.push_back(placed);
        if (enumeration.origin == import.path)
        {
            const Symbol symbol{
                .where = import.where, .index = placed, .import = importIndex, .kind = SymbolKind::Enum, .used = false};
            (void)Declare(checker, Syntax::Named{.name = enumeration.name, .where = import.where}, symbol);
        }
    }
    for (const Const &constant : library.consts)
    {
        if (constant.origin != import.path)
        {
            continue;
        }
        Const merged = constant;
        if (merged.type.kind == TypeKind::Enum)
        {
            merged.type.index = enumIndex[merged.type.index];
        }
        const Symbol symbol{.where = import.where,
                            .index = static_cast<uint32_t>(checker.program.consts.size()),
                            .import = importIndex,
                            .kind = SymbolKind::Const,
                            .used = false};
        if (Declare(checker, Syntax::Named{.name = constant.name, .where = import.where}, symbol))
        {
            checker.program.consts.push_back(std::move(merged));
        }
    }
}

void CheckDeclarations(Checker &checker, const Syntax::File &file)
{
    // Enums and params can be used anywhere in the file, so they go into the
    // namespace before any value that might use them.
    for (const Syntax::Declaration &declaration : file.declarations)
    {
        if (const Syntax::EnumDecl *enumeration = std::get_if<Syntax::EnumDecl>(&declaration))
        {
            DeclareEnum(checker, *enumeration);
        }
        else if (const Syntax::ValueDecl *value = std::get_if<Syntax::ValueDecl>(&declaration))
        {
            checker.declaredLater.emplace(value->name.name, value->name.where);
        }
    }
    for (const Syntax::Declaration &declaration : file.declarations)
    {
        if (const Syntax::ParamDecl *param = std::get_if<Syntax::ParamDecl>(&declaration))
        {
            DeclareParam(checker, *param);
        }
    }
    for (const Syntax::Declaration &declaration : file.declarations)
    {
        if (const Syntax::ValueDecl *value = std::get_if<Syntax::ValueDecl>(&declaration))
        {
            DeclareValue(checker, *value);
        }
    }
}

void WarnUnused(Checker &checker)
{
    // A file with errors has parts that were never checked, so what they use
    // was never counted and the warnings would be wrong.
    if (checker.program.library || HasErrors(checker.diagnostics))
    {
        return;
    }
    Diagnostics warnings;
    for (const std::pair<const std::string, Symbol> &entry : checker.symbols)
    {
        const Symbol &symbol = entry.second;
        if (symbol.used || symbol.import >= 0 || symbol.kind == SymbolKind::Function)
        {
            continue;
        }
        warnings.push_back(Diagnostic{.message = std::format("{} \"{}\" is never used", KindName(symbol.kind), entry.first),
                                      .file = checker.file,
                                      .where = symbol.where,
                                      .severity = Severity::Warning});
    }
    for (const ImportRecord &import : checker.imports)
    {
        if (!import.used)
        {
            warnings.push_back(Diagnostic{.message = std::format("nothing from \"{}\" is used", import.path),
                                          .file = checker.file,
                                          .where = import.where,
                                          .severity = Severity::Warning});
        }
    }
    std::ranges::sort(warnings, [](const Diagnostic &a, const Diagnostic &b) {
        return a.where.line != b.where.line ? a.where.line < b.where.line : a.where.column < b.where.column;
    });
    checker.diagnostics.insert(checker.diagnostics.end(), warnings.begin(), warnings.end());
}

} // namespace Assisi::Sigil::Compile::Detail
