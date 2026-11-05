/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "ThemeTokens.hpp"

#include "ScreenAttributes.hpp"
#include "ScreenWalk.hpp"

#include <string>

namespace Assisi::Mondrian::Import
{

bool NamesToken(std::string_view value)
{
    return value.starts_with(kTokenMark);
}

std::expected<std::string_view, MarkupError> TokenNamed(const MarkupAttribute &attribute)
{
    const std::string_view name = std::string_view{attribute.value}.substr(1);
    if (!IsParameterName(name))
    {
        return std::unexpected(At(attribute, "'" + attribute.value + "' names no token: after the " + kTokenMark +
                                                 " comes a token's name, which is letters, digits and _, not starting "
                                                 "with a digit."));
    }
    return name;
}

std::expected<void, MarkupError> CheckToken(const CompiledTheme &theme, std::string_view themeIs,
                                            std::string_view token, StyleProperty property,
                                            const MarkupAttribute &attribute)
{
    const ThemeDocument &document = theme.document;
    for (std::size_t index = 0; index < document.tokens.size(); ++index)
    {
        const ThemeToken &found = document.tokens[index];
        if (found.name.View() != token)
        {
            continue;
        }
        for (std::size_t mode = 0; mode < document.modes.size(); ++mode)
        {
            if (!found.values[mode].Writes(property))
            {
                return std::unexpected(At(attribute, std::string{themeIs} + " gives " + kTokenMark +
                                                         std::string{token} + " the value '" +
                                                         theme.written[index][mode] + "' in mode '" +
                                                         std::string{document.modes[mode].View()} + "', which '" +
                                                         std::string{AttributeFor(property).name} +
                                                         "' cannot hold."));
            }
        }
        return {};
    }
    return std::unexpected(At(attribute, std::string{themeIs} + " has no token '" + std::string{token} + "'."));
}

} // namespace Assisi::Mondrian::Import
