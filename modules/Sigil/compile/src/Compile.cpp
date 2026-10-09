/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Sigil/Compile/Compile.hpp>

#include "Checker.hpp"

#include <Assisi/Sigil/Compile/Lexer.hpp>
#include <Assisi/Sigil/Compile/Parser.hpp>
#include <Assisi/Sigil/Compile/Suggest.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <tuple>
#include <utility>
#include <variant>

namespace Assisi::Sigil::Compile
{

namespace
{

/// The value a file's `sigiltype` line may name.
constexpr std::string_view kLibrary = "library";

/// What a compile carries down into the libraries it imports.
struct ImportContext
{
    std::span<const Vocabulary> vocabularies;
    const SourceReader &read;
    /// The files being compiled, outermost first, which an import must not name again.
    std::vector<std::string> chain;
};

std::expected<Program, Diagnostics> CompileFile(std::string_view source, std::string_view file,
                                                ImportContext &context);

Diagnostic Error(std::string_view file, Span span, std::string message, std::string label)
{
    return Diagnostic{.message = std::move(message),
                      .label = std::move(label),
                      .file = std::string{file},
                      .where = span.where,
                      .length = span.length};
}

const Vocabulary *FindVocabulary(std::span<const Vocabulary> vocabularies, std::string_view name)
{
    const std::span<const Vocabulary>::iterator found =
        std::ranges::find_if(vocabularies, [name](const Vocabulary &vocabulary) { return vocabulary.name == name; });
    return found == vocabularies.end() ? nullptr : &*found;
}

std::string Chain(std::span<const std::string> files, std::string_view last)
{
    std::string chain;
    for (const std::string &file : files)
    {
        chain += std::format("\"{}\" -> ", file);
    }
    return chain + std::format("\"{}\"", last);
}

/// Compiles the library @p statement names and brings it into @p checker.
void ImportLibrary(Detail::Checker &checker, const Syntax::Import &statement, ImportContext &context)
{
    const Span path = statement.pathSpan;
    if (std::ranges::find(context.chain, statement.path) != context.chain.end())
    {
        Diagnostic &error = Detail::Fail(checker, path, "these files import each other", "imported here");
        error.help = std::format("{}; move what they share into a library neither imports",
                                 Chain(context.chain, statement.path));
        return;
    }
    if (context.chain.size() >= kMaxImportDepth)
    {
        Detail::Fail(checker, path, std::format("imports chain more than {} files deep", kMaxImportDepth),
                     "too deep here");
        return;
    }
    const std::expected<std::string, std::string> text = context.read(statement.path);
    if (!text)
    {
        Detail::Fail(checker, path, std::format("can't read \"{}\"", statement.path), text.error());
        return;
    }
    context.chain.push_back(statement.path);
    std::expected<Program, Diagnostics> library = CompileFile(*text, statement.path, context);
    context.chain.pop_back();
    if (!library)
    {
        checker.diagnostics.insert(checker.diagnostics.end(), library.error().begin(), library.error().end());
        Detail::Fail(checker, path, std::format("\"{}\" has errors", statement.path), "imported here");
        return;
    }
    if (library->vocabulary != checker.vocabulary.name)
    {
        Detail::Fail(checker, path, std::format("\"{}\" is for another vocabulary", statement.path),
                     std::format("written for the {} vocabulary, not {}", library->vocabulary,
                                 checker.vocabulary.name));
        return;
    }
    if (!library->library)
    {
        Diagnostic &error =
            Detail::Fail(checker, path, std::format("\"{}\" isn't a library", statement.path), "can't be imported");
        error.help = "only files marked \"sigiltype library;\" can be imported, and they hold only enums and consts";
        return;
    }
    Detail::MergeLibrary(checker, *library, statement);
}

/// The core functions and the vocabulary's, as names nothing else may take.
void DeclareFunctions(Detail::Checker &checker)
{
    checker.functions = CoreFunctions();
    checker.functions.insert(checker.functions.end(), checker.vocabulary.functions.begin(),
                             checker.vocabulary.functions.end());
    for (std::size_t i = 0; i < checker.functions.size(); ++i)
    {
        checker.program.functions.push_back(checker.functions[i].name);
        checker.symbols.emplace(checker.functions[i].name, Detail::Symbol{.span = {},
                                                                          .index = static_cast<uint32_t>(i),
                                                                          .from = -1,
                                                                          .kind = Detail::SymbolKind::Function,
                                                                          .used = true});
    }
}

void ReadSigilType(Detail::Checker &checker, const Syntax::File &file)
{
    if (!file.sigilType.has_value())
    {
        return;
    }
    if (file.sigilType->name != kLibrary)
    {
        Detail::Fail(checker, file.sigilType->Extent(), std::format("unknown sigiltype \"{}\"", file.sigilType->name),
                     std::format("did you mean \"{}\"?", kLibrary));
        return;
    }
    checker.program.library = true;
}

/// @p source a line per entry, without line endings.
std::vector<std::string_view> SplitLines(std::string_view source)
{
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= source.size())
    {
        const std::size_t end = std::min(source.find('\n', start), source.size());
        std::string_view line = source.substr(start, end - start);
        if (line.ends_with('\r'))
        {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        start = end + 1;
    }
    return lines;
}

std::expected<Program, Diagnostics> Check(const Syntax::File &file, std::string_view path, std::string_view source,
                                          ImportContext &context)
{
    const Vocabulary *vocabulary = FindVocabulary(context.vocabularies, file.use.vocabulary.name);
    if (vocabulary == nullptr)
    {
        std::vector<std::string_view> names;
        for (const Vocabulary &candidate : context.vocabularies)
        {
            names.push_back(candidate.name);
        }
        const std::optional<std::string_view> closest = ClosestName(names, file.use.vocabulary.name);
        return std::unexpected(Diagnostics{
            Error(path, file.use.vocabulary.Extent(), std::format("unknown vocabulary \"{}\"", file.use.vocabulary.name),
                  closest.has_value() ? std::format("did you mean \"{}\"?", *closest)
                                      : std::string{"this build has no vocabulary by that name"})});
    }
    if (const std::expected<void, std::string> usable = CheckVocabulary(*vocabulary); !usable)
    {
        return std::unexpected(
            Diagnostics{Error(path, file.use.vocabulary.Extent(),
                              std::format("the {} vocabulary can't be used", vocabulary->name), usable.error())});
    }

    Detail::Checker checker{.vocabulary = *vocabulary, .file = std::string{path}};
    checker.lines = SplitLines(source);
    checker.program.vocabulary = vocabulary->name;
    ReadSigilType(checker, file);
    DeclareFunctions(checker);
    for (const Syntax::Declaration &declaration : file.declarations)
    {
        if (const Syntax::Import *statement = std::get_if<Syntax::Import>(&declaration))
        {
            ImportLibrary(checker, *statement, context);
        }
    }
    Detail::CheckDeclarations(checker, file);
    Detail::CheckBlocks(checker, file);
    Detail::WarnUnused(checker);
    if (HasErrors(checker.diagnostics))
    {
        return std::unexpected(std::move(checker.diagnostics));
    }
    checker.program.warnings = std::move(checker.diagnostics);
    return std::move(checker.program);
}

std::expected<Program, Diagnostics> CompileText(std::string_view source, std::string_view file,
                                                ImportContext &context)
{
    const std::expected<std::vector<Token>, Diagnostics> tokens = Lex(source, file);
    if (!tokens)
    {
        return std::unexpected(tokens.error());
    }
    const std::expected<Syntax::File, Diagnostics> syntax = Parse(*tokens, file);
    if (!syntax)
    {
        return std::unexpected(syntax.error());
    }
    return Check(*syntax, file, source, context);
}

/// Gives each diagnostic about @p file the text of the line it points at.
void AttachExcerpts(Diagnostics &diagnostics, std::string_view source, std::string_view file)
{
    const std::vector<std::string_view> lines = SplitLines(source);
    for (Diagnostic &diagnostic : diagnostics)
    {
        if (diagnostic.file != file || !diagnostic.excerpt.empty())
        {
            continue;
        }
        if (diagnostic.where.line - 1 < lines.size())
        {
            diagnostic.excerpt = std::string{lines[diagnostic.where.line - 1]};
        }
        for (Related &related : diagnostic.related)
        {
            if (related.span.where.line - 1 < lines.size())
            {
                related.excerpt = std::string{lines[related.span.where.line - 1]};
            }
        }
    }
}

using Place = std::tuple<std::size_t, uint32_t, uint32_t>;

/// Where @p diagnostic sits for reading order: its file's place in @p files,
/// then its line and column.
Place PlaceOf(std::span<const std::string> files, const Diagnostic &diagnostic)
{
    const std::size_t file = static_cast<std::size_t>(std::ranges::find(files, diagnostic.file) - files.begin());
    return Place{file, diagnostic.where.line, diagnostic.where.column};
}

/// Puts @p diagnostics in the order a reader goes through the files: each
/// file's top to bottom, the files in the order they were first reported.
void SortByPlace(Diagnostics &diagnostics)
{
    std::vector<std::string> files;
    for (const Diagnostic &diagnostic : diagnostics)
    {
        if (std::ranges::find(files, diagnostic.file) == files.end())
        {
            files.push_back(diagnostic.file);
        }
    }
    std::ranges::stable_sort(diagnostics, [&files](const Diagnostic &a, const Diagnostic &b) {
        return PlaceOf(files, a) < PlaceOf(files, b);
    });
}

std::expected<Program, Diagnostics> CompileFile(std::string_view source, std::string_view file,
                                                ImportContext &context)
{
    std::expected<Program, Diagnostics> result = CompileText(source, file, context);
    Diagnostics &diagnostics = result ? result->warnings : result.error();
    AttachExcerpts(diagnostics, source, file);
    SortByPlace(diagnostics);
    return result;
}

} // namespace

std::optional<std::string> ReadUseLine(std::string_view source)
{
    const std::expected<std::vector<Token>, Diagnostics> tokens = Lex(source, {});
    if (!tokens || tokens->size() < 3)
    {
        return std::nullopt;
    }
    const std::vector<Token> &read = *tokens;
    const bool isUse = read[0].kind == TokenKind::Name && read[0].text == "use" && read[1].kind == TokenKind::Name &&
                       read[2].kind == TokenKind::Semicolon;
    if (!isUse)
    {
        return std::nullopt;
    }
    return read[1].text;
}

std::expected<Program, Diagnostics> CompileSource(std::string_view source, std::string_view file,
                                                  std::span<const Vocabulary> vocabularies, const SourceReader &read)
{
    ImportContext context{.vocabularies = vocabularies, .read = read, .chain = {std::string{file}}};
    return CompileFile(source, file, context);
}

} // namespace Assisi::Sigil::Compile
