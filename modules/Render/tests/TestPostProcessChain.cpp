/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Render/PostProcess.hpp>

using namespace Assisi::Render;

namespace
{
constexpr AaMode kAllModes[] = {AaMode::None, AaMode::MSAA, AaMode::FXAA, AaMode::MSAA_FXAA};
constexpr ChainOptions kWithoutOverlays{.overlays = false};
constexpr ChainOptions kWithOverlays{.overlays = true};
} // namespace

TEST_CASE("The scene always draws into an HDR target")
{
    // The defect this guards: the scene target used to be created with the
    // swapchain's own 8-bit format, so lighting was clamped and gamma-encoded
    // before any post stage could act on it.
    CHECK(kSceneColorFormat == nvrhi::Format::RGBA16_FLOAT);

    for (const AaMode mode : kAllModes)
    {
        CAPTURE(static_cast<int>(mode));
        for (const ChainOptions options : {kWithoutOverlays, kWithOverlays})
        {
            const ChainRoute route = RouteChain(mode, options);
            CHECK(IsHdrSurface(route.sceneTarget));
            // None used to render straight into the swapchain, which leaves
            // nowhere for the tone map to read from.
            CHECK(route.sceneTarget != ChainSurface::Swapchain);
        }
    }
}

TEST_CASE("MSAA resolves in linear, into the single-sampled image the tone map reads")
{
    // Both sides of the scene resolve are HDR, so samples average as radiance.
    // Resolving display-encoded samples averages in the wrong space, and tone
    // mapping each sample before averaging is the same mistake.
    for (const AaMode mode : {AaMode::MSAA, AaMode::MSAA_FXAA})
    {
        CAPTURE(static_cast<int>(mode));
        for (const ChainOptions options : {kWithoutOverlays, kWithOverlays})
        {
            const ChainRoute route = RouteChain(mode, options);
            REQUIRE(route.ResolvesScene());
            CHECK(IsHdrSurface(route.sceneTarget));
            CHECK(IsHdrSurface(route.sceneHdr));
            CHECK_FALSE(IsMultisampleSurface(route.sceneHdr));
        }
    }

    for (const AaMode mode : {AaMode::None, AaMode::FXAA})
    {
        CAPTURE(static_cast<int>(mode));
        CHECK_FALSE(RouteChain(mode, kWithoutOverlays).ResolvesScene());
    }
}

TEST_CASE("The tone map is the only step that leaves HDR")
{
    // It reads the one HDR image after the scene and writes display values;
    // every image after it holds display values too.
    for (const AaMode mode : kAllModes)
    {
        CAPTURE(static_cast<int>(mode));
        for (const ChainOptions options : {kWithoutOverlays, kWithOverlays})
        {
            const ChainRoute route = RouteChain(mode, options);
            CHECK(IsHdrSurface(route.sceneHdr));
            CHECK_FALSE(IsHdrSurface(route.toneMapped));
            CHECK_FALSE(IsHdrSurface(route.display));
        }
    }
}

TEST_CASE("FXAA is last and reads display-encoded input")
{
    for (const AaMode mode : {AaMode::FXAA, AaMode::MSAA_FXAA})
    {
        CAPTURE(static_cast<int>(mode));
        for (const ChainOptions options : {kWithoutOverlays, kWithOverlays})
        {
            const ChainRoute route = RouteChain(mode, options);
            REQUIRE(route.fxaa);
            CHECK_FALSE(IsHdrSurface(route.display));
            CHECK_FALSE(IsMultisampleSurface(route.display));
            // FXAA writes the swapchain, so its input is somewhere else and no
            // copy follows it.
            CHECK(route.display != ChainSurface::Swapchain);
            CHECK_FALSE(route.CopiesToSwapchain());
        }
    }
}

TEST_CASE("Overlays draw after the tone map, into a display-encoded target")
{
    // Editor chrome is already the colour it should appear on screen. Drawn into
    // the scene target it would be tone mapped, which restates it — and would
    // restate it again, differently, once exposure lands. The seam is in place:
    // overlays draw onto the image the tone map wrote.
    for (const AaMode mode : kAllModes)
    {
        CAPTURE(static_cast<int>(mode));
        const ChainRoute route = RouteChain(mode, kWithOverlays);
        CHECK(route.overlays);
        CHECK_FALSE(IsHdrSurface(route.toneMapped));
        CHECK(route.toneMapped != ChainSurface::Swapchain);
    }
}

TEST_CASE("Overlays keep the scene's sample count")
{
    // They share the scene's depth buffer to depth-test against it, and a depth
    // buffer cannot be read at a sample count other than its own. Dropping to 1x
    // would also cost wireframes the anti-aliasing they have today.
    for (const AaMode mode : {AaMode::MSAA, AaMode::MSAA_FXAA})
    {
        CAPTURE(static_cast<int>(mode));
        const ChainRoute route = RouteChain(mode, kWithOverlays);
        CHECK(IsMultisampleSurface(route.toneMapped));
        CHECK(IsMultisampleSurface(route.sceneTarget));
        // And they resolve before anything reads them single-sampled.
        CHECK(route.ResolvesOverlays());
        CHECK_FALSE(IsMultisampleSurface(route.display));
    }

    for (const AaMode mode : {AaMode::None, AaMode::FXAA})
    {
        CAPTURE(static_cast<int>(mode));
        const ChainRoute route = RouteChain(mode, kWithOverlays);
        CHECK_FALSE(IsMultisampleSurface(route.toneMapped));
        CHECK_FALSE(route.ResolvesOverlays());
    }
}

TEST_CASE("An app without overlays pays for no seam")
{
    // Pay for what you place: the overlay target and the resolve that follows it
    // must not exist in a build that draws no chrome.
    for (const AaMode mode : kAllModes)
    {
        CAPTURE(static_cast<int>(mode));
        const ChainRoute route = RouteChain(mode, kWithoutOverlays);
        CHECK_FALSE(route.overlays);
        CHECK(route.toneMapped != ChainSurface::OverlayMultisample);
        CHECK(route.display != ChainSurface::OverlayMultisample);
        CHECK_FALSE(route.ResolvesOverlays());
    }

    // With nothing following it, the tone map writes the swapchain directly —
    // no intermediate, no copy.
    for (const AaMode mode : {AaMode::None, AaMode::MSAA})
    {
        CAPTURE(static_cast<int>(mode));
        const ChainRoute route = RouteChain(mode, kWithoutOverlays);
        CHECK(route.toneMapped == ChainSurface::Swapchain);
        CHECK_FALSE(route.CopiesToSwapchain());
    }
}

TEST_CASE("A copy only ever moves an image the tone map has already mapped")
{
    // The copy is the tone map shader told to pass its input through. A route
    // that copied an HDR surface would put radiance on the screen unmapped.
    for (const AaMode mode : kAllModes)
    {
        CAPTURE(static_cast<int>(mode));
        for (const ChainOptions options : {kWithoutOverlays, kWithOverlays})
        {
            const ChainRoute route = RouteChain(mode, options);
            if (route.CopiesToSwapchain())
            {
                CHECK_FALSE(IsHdrSurface(route.display));
                CHECK_FALSE(IsMultisampleSurface(route.display));
            }
        }
    }
}

TEST_CASE("Every route reaches the swapchain exactly once, at the end")
{
    for (const AaMode mode : kAllModes)
    {
        CAPTURE(static_cast<int>(mode));
        for (const ChainOptions options : {kWithoutOverlays, kWithOverlays})
        {
            const ChainRoute route = RouteChain(mode, options);
            // FXAA writes it, or the copy does, or the picture is already there.
            const int writers = (route.fxaa ? 1 : 0) + (route.CopiesToSwapchain() ? 1 : 0) +
                                (route.display == ChainSurface::Swapchain ? 1 : 0);
            CHECK(writers == 1);
            // Nothing before the end lands on it: the scene never does, and the
            // tone map only when it is the last step.
            CHECK(route.sceneTarget != ChainSurface::Swapchain);
            CHECK(route.sceneHdr != ChainSurface::Swapchain);
            if (route.toneMapped == ChainSurface::Swapchain)
            {
                CHECK(route.display == ChainSurface::Swapchain);
                CHECK_FALSE(route.overlays);
                CHECK_FALSE(route.fxaa);
            }
        }
    }
}
