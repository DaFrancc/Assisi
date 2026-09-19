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
    layout.height = std::ceil(static_cast<float>(layout.lines.size()) * font.lineHeight * layout.scale);

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
