/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Sigil/Compile/Diagnostic.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <string_view>

namespace Assisi::Sigil::Compile
{

namespace
{

constexpr std::string_view kReset = "\x1b[0m";
constexpr std::string_view kBold = "\x1b[1m";
constexpr std::string_view kRed = "\x1b[1;31m";
constexpr std::string_view kYellow = "\x1b[1;33m";
constexpr std::string_view kCyan = "\x1b[1;36m";
constexpr std::string_view kBlue = "\x1b[1;34m";
constexpr std::string_view kGreen = "\x1b[1;32m";

/// @p text in @p color when styling, as it is when not.
std::string Paint(std::string_view text, std::string_view color, Style style)
{
    if (style == Style::Plain)
    {
        return std::string{text};
    }
    return std::format("{}{}{}", color, text, kReset);
}

/// The source line under a gutter holding its number, then a caret under
/// `where`. Tabs before the caret are kept, so it lines up however wide the
/// reader's tabs are.
std::string Excerpt(const Diagnostic &diagnostic, Style style)
{
    const std::string number = std::to_string(diagnostic.where.line);
    const std::string blank(number.size(), ' ');
    const std::size_t before = std::min<std::size_t>(diagnostic.where.column - 1, diagnostic.excerpt.size());
    std::string padding;
    for (std::size_t i = 0; i < before; ++i)
    {
        padding += diagnostic.excerpt[i] == '\t' ? '\t' : ' ';
    }
    const std::string bar = Paint("|", kBlue, style);
    return std::format(" {} {} {}\n {} {} {}{}\n", Paint(number, kBlue, style), bar, diagnostic.excerpt, blank, bar,
                       padding, Paint("^", kGreen, style));
}

} // namespace

std::string Format(const Diagnostic &diagnostic, Style style)
{
    const bool warning = diagnostic.severity == Severity::Warning;
    const std::string where =
        std::format("{}:{}:{}:", diagnostic.file, diagnostic.where.line, diagnostic.where.column);
    std::string text = std::format("{} {} {}\n", Paint(where, kBold, style),
                                   Paint(warning ? "warning:" : "error:", warning ? kYellow : kRed, style),
                                   Paint(diagnostic.message, kBold, style));
    if (!diagnostic.excerpt.empty())
    {
        text += Excerpt(diagnostic, style);
    }
    if (!diagnostic.help.empty())
    {
        text += std::format("  {} {}\n", Paint("help:", kCyan, style), diagnostic.help);
    }
    return text;
}

bool HasErrors(std::span<const Diagnostic> diagnostics)
{
    return std::ranges::any_of(diagnostics,
                               [](const Diagnostic &diagnostic) { return diagnostic.severity == Severity::Error; });
}

} // namespace Assisi::Sigil::Compile
