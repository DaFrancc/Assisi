/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Text.hpp
/// @brief Text in three steps: shape, lay out, draw.
///
/// Shape turns UTF-8 into glyphs and is the only step that reads the string.
/// LayoutText places those glyphs in lines and is the measurement: what it
/// reports is what DrawGlyphs draws, because drawing only copies its positions.
/// Shaping is a codepoint-to-glyph lookup for now; complex scripts arrive by
/// replacing Shape alone, so nothing after it indexes text by byte or assumes
/// one glyph per codepoint.

#include <Assisi/Mondrian/DrawList.hpp>
#include <Assisi/Mondrian/Font.hpp>

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace Assisi::Mondrian
{

/// @brief What a glyph means to line breaking.
enum class GlyphClass : uint8_t
{
    Ink,        ///< drawn, and never a place to break
    Whitespace, ///< a break may follow it; not counted in a line's width at its end
    Newline,    ///< ends its line; has no advance and draws nothing
    Count
};

/// @brief One glyph as the shaper produced it.
struct ShapedGlyph
{
    float advance    = 0.f; ///< in the font's cooked pixels, kerning with the next glyph included
    uint32_t index   = 0;   ///< the font's glyph index
    uint32_t cluster = 0;   ///< byte offset of the text it came from; only mapping back to the text reads it
    GlyphClass kind  = GlyphClass::Ink;
};

/// @brief A string as glyphs, in the order they are laid out.
struct ShapedText
{
    std::vector<ShapedGlyph> glyphs;
};

/// @brief Where a line's slack goes.
enum class TextAlign : uint8_t
{
    Left,
    Center,
    Right,
    Count
};

/// @brief One glyph where layout put it, relative to the block's top-left.
struct PlacedGlyph
{
    float x          = 0.f; ///< the pen, in pixels
    float y          = 0.f; ///< the line's baseline, in pixels
    uint32_t index   = 0;
    uint32_t cluster = 0;
    GlyphClass kind  = GlyphClass::Ink;
};

/// @brief One line of a layout, as a range of its glyphs.
struct TextLine
{
    float width    = 0.f; ///< pen extent, not counting whitespace at its end
    float x        = 0.f; ///< left edge, in whole pixels
    float baseline = 0.f; ///< in whole pixels
    uint32_t first = 0;
    uint32_t count = 0;
};

/// @brief Shaped text in lines at one size: its measurement, and what DrawGlyphs draws.
///
/// Refers to the font it was laid out with, which must outlive it.
struct TextLayout
{
    std::vector<PlacedGlyph> glyphs; ///< every shaped glyph, whitespace included
    std::vector<TextLine> lines;     ///< at least one, even for empty text
    const Font *font = nullptr;
    float scale      = 0.f; ///< drawn size over the font's cooked size
    float width      = 0.f; ///< the widest line, rounded up to whole pixels
    float height     = 0.f; ///< every line's height, rounded up to whole pixels
};

/// @brief @p utf8 as @p font's glyphs.
///
/// A codepoint the font does not map, and bytes that are not UTF-8, become the
/// font's missing-glyph glyph rather than disappearing.
[[nodiscard]] ShapedText Shape(std::string_view utf8, const Font &font);

/// @brief Lays @p shaped out at @p size pixels, in lines no wider than
/// @p wrapWidth when one is given.
///
/// Lines break after spaces, and at a newline. A word wider than the wrap width
/// breaks where it overflows rather than spilling out. Line edges, baselines
/// and the block's size are whole pixels, so a box sized from the measurement
/// holds exactly what is drawn; the pen inside a line keeps its fractions.
/// Alignment is within @p wrapWidth, or within the widest line without one.
[[nodiscard]] TextLayout LayoutText(const ShapedText &shaped, const Font &font, float size,
                                    std::optional<float> wrapWidth, TextAlign align);

/// @brief Adds a glyph quad to @p list for each drawn glyph of @p layout, with
/// the block's top-left at @p origin.
///
/// The quads are not clipped to the measured box: a distance-field glyph's
/// image extends past its outline by the field's spread.
void DrawGlyphs(DrawList &list, const TextLayout &layout, TextureId atlas, Point origin,
                const Math::Color4<Math::ColorSpace::Srgb> &color);

} // namespace Assisi::Mondrian
