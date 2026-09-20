/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Text.hpp>

#include <Assisi/Mondrian/Utf8.hpp>

#include <Assisi/Core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace Assisi::Mondrian
{
namespace
{

/// Every font's glyph 0 is the one drawn for a character it lacks.
constexpr uint32_t kMissingGlyph = 0;

/// How far a line reaches above its baseline, in drawn pixels.
float Ascent(const TextLayout &layout)
{
    return layout.font != nullptr ? layout.font->ascender * layout.scale : 0.f;
}

/// How tall one line's box is, top to bottom.
float LineHeight(const TextLayout &layout)
{
    return layout.font != nullptr ? (layout.font->ascender - layout.font->descender) * layout.scale : 0.f;
}

/// The byte of @p shown just past everything on @p line: where the next line
/// starts, or the end of the text for the last one.
uint32_t AfterLine(const TextLayout &layout, std::string_view shown, uint32_t line)
{
    const uint32_t next = line + 1;
    if (next < layout.lines.size() && layout.lines[next].count > 0)
    {
        return layout.glyphs[layout.lines[next].first].cluster;
    }
    return static_cast<uint32_t>(shown.size());
}

constexpr uint32_t kSpace = 0x20;
constexpr uint32_t kNewline = 0x0A;

/// The share of a line's slack that goes before it, by TextAlign.
constexpr std::array<float, static_cast<std::size_t>(TextAlign::Count)> kSlackBefore{0.f, 0.5f, 1.f};

GlyphClass Classify(uint32_t codepoint)
{
    if (codepoint == kSpace)
    {
        return GlyphClass::Whitespace;
    }
    if (codepoint == kNewline)
    {
        return GlyphClass::Newline;
    }
    return GlyphClass::Ink;
}

/// Fills @p layout's glyphs with pen positions from the line's start and its
/// lines with the glyphs they hold and their widths, breaking greedily: a line
/// takes glyphs until the next ink would pass the wrap width, then ends after
/// its last whitespace, or before that ink when it has none.
void BreakLines(const ShapedText &shaped, float scale, std::optional<float> wrapWidth, TextLayout &layout)
{
    const std::vector<ShapedGlyph> &glyphs = shaped.glyphs;
    const auto total = static_cast<uint32_t>(glyphs.size());
    layout.glyphs.resize(total);

    uint32_t first = 0; // the line's first glyph
    float pen = 0.f;    // where the next glyph goes
    float inkEnd = 0.f; // the pen after the line's last ink
    // The line's last break opportunity: the glyph after its latest whitespace
    // run, and the line's width before that run. Past first when there is one.
    uint32_t breakBefore = 0;
    float widthAtBreak = 0.f;

    const auto endLine = [&](uint32_t end, float width)
    {
        layout.lines.push_back(TextLine{.width = width, .first = first, .count = end - first});
        first = end;
        pen = 0.f;
        inkEnd = 0.f;
        breakBefore = 0;
    };

    uint32_t i = 0;
    while (i < total)
    {
        const ShapedGlyph &glyph = glyphs[i];
        const float advance = glyph.advance * scale;

        if (glyph.kind == GlyphClass::Ink && wrapWidth && pen + advance > *wrapWidth && i > first)
        {
            if (breakBefore > first)
            {
                const uint32_t restart = breakBefore;
                endLine(restart, widthAtBreak);
                i = restart;
                continue;
            }
            // No whitespace on the line, so every glyph on it is ink. Break
            // between clusters only: splitting one would separate glyphs a
            // shaper made from a single character.
            uint32_t split = i;
            while (split > first && glyphs[split].cluster == glyphs[split - 1].cluster)
            {
                --split;
            }
            if (split > first)
            {
                endLine(split, split == i ? inkEnd : layout.glyphs[split].x);
                i = split;
                continue;
            }
        }

        layout.glyphs[i] = PlacedGlyph{.x = pen, .index = glyph.index, .cluster = glyph.cluster, .kind = glyph.kind};
        ++i;
        switch (glyph.kind)
        {
        case GlyphClass::Ink:
            pen += advance;
            inkEnd = pen;
            break;
        case GlyphClass::Whitespace:
            pen += advance;
            breakBefore = i;
            widthAtBreak = inkEnd;
            break;
        case GlyphClass::Newline:
        case GlyphClass::Count:
            endLine(i, inkEnd);
            break;
        }
    }
    endLine(total, inkEnd);
}

} // namespace

ShapedText Shape(std::string_view utf8, const Font &font)
{
    ASSISI_ASSERT(utf8.size() <= std::numeric_limits<uint32_t>::max(), "Shape given text too long to index");

    ShapedText shaped;
    shaped.glyphs.reserve(utf8.size());
    uint32_t offset = 0;
    while (offset < utf8.size())
    {
        ShapedGlyph glyph{.cluster = offset};
        const uint32_t codepoint = DecodeUtf8(utf8, offset);
        glyph.kind = Classify(codepoint);
        if (glyph.kind != GlyphClass::Newline)
        {
            glyph.index = font.GlyphFor(codepoint).value_or(kMissingGlyph);
            const Glyph *metrics = font.FindGlyph(glyph.index);
            glyph.advance = metrics != nullptr ? metrics->advance : 0.f;
            if (!shaped.glyphs.empty() && shaped.glyphs.back().kind != GlyphClass::Newline)
            {
                ShapedGlyph &previous = shaped.glyphs.back();
                previous.advance += font.Kerning(previous.index, glyph.index);
            }
        }
        shaped.glyphs.push_back(glyph);
    }
    return shaped;
}

TextLayout LayoutText(const ShapedText &shaped, const Font &font, float size, std::optional<float> wrapWidth,
                      TextAlign align)
{
    ASSISI_ASSERT(size > 0.f && font.pixelSize > 0.f, "LayoutText needs a positive size and a sized font");
    ASSISI_ASSERT(!wrapWidth || (std::isfinite(*wrapWidth) && *wrapWidth > 0.f),
                  "LayoutText's wrap width must be finite and positive; no wrap is nullopt");
    ASSISI_ASSERT(align < TextAlign::Count, "LayoutText given an alignment that does not exist");

    TextLayout layout;
    layout.font = &font;
    layout.scale = size / font.pixelSize;
    BreakLines(shaped, layout.scale, wrapWidth, layout);

    float widest = 0.f;
    for (const TextLine &line : layout.lines)
    {
        widest = std::max(widest, line.width);
    }
    layout.width = std::ceil(widest);
    layout.height = BlockHeight(layout, static_cast<uint32_t>(layout.lines.size()));

    // Rounding down keeps a right-aligned line inside the box; each baseline is
    // rounded from its exact position so line spacing does not drift.
    const float box = wrapWidth.value_or(layout.width);
    const float slackBefore = kSlackBefore[static_cast<std::size_t>(align)];
    for (std::size_t n = 0; n < layout.lines.size(); ++n)
    {
        TextLine &line = layout.lines[n];
        line.x = std::max(0.f, std::floor((box - line.width) * slackBefore));
        line.baseline = std::round((font.ascender + static_cast<float>(n) * font.lineHeight) * layout.scale);
        for (uint32_t g = line.first; g < line.first + line.count; ++g)
        {
            layout.glyphs[g].x += line.x;
            layout.glyphs[g].y = line.baseline;
        }
    }
    return layout;
}

void DrawGlyphs(DrawList &list, const TextLayout &layout, TextureId atlas, Point origin,
                const Math::Color4<Math::ColorSpace::Srgb> &color)
{
    if (layout.glyphs.empty())
    {
        return;
    }
    ASSISI_ASSERT(layout.font != nullptr, "DrawGlyphs given a layout with glyphs and no font");

    const Font &font = *layout.font;
    const float scale = layout.scale;
    const float atlasWidth = static_cast<float>(font.atlasWidth);
    const float atlasHeight = static_cast<float>(font.atlasHeight);
    for (const PlacedGlyph &placed : layout.glyphs)
    {
        if (placed.kind != GlyphClass::Ink)
        {
            continue;
        }
        const Glyph *glyph = font.FindGlyph(placed.index);
        if (glyph == nullptr || glyph->width == 0 || glyph->height == 0)
        {
            continue;
        }
        const Rect rect{.x = origin.x + placed.x + static_cast<float>(glyph->bearingX) * scale,
                        .y = origin.y + placed.y - static_cast<float>(glyph->bearingY) * scale,
                        .width = static_cast<float>(glyph->width) * scale,
                        .height = static_cast<float>(glyph->height) * scale};
        const Rect uv{.x = static_cast<float>(glyph->x) / atlasWidth,
                      .y = static_cast<float>(glyph->y) / atlasHeight,
                      .width = static_cast<float>(glyph->width) / atlasWidth,
                      .height = static_cast<float>(glyph->height) / atlasHeight};
        list.Quad(rect).Fill(color).Texture(atlas, uv).Kind(QuadKind::Glyph);
    }
}

float BlockHeight(const TextLayout &layout, uint32_t lines)
{
    if (layout.font == nullptr)
    {
        return 0.f;
    }
    return std::ceil(static_cast<float>(lines) * layout.font->lineHeight * layout.scale);
}

CaretPlace PlaceCaret(const TextLayout &layout, std::string_view shown, uint32_t character)
{
    const uint32_t offset = CharacterOffset(shown, character);

    // The caret goes on the left edge of the glyph that starts at this byte.
    // Whitespace at a line's end is laid out but has no width there, so a caret
    // among it belongs to the following line, which is where typing continues.
    for (uint32_t line = 0; line < layout.lines.size(); ++line)
    {
        const TextLine &row = layout.lines[line];
        for (uint32_t index = row.first; index < row.first + row.count; ++index)
        {
            const PlacedGlyph &glyph = layout.glyphs[index];
            if (glyph.cluster >= offset)
            {
                return {.x = glyph.x, .line = line};
            }
        }
    }

    // Past every glyph: the end of the last line, which is where the next
    // character will go.
    if (layout.lines.empty())
    {
        return {};
    }
    const TextLine &last = layout.lines.back();
    return {.x = last.x + last.width, .line = static_cast<uint32_t>(layout.lines.size() - 1)};
}

uint32_t CharacterAt(const TextLayout &layout, std::string_view shown, Point local)
{
    if (layout.lines.empty())
    {
        return 0;
    }

    // The last line whose box has begun by this point: a click above the text
    // lands on its first line, and one below it on its last.
    uint32_t line = 0;
    for (uint32_t index = 1; index < layout.lines.size(); ++index)
    {
        if (local.y >= layout.lines[index].baseline - Ascent(layout))
        {
            line = index;
        }
    }

    const TextLine &row = layout.lines[line];
    uint32_t nearest = 0;
    float best = std::numeric_limits<float>::max();
    const auto consider = [&](float x, uint32_t cluster)
    {
        const float distance = std::abs(local.x - x);
        if (distance < best)
        {
            best = distance;
            nearest = cluster;
        }
    };

    for (uint32_t index = row.first; index < row.first + row.count; ++index)
    {
        const PlacedGlyph &glyph = layout.glyphs[index];
        if (glyph.kind != GlyphClass::Newline)
        {
            consider(glyph.x, glyph.cluster);
        }
    }
    // The far end of the line, so a click past the last word lands after it
    // rather than before it.
    consider(row.x + row.width, AfterLine(layout, shown, line));
    return CharacterIndex(shown, nearest);
}

Rect LineBox(const TextLayout &layout, uint32_t line)
{
    if (line >= layout.lines.size())
    {
        return {};
    }
    const TextLine &row = layout.lines[line];
    return {.x = row.x, .y = row.baseline - Ascent(layout), .width = row.width, .height = LineHeight(layout)};
}

Rect LineSelection(const TextLayout &layout, std::string_view shown, uint32_t line, TextRange range)
{
    if (line >= layout.lines.size() || range.first >= range.last)
    {
        return {};
    }
    const CaretPlace from = PlaceCaret(layout, shown, range.first);
    const CaretPlace to = PlaceCaret(layout, shown, range.last);
    if (line < from.line || line > to.line)
    {
        return {};
    }

    // A line in the middle of the selection is covered end to end; the first
    // and last lines are cut where the selection starts and stops.
    const Rect box = LineBox(layout, line);
    const float left = line == from.line ? from.x : box.x;
    const float right = line == to.line ? to.x : box.x + box.width;
    return {.x = left, .y = box.y, .width = std::max(0.f, right - left), .height = box.height};
}

float MeasureLongestWord(const ShapedText &shaped, const Font &font, float size)
{
    ASSISI_ASSERT(size > 0.f && font.pixelSize > 0.f, "MeasureLongestWord needs a positive size and a sized font");

    float longest = 0.f;
    float word = 0.f;
    for (const ShapedGlyph &glyph : shaped.glyphs)
    {
        if (glyph.kind == GlyphClass::Ink)
        {
            word += glyph.advance;
            longest = std::max(longest, word);
        }
        else
        {
            word = 0.f;
        }
    }
    return std::ceil(longest * size / font.pixelSize);
}

} // namespace Assisi::Mondrian
