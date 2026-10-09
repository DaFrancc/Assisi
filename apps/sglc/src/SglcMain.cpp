/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file SglcMain.cpp
/// @brief sglc: compiles each `.sgl` file named on the command line and prints
///        what the compiler says, exiting 1 if any file has errors.
///
///   sglc [--root <dir>] [--color=auto|always|never] <file.sgl>...
///
/// Imports are read from paths under the root, the current directory unless
/// --root names another, the way the cook reads them from the asset root.

#include "Vocabularies.hpp"

#include <Assisi/Sigil/Compile/Compile.hpp>

#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace
{

using namespace Assisi::Sigil::Compile;

/// What the process returns.
enum class Exit : int32_t
{
    Clean = 0,  ///< Every file compiled.
    Errors = 1, ///< A file has errors.
    Usage = 2,  ///< The command line was wrong, or a file couldn't be read.
    Count_,
};

constexpr std::string_view kUsage = "usage: sglc [--root <dir>] [--color=auto|always|never] <file.sgl>...\n";

struct Options
{
    std::vector<std::string> files;
    std::filesystem::path root;
    Style style = Style::Plain;
};

bool IsTerminal(std::FILE *stream)
{
#if defined(_WIN32)
    return _isatty(_fileno(stream)) != 0;
#else
    return isatty(fileno(stream)) != 0;
#endif
}

/// Color when asked for, or when writing to a terminal and NO_COLOR isn't set.
std::optional<Style> StyleFor(std::string_view choice)
{
    if (choice == "always")
    {
        return Style::Color;
    }
    if (choice == "never")
    {
        return Style::Plain;
    }
    if (choice == "auto")
    {
        const bool wanted = IsTerminal(stdout) && std::getenv("NO_COLOR") == nullptr;
        return wanted ? Style::Color : Style::Plain;
    }
    return std::nullopt;
}

std::optional<Options> ReadOptions(std::span<char *const> arguments)
{
    Options options{.files = {}, .root = std::filesystem::path{"."}, .style = Style::Plain};
    std::string_view color = "auto";
    for (std::size_t i = 1; i < arguments.size(); ++i)
    {
        const std::string_view argument = arguments[i];
        if (argument == "--root" && i + 1 < arguments.size())
        {
            options.root = arguments[++i];
        }
        else if (argument.starts_with("--color="))
        {
            color = argument.substr(std::string_view{"--color="}.size());
        }
        else if (argument.starts_with("-"))
        {
            return std::nullopt;
        }
        else
        {
            options.files.emplace_back(argument);
        }
    }
    const std::optional<Style> style = StyleFor(color);
    if (options.files.empty() || !style.has_value())
    {
        return std::nullopt;
    }
    options.style = *style;
    return options;
}

std::expected<std::string, std::string> ReadText(const std::filesystem::path &path)
{
    std::ifstream file{path, std::ios::binary};
    if (!file)
    {
        return std::unexpected("can't open it");
    }
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

/// Prints @p diagnostics and a line saying how @p file went. False if it has errors.
bool Report(const std::string &file, const std::expected<Program, Diagnostics> &result, Style style)
{
    const Diagnostics &diagnostics = result ? result->warnings : result.error();
    std::size_t errors = 0;
    for (const Diagnostic &diagnostic : diagnostics)
    {
        errors += diagnostic.severity == Severity::Error ? 1 : 0;
        std::fputs((Format(diagnostic, style) + "\n").c_str(), stdout);
    }
    const std::size_t warnings = diagnostics.size() - errors;
    if (result)
    {
        std::printf("%s: compiles, %zu warning%s\n", file.c_str(), warnings, warnings == 1 ? "" : "s");
        return true;
    }
    std::printf("%s: %zu error%s, %zu warning%s\n", file.c_str(), errors, errors == 1 ? "" : "s", warnings,
                warnings == 1 ? "" : "s");
    return false;
}

} // namespace

int main(int argc, char **argv)
{
    const std::optional<Options> options = ReadOptions(std::span<char *const>{argv, static_cast<std::size_t>(argc)});
    if (!options.has_value())
    {
        std::fputs(kUsage.data(), stderr);
        return static_cast<int>(Exit::Usage);
    }
    const std::vector<Vocabulary> vocabularies = Assisi::Sglc::Vocabularies();
    const std::filesystem::path root = options->root;
    const SourceReader read = [&root](std::string_view path) { return ReadText(root / path); };

    Exit exit = Exit::Clean;
    for (const std::string &file : options->files)
    {
        const std::expected<std::string, std::string> source = ReadText(file);
        if (!source)
        {
            std::fprintf(stderr, "%s: %s\n", file.c_str(), source.error().c_str());
            return static_cast<int>(Exit::Usage);
        }
        const std::expected<Program, Diagnostics> result = CompileSource(*source, file, vocabularies, read);
        if (!Report(file, result, options->style))
        {
            exit = Exit::Errors;
        }
    }
    return static_cast<int>(exit);
}
