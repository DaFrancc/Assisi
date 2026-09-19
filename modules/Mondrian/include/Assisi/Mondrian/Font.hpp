/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Font.hpp
/// @brief A cooked font: one atlas of glyph images and the metrics that place them.
///
/// Glyphs are keyed by the font's own glyph index, never by codepoint. A shaper
/// maps text to glyphs, and one codepoint can become several glyphs or several
/// one; the cmap is what the trivial shaper looks up in the meantime. Every
/// metric is in pixels at the size the font was cooked at, so drawing at another
/// size scales by the ratio.
///
/// The writer and the reader live together so the layout has one definition. The
/// cook writes through WriteCookedFont and a load reads through ReadCookedFont,
/// with no rasteriser behind it.

#include <Assisi/Core/BitStream.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Assisi::Mondrian
{

/// @brief How the atlas encodes a glyph.
enum class FontKind : uint8_t
{
    /// A signed distance field: 128 on the outline, higher inside.
    Sdf,
    Count
};

/// @brief One glyph's image in the atlas and how it sits against the pen.
struct Glyph
{
    float advance    = 0.f; ///< pen movement after this glyph, in pixels
    uint32_t index   = 0;   ///< the font's glyph index
    uint16_t x       = 0;   ///< atlas rect, in texels; zero-sized for a glyph with no image
    uint16_t y       = 0;
    uint16_t width   = 0;
    uint16_t height  = 0;
    int16_t bearingX = 0; ///< from the pen to the image's left edge
    int16_t bearingY = 0; ///< from the baseline up to the image's top edge
};

/// @brief One entry of the character-to-glyph table.
struct CmapEntry
{
    uint32_t codepoint = 0;
    uint32_t glyph     = 0;
};

/// @brief An adjustment to the pen between two glyphs.
struct KerningPair
{
    float adjust   = 0.f; ///< pixels, added to the first glyph's advance
    uint32_t left  = 0;   ///< glyph index
    uint32_t right = 0;   ///< glyph index
};

/// @brief A cooked font.
struct Font
{
    std::vector<Glyph> glyphs;        ///< sorted by index
    std::vector<CmapEntry> cmap;      ///< sorted by codepoint
    std::vector<KerningPair> kerning; ///< sorted by (left, right)
    std::vector<uint8_t> atlas;       ///< one byte a texel, row by row
    float pixelSize         = 0.f;    ///< the size every metric here is measured at
    float ascender          = 0.f;    ///< baseline to the top of the tallest glyph
    float descender         = 0.f;    ///< baseline to the bottom of the lowest glyph; negative
    float lineHeight        = 0.f;    ///< baseline to baseline
    uint32_t atlasWidth     = 0;
    uint32_t atlasHeight    = 0;
    uint8_t spread          = 0; ///< the distance field's reach from the outline, in texels
    FontKind kind           = FontKind::Sdf;

    /// @brief The glyph with font index @p index, or null when the font has none.
    [[nodiscard]] const Glyph *FindGlyph(uint32_t index) const;

    /// @brief The glyph index the font maps @p codepoint to, when it has one.
    [[nodiscard]] std::optional<uint32_t> GlyphFor(uint32_t codepoint) const;

    /// @brief The pen adjustment between glyphs @p left and @p right, in that
    /// order; zero when the font has none for the pair.
    [[nodiscard]] float Kerning(uint32_t left, uint32_t right) const;
};

/// @brief Version of the font payload's layout, separate from the blob envelope's.
///
/// Part of the font cooker's cache key, so bumping it re-cooks every font.
inline constexpr uint8_t kFontPayloadVersion = 1;

/// @brief Why bytes did not read as a cooked font.
enum class CookedFontError : uint8_t
{
    NotAFont,           ///< Not a cooked blob, or a blob of another kind.
    UnsupportedVersion, ///< A font layout this build does not read.
    Truncated,          ///< The bytes end, or a length claims more than is left.
    Invalid,            ///< Framed correctly, but the contents contradict each other.
    Count
};

/// @brief A short human-readable description, for a load failure's log line.
[[nodiscard]] std::string_view ToString(CookedFontError error) noexcept;

/// @brief Write @p font as a complete cooked blob, envelope included.
void WriteCookedFont(Core::BitWriter &writer, const Font &font);

/// @brief Read a blob written by WriteCookedFont.
///
/// Every length is checked against the bytes left before anything is allocated.
/// An atlas that is not width times height bytes, a glyph whose rect leaves the
/// atlas, or a table out of order is Invalid.
[[nodiscard]] std::expected<Font, CookedFontError> ReadCookedFont(std::span<const std::byte> bytes);

} // namespace Assisi::Mondrian
