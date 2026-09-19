/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Ui.hpp>

#include <Assisi/Core/Assert.hpp>

#include <algorithm>

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

    _drawList.Finalize();
}

} // namespace Assisi::Mondrian
