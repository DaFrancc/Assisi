/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Ui.hpp>

#include <Assisi/Core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Assisi::Mondrian
{
namespace
{

// The placeholder strip drawn while no screen exists: one tile of each shape
// the quad pass draws, in a row along the top of the window, so a capture shows
// at a glance whether each one renders. Sized from the viewport's shorter side,
// so the row stays in view at any size and collapses to nothing at zero.

/// Which tile is which, left to right.
enum class Tile : uint32_t
{
    Solid,
    Bordered,
    Rounded,
    Cut,
    Textured,
    Clipped,
    Count
};

/// Tile side, gap and margin as fractions of the viewport's shorter side.
constexpr float kTileFraction   = 0.1f;
constexpr float kGapFraction    = 0.025f;
constexpr float kMarginFraction = 0.05f;

/// Corner radius and border width as fractions of the tile side.
constexpr float kCornerFraction = 0.25f;
constexpr float kBorderFraction = 0.08f;

/// The clipped tile keeps this fraction of its width, cutting through its
/// rounded right-hand corners.
constexpr float kClipKeepFraction = 0.6f;

constexpr Color kTileColor{.r = 0.9f, .g = 0.2f, .b = 0.1f, .a = 1.f};
constexpr Color kBorderColor{.r = 1.f, .g = 1.f, .b = 1.f, .a = 1.f};
constexpr Color kUntinted{.r = 1.f, .g = 1.f, .b = 1.f, .a = 1.f};
constexpr Rect kWholeTexture{.x = 0.f, .y = 0.f, .width = 1.f, .height = 1.f};

/// The word the placeholder writes below the strip, once at each size, so a
/// capture shows at a glance whether one atlas stays crisp small and large.
/// ASCII, so every font the placeholder is given can draw it.
constexpr std::string_view kSampleWord = "Assisi";

/// Text sizes as fractions of the viewport's shorter side, each double the
/// last, so a row is the one above it scaled.
constexpr std::array kTextFractions{0.025f, 0.05f, 0.1f};

constexpr Color kTextColor{.r = 1.f, .g = 1.f, .b = 1.f, .a = 1.f};

/// Where the next glyph goes: the pen's left edge and the line's baseline.
struct Pen
{
    float x        = 0.f;
    float baseline = 0.f;
};

/// Writes kSampleWord in @p font at @p pen, @p size pixels high. A pen walk and
/// nothing more: no kerning, no wrapping, one glyph per byte of ASCII.
void WriteSample(DrawList &list, const Font &font, TextureId atlas, Pen pen, float size)
{
    const float scale = size / font.pixelSize;
    const float atlasWidth  = static_cast<float>(font.atlasWidth);
    const float atlasHeight = static_cast<float>(font.atlasHeight);
    for (const char letter : kSampleWord)
    {
        const std::optional<uint32_t> index = font.GlyphFor(static_cast<unsigned char>(letter));
        const Glyph *glyph                  = index ? font.FindGlyph(*index) : nullptr;
        if (glyph == nullptr)
        {
            continue;
        }
        if (glyph->width > 0 && glyph->height > 0)
        {
            const Rect rect{.x      = pen.x + static_cast<float>(glyph->bearingX) * scale,
                            .y      = pen.baseline - static_cast<float>(glyph->bearingY) * scale,
                            .width  = static_cast<float>(glyph->width) * scale,
                            .height = static_cast<float>(glyph->height) * scale};
            const Rect uv{.x      = static_cast<float>(glyph->x) / atlasWidth,
                          .y      = static_cast<float>(glyph->y) / atlasHeight,
                          .width  = static_cast<float>(glyph->width) / atlasWidth,
                          .height = static_cast<float>(glyph->height) / atlasHeight};
            list.Quad(rect).Fill(kTextColor).Texture(atlas, uv).Kind(QuadKind::Glyph);
        }
        pen.x += glyph->advance * scale;
    }
}

} // namespace

void Ui::ProcessInput()
{
    ASSISI_ASSERT(_nextStep == FrameStep::AwaitingInput, "Ui::ProcessInput called twice without a Sync between");
    _nextStep = FrameStep::AwaitingSync;
}

void Ui::Sync(Extent viewport)
{
    ASSISI_ASSERT(_nextStep == FrameStep::AwaitingSync, "Ui::Sync called without a ProcessInput before it");
    _nextStep = FrameStep::AwaitingInput;

    const float shorter = static_cast<float>(std::min(viewport.width, viewport.height));
    const float side    = shorter * kTileFraction;
    const float gap     = shorter * kGapFraction;
    const float margin  = shorter * kMarginFraction;
    const float radius  = side * kCornerFraction;

    const auto tileRect = [&](Tile tile)
    {
        return Rect{.x      = margin + static_cast<float>(tile) * (side + gap),
                    .y      = margin,
                    .width  = side,
                    .height = side};
    };

    _drawList.Clear();
    _drawList.Quad(tileRect(Tile::Solid)).Fill(kTileColor);
    _drawList.Quad(tileRect(Tile::Bordered)).Fill(kTileColor).Border(side * kBorderFraction, kBorderColor);
    _drawList.Quad(tileRect(Tile::Rounded)).Fill(kTileColor).Corners(radius, CornerStyle::Rounded);
    _drawList.Quad(tileRect(Tile::Cut)).Fill(kTileColor).Corners(radius, CornerStyle::Cut);
    _drawList.Quad(tileRect(Tile::Textured)).Fill(kUntinted).Texture(_placeholderTexture, kWholeTexture);

    Rect clip  = tileRect(Tile::Clipped);
    clip.width *= kClipKeepFraction;
    _drawList.Quad(tileRect(Tile::Clipped)).Fill(kTileColor).Corners(radius, CornerStyle::Rounded).Clip(clip);

    if (_placeholderFont != nullptr && _placeholderFont->pixelSize > 0.f && shorter > 0.f)
    {
        const Font &font = *_placeholderFont;
        Pen pen{.x = margin, .baseline = margin + side};
        for (const float fraction : kTextFractions)
        {
            const float size  = shorter * fraction;
            const float scale = size / font.pixelSize;
            pen.baseline += gap + font.ascender * scale;
            WriteSample(_drawList, font, _placeholderFontAtlas, pen, size);
            pen.baseline -= font.descender * scale;
        }
    }

    _drawList.Finalize();
}

} // namespace Assisi::Mondrian
