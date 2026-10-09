/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Sigil/Compile/Diagnostic.hpp>

#include <algorithm>
#include <format>

namespace Assisi::Sigil::Compile
{

std::string Format(const Diagnostic &diagnostic)
{
    const char *severity = diagnostic.severity == Severity::Warning ? "warning" : "error";
    return std::format("{}:{}:{}: {}: {}", diagnostic.file, diagnostic.where.line, diagnostic.where.column, severity,
                       diagnostic.message);
}

bool HasErrors(std::span<const Diagnostic> diagnostics)
{
    return std::ranges::any_of(diagnostics,
                               [](const Diagnostic &diagnostic) { return diagnostic.severity == Severity::Error; });
}

} // namespace Assisi::Sigil::Compile
