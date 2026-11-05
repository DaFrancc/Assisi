/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ThemeTokens.hpp
/// @brief A value that names a theme's token, and whether a theme gives that
/// token a value the attribute using it can hold.
///
/// Internal to the import library: a theme's styles and a screen's elements
/// use tokens the same way, and are checked the same way.

#include <Assisi/Mondrian/Import/Markup.hpp>
#include <Assisi/Mondrian/Import/ThemeCompiler.hpp>

#include <Assisi/Mondrian/Style.hpp>

#include <expected>
#include <string_view>

namespace Assisi::Mondrian::Import
{

/// What marks a value as a token's rather than one written out: `$accent`.
inline constexpr char kTokenMark = '$';

/// Whether @p value names a token rather than giving a value outright. No
/// value a style property can hold starts with the mark, so it needs no escape.
[[nodiscard]] bool NamesToken(std::string_view value);

/// The token @p attribute's value names, refusing a name no token can have.
[[nodiscard]] std::expected<std::string_view, MarkupError> TokenNamed(const MarkupAttribute &attribute);

/// Refuses @p attribute's use of @p token unless @p theme gives it, in every
/// mode, a value @p property can hold. @p themeIs is how a message names the
/// theme: "this theme", or "the theme 'ui/Dark.amth'".
[[nodiscard]] std::expected<void, MarkupError> CheckToken(const CompiledTheme &theme, std::string_view themeIs,
                                                          std::string_view token, StyleProperty property,
                                                          const MarkupAttribute &attribute);

} // namespace Assisi::Mondrian::Import
