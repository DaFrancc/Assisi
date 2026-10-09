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

/// @p text in bold @p color when styling.
std::string Paint(std::string_view text, std::string_view color, Style style)
{
    if (style == Style::Plain)
    {
        return std::string{text};
    }
    return std::string{color} + std::string{text} + "\x1b[0m";
}

/// "1 error", "2 warnings".
std::string Count(std::size_t count, std::string_view what)
{
    return std::to_string(count) + " " + std::string{what} + (count == 1 ? "" : "s");
}

/// Prints @p diagnostics and a line saying how @p file went. False if it has errors.
bool Report(const std::string &file, const std::expected<Program, Diagnostics> &result, Style style)
{
    constexpr std::string_view kRed = "\x1b[1;31m";
    constexpr std::string_view kYellow = "\x1b[1;33m";
    constexpr std::string_view kGreen = "\x1b[1;32m";
    const Diagnostics &diagnostics = result ? result->warnings : result.error();
    std::size_t errors = 0;
    for (const Diagnostic &diagnostic : diagnostics)
    {
        errors += diagnostic.severity == Severity::Error ? 1 : 0;
        std::fputs((Format(diagnostic, style) + "\n").c_str(), stdout);
    }
    const std::size_t warnings = diagnostics.size() - errors;
    if (!result)
    {
        const std::string also = warnings == 0 ? "" : "; " + Count(warnings, "warning") + " emitted";
        std::printf("%s could not compile \"%s\" due to %s%s\n", Paint("error:", kRed, style).c_str(), file.c_str(),
                    Count(errors, "error").c_str(), also.c_str());
        return false;
    }
    if (warnings > 0)
    {
        std::printf("%s \"%s\" compiled with %s\n", Paint("warning:", kYellow, style).c_str(), file.c_str(),
                    Count(warnings, "warning").c_str());
        return true;
    }
    std::printf("%s \"%s\"\n", Paint("Compiled", kGreen, style).c_str(), file.c_str());
    return true;
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
