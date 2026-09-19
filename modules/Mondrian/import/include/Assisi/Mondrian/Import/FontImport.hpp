/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file FontImport.hpp
/// @brief Turns a font description and its font file into a cooked Font.
///
/// The only code that reads font files. The cook and the editor link it; a
/// shipped game does not, and reads the Font the cook wrote instead.

#include <Assisi/Mondrian/Font.hpp>
#include <Assisi/Mondrian/FontDescription.hpp>
#include <Assisi/Mondrian/FontReader.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace Assisi::Mondrian::Import
{

/// @brief Why a font could not be imported.
enum class FontImportError : uint8_t
{
    InvalidDocument, ///< Not a FontDescription document.
    BadRanges,       ///< Ranges are empty, odd in number, backwards or too many.
    BadSize,         ///< The pixel size is not positive, or too large to rasterise.
    BadSpread,       ///< The spread is outside what the rasteriser accepts.
    MissingSource,   ///< The description names no font file.
    UnreadableFont,  ///< The font file's bytes are not a font.
    RasterizeFailed, ///< The rasteriser refused a glyph.
    AtlasFull,       ///< The glyphs do not fit the largest atlas allowed.
    Count
};

/// @brief A short human-readable description, for a cook failure's log line.
[[nodiscard]] std::string_view ToString(FontImportError error) noexcept;

/// @brief The rasteriser's version as major, minor and patch in the low three
/// bytes, so a cook can re-rasterise when it changes.
[[nodiscard]] uint32_t RasterizerVersion();

/// @brief Reads a `.afont` document and checks it describes a font that can be
/// rasterised.
[[nodiscard]] std::expected<FontDescription, FontImportError> ParseFontDescription(std::string_view text);

/// @brief Rasterises every glyph @p description's ranges map to, plus the
/// font's missing-glyph glyph, into one distance-field atlas.
///
/// Deterministic: the same description and bytes give the same Font, so a
/// re-cook of an unchanged font changes nothing.
///
/// Kerning comes from the font's legacy `kern` table, the only one FreeType
/// reads. A font whose kerning is only in GPOS, as Inter's is, gets an empty
/// table until shaping reads GPOS itself.
[[nodiscard]] std::expected<Font, FontImportError> RasterizeFont(const FontDescription &description,
                                                                std::span<const std::byte> fontFile);

/// @brief The FontReader for the source tree: reads the description at @p vpath
/// and the font file it names through the asset system, and rasterises them.
[[nodiscard]] std::expected<Font, FontLoadError> ReadSourceFont(std::string_view vpath);

} // namespace Assisi::Mondrian::Import
