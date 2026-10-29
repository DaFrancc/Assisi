/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ThemeCompiler.hpp
/// @brief A theme file becoming a ThemeDocument, and the bytes a package
/// carries.
///
/// A theme is written in the screen markup:
///
///     <theme>
///       <palette mode="dark" accent="#e63319" surface="#1a1c24" />
///       <palette mode="light" accent="#c42a10" surface="#f4f4f4" />
///       <style name="panel" background="$surface" padding="32" />
///       <style name="button" extends="panel" hover:background="$accent" />
///     </theme>
///
/// A palette's attributes other than `mode` are its tokens, and every palette
/// gives every token a value. A style takes the attributes an element takes
/// about how it looks, state prefixes and tokens included, and nothing about
/// what a node is or does. Every value is parsed here, so the game reads none.
///
/// Cook-side only. The shipped game parses no source text.

#include <Assisi/Mondrian/Import/Markup.hpp>

#include <Assisi/Mondrian/Theme.hpp>

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Mondrian::Import
{

/// @brief What a theme file's path ends in.
inline constexpr std::string_view kThemeExtension = ".amth";

/// @brief A compiled theme, with what a message about one of its tokens needs.
struct CompiledTheme
{
    ThemeDocument document;
    /// Each token's value as its palettes write it, by token and then by mode,
    /// in the document's orders.
    std::vector<std::vector<std::string>> written;
};

/// @brief Compiles a parsed theme.
[[nodiscard]] std::expected<CompiledTheme, MarkupError> CompileTheme(const MarkupElement &root);

/// @brief Parses and compiles @p text.
[[nodiscard]] std::expected<CompiledTheme, MarkupError> CompileThemeSource(std::string_view text);

/// @brief Parses and compiles @p text, and writes the cooked blob.
[[nodiscard]] std::expected<std::vector<std::byte>, MarkupError> CompileThemeText(std::string_view text);

} // namespace Assisi::Mondrian::Import
