/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestOutlinePicking.cpp
/// @brief A light's outline is grabbed by how near the cursor is to the drawn
/// line, in pixels, and reports the depth of the point actually grabbed.
///
/// Two things go wrong here and neither shows up in a screenshot. A segment that
/// crosses the eye plane has an endpoint with negative w, and dividing that
/// through mirrors it to the far side of the viewport — so a line behind the
/// camera becomes a clickable streak across the screen, in a place no shape is.
/// And the point of a segment nearest the cursor on screen is not the point at
/// the matching fraction along it: perspective packs the far half of a line into
/// fewer pixels, so reporting the linear one gives a depth belonging to some
/// other part of the line, and the outline then wins or loses against a wall it
/// is not beside.
///
/// So the assertions are about pixel distances that are checkable by hand — a
/// camera with a 90 degree field of view over a square viewport puts the frustum
/// edge exactly at the viewport edge — and about depths compared to the geometry
/// they come from rather than to a recorded number.

#include <doctest/doctest.h>

#include <Assisi/Editor/ScenePick.hpp>
#include <Assisi/Math/GLM.hpp>

using Assisi::Editor::kOutlinePickPixels;
using Assisi::Editor::PickRay;
using Assisi::Editor::ScreenDistanceToSegment;

namespace
{

/// A square viewport, so one degree is the same number of pixels on both axes and
/// a hand-computed screen position needs no aspect correction.
constexpr float kViewport = 800.f;

/// Half the viewport: where the camera's forward axis lands.
constexpr float kCenter = kViewport * 0.5f;

constexpr float kEpsilon = 1e-3f;

/// "On the line", in pixels. A hundredth of one is far below anything a cursor
/// can express, so a hit this close is on the line as far as clicking goes.
constexpr float kSubPixel = 0.01f;

/// A camera at the origin looking down -Z, 90 degrees across.
///
/// The field of view is the whole point of the number: tan(45 degrees) is one, so
/// a point at (d, 0, -d) sits exactly on the right frustum edge and lands exactly
/// on the right viewport edge. Every expected pixel below is derived from that.
PickRay Camera()
{
    PickRay ray;
    ray.origin         = glm::vec3(0.f);
    ray.direction      = glm::vec3(0.f, 0.f, -1.f);
    ray.viewProjection = glm::perspective(glm::radians(90.f), 1.f, 0.1f, 1000.f);
    ray.viewportSize   = glm::vec2(kViewport);
    ray.valid          = true;
    return ray;
}

/// Where a world point lands, by the same rule the pick uses — for placing a
/// cursor at a known offset from a known point on a segment.
glm::vec2 Screen(const PickRay &ray, glm::vec3 world)
{
    const glm::vec4 clip = ray.viewProjection * glm::vec4(world, 1.f);
    const glm::vec2 ndc  = glm::vec2(clip) / clip.w;
    return glm::vec2((ndc.x * 0.5f + 0.5f) * ray.viewportSize.x, (0.5f - ndc.y * 0.5f) * ray.viewportSize.y);
}

} // namespace

TEST_CASE("ScreenDistanceToSegment: the cursor on the line reads zero pixels")
{
    const PickRay ray = Camera();

    // A horizontal segment ten metres out, spanning the middle of the view.
    const glm::vec3 a(-3.f, 0.f, -10.f);
    const glm::vec3 b(3.f, 0.f, -10.f);

    float pixels   = 0.f;
    float distance = 0.f;
    REQUIRE(ScreenDistanceToSegment(ray, Screen(ray, glm::vec3(1.f, 0.f, -10.f)), a, b, pixels, distance));

    CHECK(pixels < kSubPixel);
    // The nearest point is the one the cursor was placed over, so the distance is
    // that point's own — not either endpoint's, which are further out.
    CHECK(distance == doctest::Approx(glm::length(glm::vec3(1.f, 0.f, -10.f))).epsilon(kEpsilon));
}

TEST_CASE("ScreenDistanceToSegment: distance off the line is measured in pixels")
{
    const PickRay ray = Camera();

    const glm::vec3 a(-3.f, 0.f, -10.f);
    const glm::vec3 b(3.f, 0.f, -10.f);

    // Straight down from a point on the line. Pixels, not world units: that is
    // what makes the grab radius mean the same thing at every distance.
    const glm::vec2 onLine = Screen(ray, glm::vec3(0.f, 0.f, -10.f));

    float pixels   = 0.f;
    float distance = 0.f;
    REQUIRE(ScreenDistanceToSegment(ray, onLine + glm::vec2(0.f, 20.f), a, b, pixels, distance));
    CHECK(pixels == doctest::Approx(20.f).epsilon(kEpsilon));

    // And the tolerance is a tolerance: just outside it is a miss, just inside a
    // hit. Asserted here rather than trusted, because every caller compares
    // against kOutlinePickPixels and nothing else states what it is measured in.
    REQUIRE(ScreenDistanceToSegment(ray, onLine + glm::vec2(0.f, kOutlinePickPixels - 1.f), a, b, pixels,
                                    distance));
    CHECK(pixels < kOutlinePickPixels);
    REQUIRE(ScreenDistanceToSegment(ray, onLine + glm::vec2(0.f, kOutlinePickPixels + 1.f), a, b, pixels,
                                    distance));
    CHECK(pixels > kOutlinePickPixels);
}

TEST_CASE("ScreenDistanceToSegment: past the end of the line, the end is nearest")
{
    const PickRay ray = Camera();

    const glm::vec3 a(-3.f, 0.f, -10.f);
    const glm::vec3 b(0.f, 0.f, -10.f);

    // A cursor beyond b along the line's own direction. Clamped to the endpoint,
    // so the segment does not act like the infinite line through it — a cone's rib
    // must not be clickable past the rim.
    const glm::vec2 beyond = Screen(ray, glm::vec3(2.f, 0.f, -10.f));

    float pixels   = 0.f;
    float distance = 0.f;
    REQUIRE(ScreenDistanceToSegment(ray, beyond, a, b, pixels, distance));

    CHECK(pixels == doctest::Approx(beyond.x - Screen(ray, b).x).epsilon(kEpsilon));
    CHECK(distance == doctest::Approx(glm::length(b)).epsilon(kEpsilon));
}

TEST_CASE("ScreenDistanceToSegment: a segment behind the camera has no screen position")
{
    const PickRay ray = Camera();

    // Both endpoints behind the eye. Without the w test this projects to a line on
    // screen — mirrored through the origin — and every pixel of it is clickable.
    float pixels   = 0.f;
    float distance = 0.f;
    CHECK_FALSE(ScreenDistanceToSegment(ray, glm::vec2(kCenter), glm::vec3(-3.f, 0.f, 10.f),
                                        glm::vec3(3.f, 0.f, 10.f), pixels, distance));
}

TEST_CASE("ScreenDistanceToSegment: a segment through the eye plane keeps only its visible half")
{
    const PickRay ray = Camera();

    // Runs from behind the camera and above it, down to a point dead ahead. The
    // endpoint behind must be off the camera's axis: one *on* the axis projects to
    // the screen centre whether its w is divided through as positive or negative,
    // so it would hide the very error this is about.
    const glm::vec3 behind(0.f, 20.f, 50.f);
    const glm::vec3 front(0.f, 0.f, -10.f);

    // Crossing the eye plane at y = 20/6, the visible half runs from far above the
    // viewport down to the centre, where `front` lands. So: on the line anywhere
    // above centre...
    float pixels   = 0.f;
    float distance = 0.f;
    REQUIRE(ScreenDistanceToSegment(ray, glm::vec2(kCenter, kCenter * 0.5f), behind, front, pixels, distance));
    CHECK(pixels < kSubPixel);

    // ...and nowhere below it. Unclipped, `behind` divides by a negative w and
    // lands *below* centre at 0.7 of the viewport height — putting a clickable
    // stretch of line in the lower half of the screen, where the segment does not
    // go and nothing is drawn.
    const glm::vec2 mirroredEnd(kCenter, 0.7f * kViewport);
    REQUIRE(ScreenDistanceToSegment(ray, mirroredEnd, behind, front, pixels, distance));
    CHECK(pixels > kOutlinePickPixels);
}

TEST_CASE("ScreenDistanceToSegment: depth along a foreshortened line is perspective-correct")
{
    const PickRay ray = Camera();

    // Nearly edge-on: the far end is a hundred metres out and the near end five,
    // so the far half of the line occupies a small fraction of the pixels it
    // spans. Reading the depth off the screen fraction is wrong by tens of metres
    // here, and right by construction on a line of even depth — which is why the
    // earlier cases cannot catch this.
    const glm::vec3 nearEnd(2.f, 0.f, -5.f);
    const glm::vec3 farEnd(2.f, 0.f, -100.f);

    // A point three quarters of the way along the segment in *world* terms.
    const glm::vec3 target = glm::mix(nearEnd, farEnd, 0.75f);

    float pixels   = 0.f;
    float distance = 0.f;
    REQUIRE(ScreenDistanceToSegment(ray, Screen(ray, target), nearEnd, farEnd, pixels, distance));

    CHECK(pixels < kSubPixel);
    CHECK(distance == doctest::Approx(glm::length(target)).epsilon(kEpsilon));
}

TEST_CASE("ScreenDistanceToSegment: an invalid ray picks nothing")
{
    PickRay ray = Camera();
    ray.valid   = false;

    float pixels   = 0.f;
    float distance = 0.f;
    CHECK_FALSE(ScreenDistanceToSegment(ray, glm::vec2(kCenter), glm::vec3(-1.f, 0.f, -10.f),
                                        glm::vec3(1.f, 0.f, -10.f), pixels, distance));

    // And a zero-size framebuffer, where there are no pixels to be near.
    ray.valid        = true;
    ray.viewportSize = glm::vec2(0.f);
    CHECK_FALSE(ScreenDistanceToSegment(ray, glm::vec2(kCenter), glm::vec3(-1.f, 0.f, -10.f),
                                        glm::vec3(1.f, 0.f, -10.f), pixels, distance));
}
