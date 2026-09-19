/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Ui.hpp>

#include <Assisi/Mondrian/Text.hpp>

#include <Assisi/Core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

constexpr Math::Color4<Math::ColorSpace::Srgb> kTileColor{0.9f, 0.2f, 0.1f, 1.f};
constexpr Math::Color4<Math::ColorSpace::Srgb> kBorderColor{1.f, 1.f, 1.f, 1.f};
constexpr Math::Color4<Math::ColorSpace::Srgb> kUntinted{1.f, 1.f, 1.f, 1.f};
constexpr Rect kWholeTexture{.x = 0.f, .y = 0.f, .width = 1.f, .height = 1.f};

/// The paragraph the placeholder sets below the strip, spelled in bytes so the
/// source file's encoding cannot change it. Accented Latin, a no-break space
/// and a forced break, so a capture shows wrapping, alignment and non-ASCII
/// text at a glance. It reads: "Mondrian sets text in lines, breaking at
/// spaces and aligning each one.", then on a new line "Crème brûlée, jalapeño,
/// naïve café, Straße, smørrebrød: 100 km." (a hex escape runs on through any
/// hex digit, hence the splits before an e or d).
constexpr std::string_view kSampleParagraph =
    "Mondrian sets text in lines, breaking at spaces and aligning each one.\n"
    "Cr\xC3\xA8me br\xC3\xBBl\xC3\xA9"
    "e, jalape\xC3\xB1o, na\xC3\xAFve caf\xC3\xA9, Stra\xC3\x9F"
    "e, sm\xC3\xB8rrebr\xC3\xB8"
    "d: 100\xC2\xA0km.";

/// Text sizes as fractions of the viewport's shorter side, the second double
/// the first. Each is set once in every alignment, side by side.
constexpr std::array kTextFractions{0.02f, 0.04f};

constexpr std::size_t kColumnCount = static_cast<std::size_t>(TextAlign::Count);

constexpr Math::Color4<Math::ColorSpace::Srgb> kTextColor{1.f, 1.f, 1.f, 1.f};

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
        const Font &font         = *_placeholderFont;
        const ShapedText shaped  = Shape(kSampleParagraph, font);
        const float columnCount  = static_cast<float>(kColumnCount);
        const float columnWidth  = (static_cast<float>(viewport.width) - 2.f * margin - (columnCount - 1.f) * gap) /
                                  columnCount;
        float top = std::round(margin + side + gap);
        for (const float fraction : kTextFractions)
        {
            float blockHeight = 0.f;
            for (std::size_t column = 0; column < kColumnCount; ++column)
            {
                const TextLayout layout = LayoutText(shaped, font, shorter * fraction, columnWidth,
                                                     static_cast<TextAlign>(column));
                const Point origin{.x = std::round(margin + static_cast<float>(column) * (columnWidth + gap)),
                                   .y = top};
                DrawGlyphs(_drawList, layout, _placeholderFontAtlas, origin, kTextColor);
                blockHeight = std::max(blockHeight, layout.height);
            }
            top += std::round(blockHeight + gap);
        }
    }

    _drawList.Finalize();
}

} // namespace Assisi::Mondrian
