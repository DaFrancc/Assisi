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

/// Refuses what a library can't hold, with @p what naming it.
void RefuseInLibrary(Checker &checker, const Syntax::Named &name, std::string_view what)
{
    Diagnostic &error = Fail(checker, name.Extent(), std::format("a library can't declare {}", what),
                             "not allowed in a library");
    error.help = std::format("a library holds only imports, enums and consts; {}", kNotALibrary);
}

void DeclareEnum(Checker &checker, const Syntax::EnumDecl &declaration)
{
    Enum declared{.name = declaration.name.name, .values = {}, .origin = checker.file};
    std::vector<Span> spans;
    for (const Syntax::Named &value : declaration.values)
    {
        const std::vector<std::string>::iterator existing = std::ranges::find(declared.values, value.name);
        if (existing != declared.values.end())
        {
            Diagnostic &error = Fail(checker, value.Extent(),
                                     std::format("{} has two values called \"{}\"", declared.name, value.name),
                                     "declared again here");
            Relate(error, spans[static_cast<std::size_t>(existing - declared.values.begin())], "first declared here");
            continue;
        }
        declared.values.push_back(value.name);
        spans.push_back(value.Extent());
    }
    if (declared.values.empty())
    {
        Fail(checker, declaration.name.Extent(), std::format("enum {} has no values", declared.name),
             "an enum needs at least one value");
        return;
    }
    const Symbol symbol{.span = declaration.name.Extent(),
                        .index = static_cast<uint32_t>(checker.program.enums.size()),
                        .from = -1,
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
        RefuseInLibrary(checker, declaration.name, "params");
        return;
    }
    const std::optional<Type> type = ResolveType(checker, declaration.type);
    if (!type.has_value())
    {
        return;
    }
    const Symbol symbol{.span = declaration.name.Extent(),
                        .index = static_cast<uint32_t>(checker.program.params.size()),
                        .from = -1,
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
            Fail(checker, value.span, std::format("can't work out const \"{}\"", declaration.name.name),
                 std::move(evaluated.error()));
            value.type = Type{};
        }
        else
        {
            constant = std::move(*evaluated);
        }
    }
    const Symbol symbol{.span = declaration.name.Extent(),
                        .index = static_cast<uint32_t>(checker.program.consts.size()),
                        .from = -1,
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
        Diagnostic &error = Fail(checker, value.span, std::format("a let can't hold {}", TypeName(checker, value.type)),
                                 std::format("this is {}", TypeName(checker, value.type)));
        error.help = "only consts hold strings";
        error.suggestions.push_back(SwapKeyword(checker, declaration, "const"));
        value.type = Type{};
    }
    const Symbol symbol{.span = declaration.name.Extent(),
                        .index = static_cast<uint32_t>(checker.program.lets.size()),
                        .from = -1,
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
        RefuseInLibrary(checker, declaration.name, "lets");
        return;
    }
    const ExprMode mode = declaration.isConst ? ExprMode::Constant : ExprMode::Formula;
    checker.declaring = &declaration;
    Expr value = CheckExpression(checker, declaration.value, mode);
    checker.declaring = nullptr;
    if (declaration.type.has_value())
    {
        const std::optional<Type> wanted = ResolveType(checker, *declaration.type);
        const Reason because{.span = declaration.type->Extent(), .label = "expected because of this type"};
        value = Coerce(checker, std::move(value), wanted.value_or(Type{}), because);
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

std::string_view LineOf(const Checker &checker, uint32_t line)
{
    return line >= 1 && line <= checker.lines.size() ? checker.lines[line - 1] : std::string_view{};
}

Suggestion SwapKeyword(const Checker &checker, const Syntax::ValueDecl &declaration, std::string_view keyword)
{
    const std::string_view was = declaration.isConst ? "const" : "let";
    const Edit edit{.text = std::string{keyword},
                    .line = std::string{LineOf(checker, declaration.keyword.line)},
                    .span = Span{.where = declaration.keyword, .length = static_cast<uint32_t>(was.size())},
                    .kind = EditKind::Replace};
    return Suggestion{.message = std::format("make \"{}\" a {}", declaration.name.name, keyword), .edits = {edit}};
}

Diagnostic &Fail(Checker &checker, Span span, std::string message, std::string label)
{
    checker.diagnostics.push_back(Diagnostic{.message = std::move(message),
                                             .label = std::move(label),
                                             .file = checker.file,
                                             .where = span.where,
                                             .length = span.length,
                                             .severity = Severity::Error});
    return checker.diagnostics.back();
}

void Relate(Diagnostic &diagnostic, Span span, std::string label)
{
    diagnostic.related.push_back(Related{.label = std::move(label), .span = span});
}

bool IsReserved(Checker &checker, const Syntax::Named &name)
{
    if (Syntax::IsCoreWord(name.name))
    {
        Diagnostic &error = Fail(checker, name.Extent(), std::format("\"{}\" can't be a name", name.name),
                                 "a reserved word");
        error.help = "pick another name";
        return true;
    }
    if (IsVocabularyWord(checker.vocabulary, name.name))
    {
        Diagnostic &error = Fail(checker, name.Extent(), std::format("\"{}\" can't be a name", name.name),
                                 std::format("a word of the {} vocabulary", checker.vocabulary.name));
        error.help = "pick another name";
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
    if (existing == checker.symbols.end())
    {
        checker.symbols.emplace(name.name, symbol);
        return true;
    }
    const Symbol &first = existing->second;
    if (first.kind == SymbolKind::Function)
    {
        Diagnostic &error = Fail(checker, name.Extent(), std::format("\"{}\" can't be a name", name.name),
                                 std::format("{}() is a function", name.name));
        error.help = "pick another name";
        return false;
    }
    Diagnostic &error = Fail(checker, name.Extent(), std::format("\"{}\" is declared twice", name.name),
                             "declared again here");
    if (first.from >= 0)
    {
        Relate(error, first.span,
               std::format("imported from \"{}\" here", checker.imports[static_cast<std::size_t>(first.from)].path));
    }
    else
    {
        const std::string_view article = first.kind == SymbolKind::Enum ? "an" : "a";
        Relate(error, first.span, std::format("first declared here, as {} {}", article, KindName(first.kind)));
    }
    error.help = "two things in one file can't share a name; rename one of them";
    return false;
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
        if (found->second.from >= 0)
        {
            checker.imports[static_cast<std::size_t>(found->second.from)].used = true;
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
    const std::optional<std::string_view> closest = ClosestName(names, name.name);
    Fail(checker, name.Extent(), std::format("unknown type \"{}\"", name.name),
         closest.has_value() ? std::format("did you mean \"{}\"?", *closest) : std::string{"no such type"});
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

void MergeLibrary(Checker &checker, const Program &library, const Syntax::Import &statement)
{
    const int32_t importIndex = static_cast<int32_t>(checker.imports.size());
    checker.imports.push_back(ImportRecord{.path = statement.path, .span = statement.pathSpan, .used = false});
    std::vector<uint32_t> enumIndex;
    for (const Enum &enumeration : library.enums)
    {
        const uint32_t placed = PlaceEnum(checker, enumeration);
        enumIndex.push_back(placed);
        if (enumeration.origin == statement.path)
        {
            const Symbol symbol{
                .span = statement.pathSpan, .index = placed, .from = importIndex, .kind = SymbolKind::Enum, .used = false};
            (void)Declare(checker, Syntax::Named{.name = enumeration.name, .where = statement.pathSpan.where}, symbol);
        }
    }
    for (const Const &constant : library.consts)
    {
        if (constant.origin != statement.path)
        {
            continue;
        }
        Const merged = constant;
        if (merged.type.kind == TypeKind::Enum)
        {
            merged.type.index = enumIndex[merged.type.index];
        }
        const Symbol symbol{.span = statement.pathSpan,
                            .index = static_cast<uint32_t>(checker.program.consts.size()),
                            .from = importIndex,
                            .kind = SymbolKind::Const,
                            .used = false};
        if (Declare(checker, Syntax::Named{.name = constant.name, .where = statement.pathSpan.where}, symbol))
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
            checker.declaredLater.emplace(value->name.name, value->name.Extent());
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
        if (symbol.used || symbol.from >= 0 || symbol.kind == SymbolKind::Function)
        {
            continue;
        }
        warnings.push_back(Diagnostic{.message = std::format("unused {} \"{}\"", KindName(symbol.kind), entry.first),
                                      .label = "never used",
                                      .help = "remove it if nothing needs it",
                                      .file = checker.file,
                                      .where = symbol.span.where,
                                      .length = symbol.span.length,
                                      .severity = Severity::Warning});
    }
    for (const ImportRecord &record : checker.imports)
    {
        if (!record.used)
        {
            warnings.push_back(Diagnostic{.message = std::format("unused import \"{}\"", record.path),
                                          .label = "nothing from it is used",
                                          .help = "remove the import if nothing needs it",
                                          .file = checker.file,
                                          .where = record.span.where,
                                          .length = record.span.length,
                                          .severity = Severity::Warning});
        }
    }
    checker.diagnostics.insert(checker.diagnostics.end(), warnings.begin(), warnings.end());
}

} // namespace Assisi::Sigil::Compile::Detail
