/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Sigil/Compile/Compile.hpp>

#include "Checker.hpp"

#include <Assisi/Sigil/Compile/Lexer.hpp>
#include <Assisi/Sigil/Compile/Parser.hpp>
#include <Assisi/Sigil/Compile/Suggest.hpp>

#include <algorithm>
#include <format>
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

Diagnostic Error(std::string_view file, SourceLocation where, std::string message)
{
    return Diagnostic{.message = std::move(message), .file = std::string{file}, .where = where};
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

/// Compiles the library @p import names and brings it into @p checker.
void ImportLibrary(Detail::Checker &checker, const Syntax::Import &import, ImportContext &context)
{
    if (std::ranges::find(context.chain, import.path) != context.chain.end())
    {
        Detail::Fail(checker, import.where, std::format("these files import each other: {}", Chain(context.chain, import.path)));
        return;
    }
    if (context.chain.size() >= kMaxImportDepth)
    {
        Detail::Fail(checker, import.where, std::format("imports chain more than {} files deep here", kMaxImportDepth));
        return;
    }
    const std::expected<std::string, std::string> text = context.read(import.path);
    if (!text)
    {
        Detail::Fail(checker, import.where, std::format("can't read \"{}\": {}", import.path, text.error()));
        return;
    }
    context.chain.push_back(import.path);
    std::expected<Program, Diagnostics> library = CompileFile(*text, import.path, context);
    context.chain.pop_back();
    if (!library)
    {
        checker.diagnostics.insert(checker.diagnostics.end(), library.error().begin(), library.error().end());
        Detail::Fail(checker, import.where, std::format("\"{}\" has errors", import.path));
        return;
    }
    if (library->vocabulary != checker.vocabulary.name)
    {
        Detail::Fail(checker, import.where,
                     std::format("\"{}\" is written for the {} vocabulary, not {}", import.path, library->vocabulary,
                                 checker.vocabulary.name));
        return;
    }
    if (!library->library)
    {
        Detail::Fail(checker, import.where,
                     std::format("\"{}\" isn't a library; only files marked \"sigiltype library;\" can be imported",
                                 import.path));
        return;
    }
    Detail::MergeLibrary(checker, *library, import);
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
        checker.symbols.emplace(checker.functions[i].name, Detail::Symbol{.where = {},
                                                                          .index = static_cast<uint32_t>(i),
                                                                          .import = -1,
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
        const std::array<std::string_view, 1> kinds{kLibrary};
        Detail::Fail(checker, file.sigilType->where,
                     std::format("unknown sigiltype \"{}\"{}", file.sigilType->name,
                                 DidYouMean(kinds, file.sigilType->name)));
        return;
    }
    checker.program.library = true;
}

std::expected<Program, Diagnostics> Check(const Syntax::File &file, std::string_view path, ImportContext &context)
{
    const Vocabulary *vocabulary = FindVocabulary(context.vocabularies, file.use.vocabulary.name);
    if (vocabulary == nullptr)
    {
        std::vector<std::string_view> names;
        for (const Vocabulary &candidate : context.vocabularies)
        {
            names.push_back(candidate.name);
        }
        return std::unexpected(Diagnostics{Error(path, file.use.vocabulary.where,
                                                 std::format("unknown vocabulary \"{}\"{}", file.use.vocabulary.name,
                                                             DidYouMean(names, file.use.vocabulary.name)))});
    }
    if (const std::expected<void, std::string> usable = CheckVocabulary(*vocabulary); !usable)
    {
        return std::unexpected(Diagnostics{Error(
            path, file.use.vocabulary.where,
            std::format("the {} vocabulary can't be used: {}", vocabulary->name, usable.error()))});
    }

    Detail::Checker checker{.vocabulary = *vocabulary, .file = std::string{path}};
    checker.program.vocabulary = vocabulary->name;
    ReadSigilType(checker, file);
    DeclareFunctions(checker);
    for (const Syntax::Declaration &declaration : file.declarations)
    {
        if (const Syntax::Import *import = std::get_if<Syntax::Import>(&declaration))
        {
            ImportLibrary(checker, *import, context);
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

std::expected<Program, Diagnostics> CompileFile(std::string_view source, std::string_view file,
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
    return Check(*syntax, file, context);
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
