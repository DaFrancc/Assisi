/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Ui.hpp>

#include <Assisi/Core/Assert.hpp>

namespace Assisi::Mondrian
{
namespace
{

/// The placeholder quad drawn while no screen exists, so the path from Sync to
/// the swapchain can be seen working. Placed as a fraction of the viewport, so
/// it stays in view at any size and collapses to nothing at zero.
constexpr float kPlaceholderInset = 0.05f;
constexpr float kPlaceholderSize  = 0.2f;

/// Opaque and saturated, so it cannot be mistaken for anything the scene draws.
constexpr Color kPlaceholderColor{.r = 0.9f, .g = 0.2f, .b = 0.1f, .a = 1.f};

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

    const float width  = static_cast<float>(viewport.width);
    const float height = static_cast<float>(viewport.height);

    _drawList.Clear();
    _drawList.Push({.rect  = {.x      = width * kPlaceholderInset,
                              .y      = height * kPlaceholderInset,
                              .width  = width * kPlaceholderSize,
                              .height = height * kPlaceholderSize},
                    .color = kPlaceholderColor});
}

} // namespace Assisi::Mondrian
