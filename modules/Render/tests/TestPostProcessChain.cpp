/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Render/PostProcess.hpp>

using namespace Assisi::Render;

namespace
{
constexpr AaMode kAllModes[] = {AaMode::None, AaMode::MSAA, AaMode::FXAA, AaMode::MSAA_FXAA};
constexpr ChainOptions kWithoutOverlays{.overlays = false};
constexpr ChainOptions kWithOverlays{.overlays = true};

const ChainStep *FindStep(const ChainPlan &plan, ChainStage stage)
{
    const uint32_t index = plan.IndexOf(stage);
    return index < plan.stepCount ? &plan.steps[index] : nullptr;
}
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
            const ChainPlan plan = PlanChain(mode, options);
            CHECK(IsHdrSurface(plan.sceneTarget));
            // None used to render straight into the swapchain, which leaves
            // nowhere for the tone map to read from.
            CHECK(plan.sceneTarget != ChainSurface::Swapchain);
        }
    }
}

TEST_CASE("MSAA resolves in linear")
{
    // Both sides of the scene resolve are HDR, so samples average as radiance.
    // Resolving display-encoded samples averages in the wrong space.
    for (const AaMode mode : {AaMode::MSAA, AaMode::MSAA_FXAA})
    {
        CAPTURE(static_cast<int>(mode));
        for (const ChainOptions options : {kWithoutOverlays, kWithOverlays})
        {
            const ChainPlan plan = PlanChain(mode, options);
            const ChainStep *resolve = FindStep(plan, ChainStage::Resolve);
            REQUIRE(resolve != nullptr);
            CHECK(IsHdrSurface(resolve->source));
            CHECK(IsHdrSurface(resolve->destination));
        }
    }

    for (const AaMode mode : {AaMode::None, AaMode::FXAA})
    {
        CAPTURE(static_cast<int>(mode));
        CHECK_FALSE(PlanChain(mode, kWithoutOverlays).Has(ChainStage::Resolve));
    }
}

TEST_CASE("The tone map is the only step that leaves HDR")
{
    for (const AaMode mode : kAllModes)
    {
        CAPTURE(static_cast<int>(mode));
        for (const ChainOptions options : {kWithoutOverlays, kWithOverlays})
        {
            const ChainPlan plan = PlanChain(mode, options);

            uint32_t encodingSteps = 0;
            for (uint32_t i = 0; i < plan.stepCount; ++i)
            {
                if (IsHdrSurface(plan.steps[i].source) && !IsHdrSurface(plan.steps[i].destination))
                {
                    ++encodingSteps;
                    CHECK(plan.steps[i].stage == ChainStage::Tonemap);
                }
            }
            CHECK(encodingSteps == 1);
        }
    }
}

TEST_CASE("The tone map runs after the resolve, never before it")
{
    // Tone mapping each sample and then averaging is the gamma-space resolve this
    // chain exists to stop doing.
    for (const ChainOptions options : {kWithoutOverlays, kWithOverlays})
    {
        const ChainPlan plan = PlanChain(AaMode::MSAA_FXAA, options);
        REQUIRE(plan.stepCount >= 2);
        CHECK(plan.steps[0].stage == ChainStage::Resolve);
        CHECK(plan.steps[1].stage == ChainStage::Tonemap);
        CHECK(plan.steps[1].source == plan.steps[0].destination);
    }
}

TEST_CASE("FXAA is last and reads display-encoded input")
{
    for (const AaMode mode : {AaMode::FXAA, AaMode::MSAA_FXAA})
    {
        CAPTURE(static_cast<int>(mode));
        for (const ChainOptions options : {kWithoutOverlays, kWithOverlays})
        {
            const ChainPlan plan = PlanChain(mode, options);
            REQUIRE(plan.stepCount >= 2);

            const ChainStep &last = plan.steps[plan.stepCount - 1];
            CHECK(last.stage == ChainStage::Fxaa);
            CHECK_FALSE(IsHdrSurface(last.source));
            CHECK(last.destination == ChainSurface::Swapchain);
        }
    }
}

TEST_CASE("Overlays draw after the tone map, into a display-encoded target")
{
    // Editor chrome is already the colour it should appear on screen. Drawn into
    // the scene target it would be tone mapped, which restates it — and would
    // restate it again, differently, once exposure lands.
    for (const AaMode mode : kAllModes)
    {
        CAPTURE(static_cast<int>(mode));
        const ChainPlan plan = PlanChain(mode, kWithOverlays);

        const uint32_t overlays = plan.IndexOf(ChainStage::Overlays);
        REQUIRE(overlays < plan.stepCount);
        CHECK(overlays > plan.IndexOf(ChainStage::Tonemap));
        CHECK_FALSE(IsHdrSurface(plan.OverlaySurface()));
        // The seam is in-place: overlays composite onto the image, they do not
        // move it anywhere.
        CHECK(plan.steps[overlays].source == plan.steps[overlays].destination);
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
        const ChainPlan plan = PlanChain(mode, kWithOverlays);
        CHECK(IsMultisampleSurface(plan.OverlaySurface()));
        CHECK(IsMultisampleSurface(plan.sceneTarget));
    }

    for (const AaMode mode : {AaMode::None, AaMode::FXAA})
    {
        CAPTURE(static_cast<int>(mode));
        const ChainPlan plan = PlanChain(mode, kWithOverlays);
        CHECK_FALSE(IsMultisampleSurface(plan.OverlaySurface()));
    }
}

TEST_CASE("An app without overlays pays for no seam")
{
    // Pay for what you place: the overlay target and the copy that follows it
    // must not exist in a build that draws no chrome.
    for (const AaMode mode : kAllModes)
    {
        CAPTURE(static_cast<int>(mode));
        const ChainPlan without = PlanChain(mode, kWithoutOverlays);
        const ChainPlan with = PlanChain(mode, kWithOverlays);

        CHECK_FALSE(without.Has(ChainStage::Overlays));
        CHECK(without.stepCount < with.stepCount);
        for (uint32_t i = 0; i < without.stepCount; ++i)
        {
            CHECK(without.steps[i].source != ChainSurface::OverlayMultisample);
            CHECK(without.steps[i].destination != ChainSurface::OverlayMultisample);
        }
    }

    // With nothing following it, the tone map writes the swapchain directly —
    // no intermediate, no copy.
    for (const AaMode mode : {AaMode::None, AaMode::MSAA})
    {
        CAPTURE(static_cast<int>(mode));
        const ChainPlan plan = PlanChain(mode, kWithoutOverlays);
        const ChainStep *tonemap = FindStep(plan, ChainStage::Tonemap);
        REQUIRE(tonemap != nullptr);
        CHECK(tonemap->destination == ChainSurface::Swapchain);
        CHECK_FALSE(plan.Has(ChainStage::Blit));
    }
}

TEST_CASE("A Blit only ever moves an image the tone map has already mapped")
{
    // The Blit is the tone map shader told to copy, so it shares its push
    // constants. A chain that blitted an HDR surface would be asking that shader
    // to map radiance while it was told to copy — and one that ran two Tonemap
    // steps would expose and grade the frame twice.
    for (const AaMode mode : kAllModes)
    {
        CAPTURE(static_cast<int>(mode));
        for (const ChainOptions options : {kWithoutOverlays, kWithOverlays})
        {
            const ChainPlan plan = PlanChain(mode, options);

            uint32_t tonemaps = 0;
            for (uint32_t i = 0; i < plan.stepCount; ++i)
            {
                if (plan.steps[i].stage == ChainStage::Tonemap)
                {
                    ++tonemaps;
                }
                if (plan.steps[i].stage == ChainStage::Blit)
                {
                    CHECK_FALSE(IsHdrSurface(plan.steps[i].source));
                    CHECK(i > plan.IndexOf(ChainStage::Tonemap));
                }
            }
            CHECK(tonemaps == 1);
        }
    }
}

TEST_CASE("Every chain ends on the swapchain, and only at the end")
{
    for (const AaMode mode : kAllModes)
    {
        CAPTURE(static_cast<int>(mode));
        for (const ChainOptions options : {kWithoutOverlays, kWithOverlays})
        {
            const ChainPlan plan = PlanChain(mode, options);
            REQUIRE(plan.stepCount >= 1);
            REQUIRE(plan.stepCount <= kMaxChainSteps);
            CHECK(plan.steps[plan.stepCount - 1].destination == ChainSurface::Swapchain);

            for (uint32_t i = 0; i + 1 < plan.stepCount; ++i)
            {
                CHECK(plan.steps[i].destination != ChainSurface::Swapchain);
                // Each step reads what the one before it wrote.
                CHECK(plan.steps[i + 1].source == plan.steps[i].destination);
            }
            // And the first step reads what the scene drew.
            CHECK(plan.steps[0].source == plan.sceneTarget);
        }
    }
}
