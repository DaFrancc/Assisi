/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestShadowCadence.cpp
/// @brief The stale-cascade cases, written down.
///
/// A cascade kept when it should have been redrawn is a wrong image with no
/// visual tell — a shadow that quietly stopped following its object. Every test
/// here is one way that can happen: the camera walks, the sun turns, a caster
/// moves in, a caster moves out. The decision is device-free precisely so these
/// can be asserted rather than looked for on screen.

#include <doctest/doctest.h>

#include <Assisi/Render/ShadowCadence.hpp>

#include <cstdint>
#include <vector>

using namespace Assisi::Render;
using Assisi::Geometry::BoundingSphere;

namespace
{
constexpr std::uint32_t kResolution = 2048;

/// One cascade of @p radius centred at @p center, with the derived scalars the
/// real fit produces: an ortho box twice the radius deep, and a texel of that
/// box divided by the map's width.
ShadowCascade CascadeOf(float radius, const glm::vec3 &center, float splitNear, float splitFar)
{
    return ShadowCascade{.viewProjection = glm::mat4(1.f),
                         .center = center,
                         .radius = radius,
                         .splitNearView = splitNear,
                         .splitFarView = splitFar,
                         .worldUnitsPerTexel = 2.f * radius / static_cast<float>(kResolution),
                         .depthRange = 2.f * radius};
}

/// A fit of @p count cascades, each twice the radius of the one before it and
/// all centred on the origin — the shape the real fit produces for a camera
/// looking down a logarithmic split scheme.
CascadeFit FitOf(std::uint32_t count, const glm::vec3 &center = glm::vec3(0.f))
{
    CascadeFit fit;
    fit.count = count;
    float radius = 4.f;
    float near = 0.1f;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const float far = near + radius;
        fit.cascades[i] = CascadeOf(radius, center, near, far);
        near = far;
        radius *= 2.f;
    }
    return fit;
}

SunShadowCadenceFrame FrameAt(std::uint32_t index, std::span<const ShadowMover> movers = {},
                              const glm::vec3 &lightDirection = glm::vec3(0.f, -1.f, 0.f))
{
    SunShadowCadenceFrame frame;
    frame.frameIndex = index;
    frame.lightDirection = lightDirection;
    frame.movers = movers;
    return frame;
}

ShadowMover CasterAt(std::uint64_t id, const glm::vec3 &position, float radius = 0.5f)
{
    return ShadowMover{id, BoundingSphere{position, radius}};
}

/// The sun rotated @p radians about the world X axis, from straight down.
glm::vec3 SunTiltedBy(float radians)
{
    return glm::vec3(0.f, -std::cos(radians), std::sin(radians));
}
} // namespace

TEST_CASE("SunShadowCadence: a still scene draws every cascade once and then nothing")
{
    SunShadowCadence cadence;
    SunShadowCadencePlan plan;
    const CascadeFit fit = FitOf(4);

    cadence.Plan(FrameAt(1), fit, plan);
    CHECK(plan.redrawCount == 4);
    CHECK(cadence.Stats().kept == 0);

    for (std::uint32_t frame = 2; frame < 10; ++frame)
    {
        cadence.Plan(FrameAt(frame), fit, plan);
        CHECK(plan.redrawCount == 0);
        CHECK(cadence.Stats().kept == 4);
    }
    // Ages count from the frame the depth was drawn, which is what a panel
    // showing a cascade that has been paying nothing reads.
    CHECK(plan.ageFrames[0] == 8);
}

TEST_CASE("SunShadowCadence: switched off, every cascade draws every frame")
{
    SunShadowCadence cadence;
    SunShadowCadencePlan plan;
    const CascadeFit fit = FitOf(4);

    SunShadowCadenceFrame frame = FrameAt(1);
    frame.settings.enabled = false;
    for (std::uint32_t index = 1; index < 5; ++index)
    {
        frame.frameIndex = index;
        cadence.Plan(frame, fit, plan);
        CHECK(plan.redrawCount == 4);
        CHECK(cadence.Stats().kept == 0);
        CHECK(plan.ageFrames[3] == 0);
    }
}

TEST_CASE("SunShadowCadence: a kept cascade publishes the fit its depth was drawn at")
{
    SunShadowCadence cadence;
    SunShadowCadencePlan plan;
    const CascadeFit drawn = FitOf(1);
    cadence.Plan(FrameAt(1), drawn, plan);

    // A centre a tenth of a texel away — inside the tolerance, so the slice is
    // kept. What the shader must be handed is the matrix the depth in that slice
    // was rasterized with, not this frame's: sampling held depth with a newer
    // matrix slides every shadow by the difference.
    const float texel = drawn.cascades[0].worldUnitsPerTexel;
    const CascadeFit candidate = FitOf(1, glm::vec3(texel * 0.1f, 0.f, 0.f));
    cadence.Plan(FrameAt(2), candidate, plan);

    REQUIRE(plan.redrawCount == 0);
    CHECK(plan.fit.cascades[0].center == drawn.cascades[0].center);
    CHECK(plan.fit.cascades[0].center != candidate.cascades[0].center);
    CHECK(plan.fit.count == 1);
}

TEST_CASE("SunShadowCadence: a redrawn cascade publishes this frame's fit")
{
    SunShadowCadence cadence;
    SunShadowCadencePlan plan;
    cadence.Plan(FrameAt(1), FitOf(1), plan);

    const CascadeFit candidate = FitOf(1, glm::vec3(100.f, 0.f, 0.f));
    cadence.Plan(FrameAt(2), candidate, plan);

    REQUIRE(plan.redrawCount == 1);
    CHECK(plan.fit.cascades[0].center == candidate.cascades[0].center);
}

TEST_CASE("SunShadowCadence: the centre drift is measured in the cascade's own texels")
{
    SunShadowCadence cadence;
    SunShadowCadencePlan plan;
    const CascadeFit drawn = FitOf(3);
    cadence.Plan(FrameAt(1), drawn, plan);
    REQUIRE(plan.redrawCount == 3);

    // Three quarters of the nearest cascade's texel. The next cascade's texel is
    // twice as wide and the one after that four times, so the same walk is well
    // inside their tolerances — which is the whole of what "per-cascade cadence"
    // means here, and it comes from the fit rather than from a tier.
    const float step = drawn.cascades[0].worldUnitsPerTexel * 0.75f;
    CascadeFit candidate = FitOf(3);
    for (std::uint32_t i = 0; i < candidate.count; ++i)
    {
        candidate.cascades[i].center = glm::vec3(step, 0.f, 0.f);
    }
    cadence.Plan(FrameAt(2), candidate, plan);

    REQUIRE(plan.redrawCount == 1);
    CHECK(plan.redraw[0] == 0);
    CHECK(cadence.Stats().kept == 2);
}

TEST_CASE("SunShadowCadence: the sun's rotation trips every cascade at once")
{
    SunShadowCadence cadence;
    SunShadowCadencePlan plan;
    const CascadeFit fit = FitOf(4);
    cadence.Plan(FrameAt(1), fit, plan);

    // A cascade's depth range is twice its radius and its texel is twice its
    // radius over the resolution, so the ratio between them is the resolution in
    // every cascade alike. The sun term therefore has no per-cascade cadence in
    // it whatever the radii are: it trips all of them or none.
    const float threshold = 0.5f / static_cast<float>(kResolution);

    cadence.Plan(FrameAt(2, {}, SunTiltedBy(threshold * 0.5f)), fit, plan);
    CHECK(plan.redrawCount == 0);

    cadence.Plan(FrameAt(3, {}, SunTiltedBy(threshold * 2.f)), fit, plan);
    CHECK(plan.redrawCount == 4);
}

TEST_CASE("SunShadowCadence: a day-length sun turns slowly enough to be kept for minutes")
{
    SunShadowCadence cadence;
    SunShadowCadencePlan plan;
    const CascadeFit fit = FitOf(4);

    // A 24-hour cycle at 60 fps, which is the rate the whole feature is for. The
    // half-texel threshold is 0.5 / 2048 radians, so this should hold for about
    // 200 frames and then trip — and it is small enough that a dot-product angle
    // would measure it as no rotation at all and hold for ever.
    constexpr float kPerFrame = 1.2e-6f;
    std::uint32_t firstRedraw = 0;
    for (std::uint32_t frameIndex = 1; frameIndex <= 400; ++frameIndex)
    {
        cadence.Plan(FrameAt(frameIndex, {}, SunTiltedBy(kPerFrame * static_cast<float>(frameIndex))), fit, plan);
        if (frameIndex > 1 && plan.redrawCount > 0 && firstRedraw == 0)
        {
            firstRedraw = frameIndex;
        }
    }
    CHECK(firstRedraw > 100);
    CHECK(firstRedraw < 400);
}

TEST_CASE("SunShadowCadence: a six-second day redraws every cascade every frame")
{
    SunShadowCadence cadence;
    SunShadowCadencePlan plan;
    const CascadeFit fit = FitOf(4);

    // The other end of the same expression, with no boundary crossed between the
    // two: 0.017 radians a frame is past every cascade's threshold, so the
    // system is the per-frame implementation exactly.
    constexpr float kPerFrame = 0.017f;
    for (std::uint32_t frameIndex = 1; frameIndex <= 10; ++frameIndex)
    {
        cadence.Plan(FrameAt(frameIndex, {}, SunTiltedBy(kPerFrame * static_cast<float>(frameIndex))), fit, plan);
        CHECK(plan.redrawCount == 4);
    }
}

TEST_CASE("SunShadowCadence: a wider tolerance keeps a rotation a narrower one redraws")
{
    // Between the two tolerances: a two-texel edge step is more than half a texel
    // and less than four.
    const float rotation = 2.f / static_cast<float>(kResolution);
    const CascadeFit fit = FitOf(2);

    SunShadowCadence tight;
    SunShadowCadencePlan plan;
    SunShadowCadenceFrame frame = FrameAt(1);
    frame.settings.driftTexels = 0.5f;
    tight.Plan(frame, fit, plan);
    frame = FrameAt(2, {}, SunTiltedBy(rotation));
    frame.settings.driftTexels = 0.5f;
    tight.Plan(frame, fit, plan);
    CHECK(plan.redrawCount == 2);

    SunShadowCadence loose;
    frame = FrameAt(1);
    frame.settings.driftTexels = 4.f;
    loose.Plan(frame, fit, plan);
    frame = FrameAt(2, {}, SunTiltedBy(rotation));
    frame.settings.driftTexels = 4.f;
    loose.Plan(frame, fit, plan);
    CHECK(plan.redrawCount == 0);
}

TEST_CASE("SunShadowCadence: a changed split redraws whatever the drift says")
{
    SunShadowCadence cadence;
    SunShadowCadencePlan plan;
    cadence.Plan(FrameAt(1), FitOf(2), plan);

    // The camera's field of view or its far plane moved, so the slice this
    // cascade covers is a different slice. Nothing about the depth it holds
    // describes it any more, and no tolerance applies to that.
    CascadeFit candidate = FitOf(2);
    candidate.cascades[1].splitFarView += 1.f;
    cadence.Plan(FrameAt(2), candidate, plan);

    REQUIRE(plan.redrawCount == 1);
    CHECK(plan.redraw[0] == 1);
}

TEST_CASE("SunShadowCadence: a caster moving inside a cascade dirties it that frame")
{
    SunShadowCadence cadence;
    SunShadowCadencePlan plan;
    const CascadeFit fit = FitOf(3);
    cadence.Plan(FrameAt(1), fit, plan);

    // Its first move is unrecorded, so every cascade is dirtied. That is the
    // conservative half of the rule, and it costs one frame per caster.
    const ShadowMover first[] = {CasterAt(7, glm::vec3(1.f, 0.f, 0.f))};
    cadence.Plan(FrameAt(2, first), fit, plan);
    CHECK(plan.redrawCount == 3);
    CHECK(cadence.Stats().unrecordedMovers == 1);

    // Moving again inside the nearest cascade dirties that one alone: it is well
    // inside cascade 0's volume and the wider cascades are centred on the same
    // point, so the sweep reaches all three — which is the honest answer, since
    // a caster in cascade 0 really is in the volume of every cascade around it.
    const ShadowMover again[] = {CasterAt(7, glm::vec3(1.5f, 0.f, 0.f))};
    cadence.Plan(FrameAt(3, again), fit, plan);
    CHECK(plan.redrawCount == 3);
    CHECK(cadence.Stats().dirtiedByMotion == 3);
    CHECK(cadence.Stats().unrecordedMovers == 0);
}

TEST_CASE("SunShadowCadence: a caster only the near cascade holds dirties only it")
{
    SunShadowCadence cadence;
    SunShadowCadencePlan plan;

    // Two cascades side by side rather than concentric, so a caster can be
    // inside one and outside the other — which is what the per-cascade
    // invalidation has to be able to tell apart.
    CascadeFit fit;
    fit.count = 2;
    fit.cascades[0] = CascadeOf(4.f, glm::vec3(0.f), 0.1f, 4.f);
    fit.cascades[1] = CascadeOf(4.f, glm::vec3(100.f, 0.f, 0.f), 4.f, 8.f);
    cadence.Plan(FrameAt(1), fit, plan);

    const ShadowMover known[] = {CasterAt(3, glm::vec3(0.f, 0.f, 0.f))};
    cadence.Plan(FrameAt(2, known), fit, plan); // unrecorded: dirties both
    REQUIRE(plan.redrawCount == 2);

    const ShadowMover moved[] = {CasterAt(3, glm::vec3(0.5f, 0.f, 0.f))};
    cadence.Plan(FrameAt(3, moved), fit, plan);
    REQUIRE(plan.redrawCount == 1);
    CHECK(plan.redraw[0] == 0);
}

TEST_CASE("SunShadowCadence: a caster leaving a cascade dirties the cascade it left")
{
    SunShadowCadence cadence;
    SunShadowCadencePlan plan;

    CascadeFit fit;
    fit.count = 1;
    fit.cascades[0] = CascadeOf(4.f, glm::vec3(0.f), 0.1f, 4.f);
    cadence.Plan(FrameAt(1), fit, plan);

    const ShadowMover inside[] = {CasterAt(9, glm::vec3(0.f, 0.f, 0.f))};
    cadence.Plan(FrameAt(2, inside), fit, plan);
    REQUIRE(plan.redrawCount == 1);

    // Gone, far past anything the cascade covers. Where it now stands reaches
    // nothing — so a rule that tested only the new pose would keep the cascade,
    // and the cascade would go on shadowing an object that is no longer there.
    const ShadowMover gone[] = {CasterAt(9, glm::vec3(0.f, 0.f, 500.f))};
    cadence.Plan(FrameAt(3, gone), fit, plan);
    CHECK(plan.redrawCount == 1);
    CHECK(cadence.Stats().dirtiedByMotion == 1);
}

TEST_CASE("SunShadowCadence: a caster moving where no cascade reaches dirties nothing")
{
    SunShadowCadence cadence;
    SunShadowCadencePlan plan;

    CascadeFit fit;
    fit.count = 1;
    fit.cascades[0] = CascadeOf(4.f, glm::vec3(0.f), 0.1f, 4.f);
    cadence.Plan(FrameAt(1), fit, plan);

    // Off in the distance, and moving. The first frame is unrecorded and dirties
    // everything; from then on the cascade knows it holds nothing of this caster
    // and stops paying for it, which is what keeps something walking past the
    // shadow distance from costing a redraw on every frame of its journey.
    const ShadowMover far1[] = {CasterAt(11, glm::vec3(0.f, 0.f, 400.f))};
    cadence.Plan(FrameAt(2, far1), fit, plan);
    REQUIRE(plan.redrawCount == 1);

    for (std::uint32_t frame = 3; frame < 8; ++frame)
    {
        const ShadowMover onward[] = {CasterAt(11, glm::vec3(static_cast<float>(frame), 0.f, 400.f))};
        cadence.Plan(FrameAt(frame, onward), fit, plan);
        CHECK(plan.redrawCount == 0);
        CHECK(cadence.Stats().unrecordedMovers == 0);
    }
}

TEST_CASE("SunShadowCadence: a caster up-light of a cascade still dirties it")
{
    SunShadowCadence cadence;
    SunShadowCadencePlan plan;

    CascadeFit fit;
    fit.count = 1;
    fit.cascades[0] = CascadeOf(4.f, glm::vec3(0.f), 0.1f, 4.f);
    cadence.Plan(FrameAt(1), fit, plan);

    const ShadowMover overhead[] = {CasterAt(5, glm::vec3(0.f, 200.f, 0.f))};
    cadence.Plan(FrameAt(2, overhead), fit, plan);
    REQUIRE(plan.redrawCount == 1);

    // Still overhead, and the sun travels straight down — so it casts into the
    // cascade from far outside it. Testing the sphere where it sits would keep
    // the cascade and lose the shadow of everything above the camera.
    const ShadowMover stillOverhead[] = {CasterAt(5, glm::vec3(1.f, 200.f, 0.f))};
    cadence.Plan(FrameAt(3, stillOverhead), fit, plan);
    CHECK(plan.redrawCount == 1);
    CHECK(cadence.Stats().dirtiedByMotion == 1);
}

TEST_CASE("SunShadowCadence: forgetting draws everything again")
{
    SunShadowCadence cadence;
    SunShadowCadencePlan plan;
    const CascadeFit fit = FitOf(4);

    cadence.Plan(FrameAt(1), fit, plan);
    cadence.Plan(FrameAt(2), fit, plan);
    REQUIRE(plan.redrawCount == 0);

    // What a time skip, a level load or a reallocation wants: nothing stale is
    // displayed, and the whole of today's per-frame cost is paid once.
    cadence.Forget();
    cadence.Plan(FrameAt(3), fit, plan);
    CHECK(plan.redrawCount == 4);
}

TEST_CASE("SunShadowCadence: an unfitted frame keeps nothing across it")
{
    SunShadowCadence cadence;
    SunShadowCadencePlan plan;
    const CascadeFit fit = FitOf(2);

    cadence.Plan(FrameAt(1), fit, plan);
    cadence.Plan(FrameAt(2), CascadeFit{}, plan);
    CHECK(plan.redrawCount == 0);
    CHECK(plan.fit.count == 0);

    // The sun came back. Whatever was in those slices is a frame old at best and
    // was drawn for a fit nothing has checked since.
    cadence.Plan(FrameAt(3), fit, plan);
    CHECK(plan.redrawCount == 2);
}

TEST_CASE("SunShadowCadence: raising the cascade count draws the new ones")
{
    SunShadowCadence cadence;
    SunShadowCadencePlan plan;

    cadence.Plan(FrameAt(1), FitOf(2), plan);
    cadence.Plan(FrameAt(2), FitOf(4), plan);

    // The two that already existed are fitted identically and keep their depth;
    // the two that did not have never been drawn.
    REQUIRE(plan.redrawCount == 2);
    CHECK(plan.redraw[0] == 2);
    CHECK(plan.redraw[1] == 3);
}
