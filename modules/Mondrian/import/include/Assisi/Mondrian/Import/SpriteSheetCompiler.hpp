/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SpriteSheetCompiler.hpp
/// @brief A sprite sheet file becoming a SpriteSheetDocument, and the bytes a
/// package carries.
///
/// A sheet is written in the screen markup:
///
///     <sheet texture="ui/frames.png">
///       <sprite name="panel" rect="0 0 48 48" borders="16" />
///       <sprite name="dot" rect="48 0 16 16" />
///     </sheet>
///
/// A sprite's `rect` is `x y width height` in the texture's pixels, the whole
/// texture when left out. Its `borders`, one number for every side or four for
/// left, top, right and bottom as padding is written, say how far its corners
/// reach; a sprite with any draws as a nine-slice.
///
/// Cook-side only. The shipped game parses no source text.

#include <Assisi/Mondrian/Import/Markup.hpp>
#include <Assisi/Mondrian/Import/SourceReader.hpp>
#include <Assisi/Mondrian/Sprite.hpp>

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Mondrian::Import
{

/// @brief What a sprite sheet file's path ends in.
inline constexpr std::string_view kSpriteSheetExtension = ".amsp";

/// @brief Compiles a parsed sheet, reading the size of its texture through
/// @p read.
[[nodiscard]] std::expected<SpriteSheetDocument, MarkupError> CompileSpriteSheet(const MarkupElement &root,
                                                                                 const SourceReader &read);

/// @brief Parses and compiles @p text.
[[nodiscard]] std::expected<SpriteSheetDocument, MarkupError> CompileSpriteSheetSource(std::string_view text,
                                                                                       const SourceReader &read);

/// @brief Parses and compiles @p text, and writes the cooked blob.
[[nodiscard]] std::expected<std::vector<std::byte>, MarkupError> CompileSpriteSheetText(std::string_view text,
                                                                                        const SourceReader &read);

/// @brief The texture the sheet written in @p text names, or empty when it
/// names none or does not parse: what a sheet is cooked again for when it
/// changes.
[[nodiscard]] std::string SpriteSheetTexture(std::string_view text);

/// @brief The size of the image whose file is @p bytes, or a message saying
/// why they are no image.
[[nodiscard]] std::expected<Extent, std::string> ImageSize(std::string_view bytes);

} // namespace Assisi::Mondrian::Import
