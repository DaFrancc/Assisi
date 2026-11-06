/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/ShadowView.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

using namespace Assisi::Render;

namespace
{
/// The sphere the convex triangles are cut from, and how many rings and
/// segments of it the tests walk.
constexpr float kSphereRadius = 0.5f;
constexpr std::uint32_t kRings = 6;
constexpr std::uint32_t kSegments = 12;

/// Steps along each edge of the barycentric grid a triangle is sampled on.
constexpr int kGridSteps = 6;

/// How close two lifts must agree, in the sphere's units: a few float roundings
/// of numbers about the sphere's size.
constexpr float kLiftTolerance = 1e-5f;

/// Hanika's construction as published: each corner lifts the point by however
/// far it lies below that corner's tangent plane, never down, and the lifts
/// blend as the corners do.
glm::vec3 HanikaLift(const ShadedTriangle &triangle, const glm::vec3 &barycentric)
{
    glm::vec3 point(0.f);
    for (std::size_t corner = 0; corner < triangle.corners.size(); ++corner)
    {
        point += barycentric[static_cast<glm::length_t>(corner)] * triangle.corners[corner];
    }
    glm::vec3 lift(0.f);
    for (std::size_t corner = 0; corner < triangle.corners.size(); ++corner)
    {
        const glm::vec3 normal = glm::normalize(triangle.normals[corner]);
        const float below = std::max(glm::dot(triangle.corners[corner] - point, normal), 0.f);
        lift += barycentric[static_cast<glm::length_t>(corner)] * below * normal;
    }
    return lift;
}

/// A point on the sphere at @p ring (from the top) and @p segment.
glm::vec3 SpherePoint(std::uint32_t ring, std::uint32_t segment)
{
    const float polar = glm::pi<float>() * static_cast<float>(ring) / static_cast<float>(kRings);
    const float azimuth = glm::two_pi<float>() * static_cast<float>(segment) / static_cast<float>(kSegments);
    return kSphereRadius *
           glm::vec3(std::sin(polar) * std::cos(azimuth), std::cos(polar), std::sin(polar) * std::sin(azimuth));
}

/// The triangle of a UV sphere between rings @p ring and @p ring + 1, with each
/// corner's normal pointing out from the centre, set off from the origin by
/// @p centre as a model away from the camera would be.
ShadedTriangle SphereTriangle(std::uint32_t ring, std::uint32_t segment, const glm::vec3 &centre)
{
    ShadedTriangle triangle;
    triangle.corners = {SpherePoint(ring, segment), SpherePoint(ring + 1, segment), SpherePoint(ring + 1, segment + 1)};
    for (std::size_t corner = 0; corner < triangle.corners.size(); ++corner)
    {
        triangle.normals[corner] = glm::normalize(triangle.corners[corner]);
        triangle.corners[corner] += centre;
    }
    return triangle;
}

glm::vec3 Barycentric(int i, int j)
{
    const float steps = static_cast<float>(kGridSteps);
    return glm::vec3(static_cast<float>(i) / steps, static_cast<float>(j) / steps,
                     static_cast<float>(kGridSteps - i - j) / steps);
}

/// The triangle's own normal, turned away from the origin: outward for a
/// triangle of a sphere centred there.
glm::vec3 OutwardFaceNormal(const ShadedTriangle &triangle)
{
    const glm::vec3 face =
        glm::normalize(glm::cross(triangle.corners[1] - triangle.corners[0], triangle.corners[2] - triangle.corners[0]));
    return glm::dot(face, triangle.corners[0]) < 0.f ? -face : face;
}
} // namespace

TEST_CASE("SmoothSurfaceLift puts the whole of a sphere behind every lifted point's plane")
{
    // What keeps a convex mesh from shadowing itself: the plane through the
    // lifted point, facing the interpolated normal, which the lookup's receiver
    // plane turns to, has every facet of the sphere behind it, since they all
    // lie inside the sphere. Away from the origin, as a model away from the
    // camera is, to keep the difference the fragment takes honest.
    const glm::vec3 centre(3.f, 1.f, -4.f);
    for (std::uint32_t ring = 1; ring + 1 < kRings; ++ring)
    {
        for (std::uint32_t segment = 0; segment < kSegments; ++segment)
        {
            const ShadedTriangle triangle = SphereTriangle(ring, segment, centre);
            for (int i = 0; i <= kGridSteps; ++i)
            {
                for (int j = 0; i + j <= kGridSteps; ++j)
                {
                    CAPTURE(ring);
                    CAPTURE(segment);
                    CAPTURE(i);
                    CAPTURE(j);
                    const glm::vec3 barycentric = Barycentric(i, j);
                    const glm::vec3 normal = glm::normalize(barycentric.x * triangle.normals[0] +
                                                            barycentric.y * triangle.normals[1] +
                                                            barycentric.z * triangle.normals[2]);
                    const glm::vec3 point = barycentric.x * triangle.corners[0] + barycentric.y * triangle.corners[1] +
                                            barycentric.z * triangle.corners[2];
                    const glm::vec3 lift = SmoothSurfaceLift(triangle, barycentric, normal);
                    CHECK(glm::dot(point + lift - centre, normal) > kSphereRadius - kLiftTolerance);
                }
            }
        }
    }
}

TEST_CASE("SmoothSurfaceLift lifts a convex triangle at least as far as Hanika's construction")
{
    // Hanika's projects the point onto each corner's tangent plane and blends
    // the projections; it needs the corners one by one, which a fragment does
    // not have. The lift here blends the same per-corner depths but takes them
    // all along the interpolated normal, which is never shorter.
    for (std::uint32_t ring = 1; ring + 1 < kRings; ++ring)
    {
        for (std::uint32_t segment = 0; segment < kSegments; ++segment)
        {
            const ShadedTriangle triangle = SphereTriangle(ring, segment, glm::vec3(0.f));
            for (int i = 0; i <= kGridSteps; ++i)
            {
                for (int j = 0; i + j <= kGridSteps; ++j)
                {
                    CAPTURE(ring);
                    CAPTURE(segment);
                    CAPTURE(i);
                    CAPTURE(j);
                    const glm::vec3 barycentric = Barycentric(i, j);
                    const glm::vec3 facing = OutwardFaceNormal(triangle);
                    const float lift = glm::length(SmoothSurfaceLift(triangle, barycentric, facing));
                    CHECK(lift > glm::length(HanikaLift(triangle, barycentric)) - kLiftTolerance);
                }
            }
        }
    }
}

TEST_CASE("SmoothSurfaceLift lifts a facet out to the sphere it was cut from, and past it")
{
    // Inside a triangle the curve stands outside the sphere its corners lie on,
    // by about the facet's own gap again, so a lifted point is clear of the
    // neighbouring facets, which are inside the sphere.
    const ShadedTriangle triangle = SphereTriangle(2, 0, glm::vec3(0.f));
    const glm::vec3 barycentric(1.f / 3.f, 1.f / 3.f, 1.f / 3.f);
    const glm::vec3 point = (triangle.corners[0] + triangle.corners[1] + triangle.corners[2]) / 3.f;
    const glm::vec3 lift = SmoothSurfaceLift(triangle, barycentric, glm::normalize(point));
    CHECK(glm::length(point + lift) > kSphereRadius);
    CHECK(glm::dot(lift, OutwardFaceNormal(triangle)) > 0.f);
}

TEST_CASE("SmoothSurfaceLift is zero at the corners and on a flat-shaded triangle")
{
    const ShadedTriangle curved = SphereTriangle(2, 3, glm::vec3(0.f));
    for (std::size_t corner = 0; corner < curved.corners.size(); ++corner)
    {
        glm::vec3 barycentric(0.f);
        barycentric[static_cast<glm::length_t>(corner)] = 1.f;
        CHECK(glm::length(SmoothSurfaceLift(curved, barycentric, curved.normals[corner])) < kLiftTolerance);
    }

    ShadedTriangle flat = curved;
    const glm::vec3 face = OutwardFaceNormal(flat);
    flat.normals = {face, face, face};
    CHECK(glm::length(SmoothSurfaceLift(flat, Barycentric(2, 2), face)) < kLiftTolerance);
}

TEST_CASE("SmoothSurfaceLift does not lift a triangle that curves inward")
{
    // The inside of a bowl: the curve its normals describe sinks below the
    // triangle, and a lookup moved down would leave the bowl's own shadow.
    ShadedTriangle bowl = SphereTriangle(2, 1, glm::vec3(0.f));
    for (glm::vec3 &normal : bowl.normals)
    {
        normal = -normal;
    }
    const glm::vec3 facing = -OutwardFaceNormal(bowl);
    CHECK(SmoothSurfaceLift(bowl, Barycentric(2, 2), facing) == glm::vec3(0.f));
}

TEST_CASE("TerminatorGate lifts fully where the triangle faces away and not at all well inside the light")
{
    const glm::vec3 toLight(0.f, 1.f, 0.f);
    // The interpolated normal just clears the horizon; the triangle is past it.
    const glm::vec3 normal = glm::normalize(glm::vec3(1.f, 0.05f, 0.f));
    const glm::vec3 facet = glm::normalize(glm::vec3(1.f, -0.05f, 0.f));
    CHECK(TerminatorGate(normal, facet, toLight) == 1.f);

    // Both well inside the light.
    const glm::vec3 lit = glm::normalize(glm::vec3(1.f, 1.f, 0.f));
    CHECK(TerminatorGate(lit, lit, toLight) == 0.f);
}

TEST_CASE("TerminatorGate fades in without a step")
{
    // Walking a surface toward the edge of the light, with the triangle a fixed
    // tilt behind the interpolated normal, the gate never jumps.
    constexpr int kSteps = 1000;
    constexpr float kTiltRadians = 0.05f;
    constexpr float kLargestStep = 0.05f;
    const glm::vec3 toLight(0.f, 1.f, 0.f);
    float previous = -1.f;
    for (int step = 0; step <= kSteps; ++step)
    {
        const float elevation = glm::half_pi<float>() * (1.f - static_cast<float>(step) / static_cast<float>(kSteps));
        const glm::vec3 normal(std::cos(elevation), std::sin(elevation), 0.f);
        const glm::vec3 facet(std::cos(elevation - kTiltRadians), std::sin(elevation - kTiltRadians), 0.f);
        const float gate = TerminatorGate(normal, facet, toLight);
        CAPTURE(step);
        CHECK(gate >= 0.f);
        CHECK(gate <= 1.f);
        if (previous >= 0.f)
        {
            CHECK(gate >= previous);
            CHECK(gate - previous < kLargestStep);
        }
        previous = gate;
    }
    CHECK(previous == 1.f);
}

namespace
{
/// A light @p elevation radians above the horizon of a surface facing +Y, coming
/// from +X.
glm::vec3 LightAt(float elevation)
{
    return glm::vec3(std::cos(elevation), std::sin(elevation), 0.f);
}

/// A bump's normal: +Y tilted @p tilt radians toward +X, toward the light.
glm::vec3 BumpTowardLight(float tilt)
{
    return glm::vec3(std::sin(tilt), std::cos(tilt), 0.f);
}

/// Steps the light takes from overhead to the horizon in the fade tests.
constexpr int kElevationSteps = 1000;
/// The tilt of the bump the fade tests light: steep for a normal map.
constexpr float kBumpTiltRadians = 0.4f;
/// The most one step of the light may change what the surface keeps.
constexpr float kLargestFadeStep = 0.02f;
} // namespace

TEST_CASE("BumpShadowing leaves a surface without a normal map all of its light")
{
    const glm::vec3 up(0.f, 1.f, 0.f);
    for (int step = 0; step < kElevationSteps; ++step)
    {
        const float elevation = glm::half_pi<float>() * static_cast<float>(step + 1) / kElevationSteps;
        CAPTURE(step);
        CHECK(BumpShadowing(up, up, LightAt(elevation)) == 1.f);
    }
}

TEST_CASE("BumpShadowing gives a bump no light once the surface under it faces away")
{
    // The bump still faces the light; the surface, and the shadow map with it,
    // does not.
    const glm::vec3 up(0.f, 1.f, 0.f);
    const glm::vec3 bump = BumpTowardLight(kBumpTiltRadians);
    CHECK(BumpShadowing(up, bump, LightAt(0.f)) == 0.f);
    CHECK(BumpShadowing(up, bump, LightAt(-0.1f)) == 0.f);
    CHECK(glm::dot(bump, LightAt(-0.1f)) > 0.f);
}

TEST_CASE("BumpShadowing fades a bump's light out toward the surface's horizon without a step")
{
    // Lowering the light from overhead to the horizon, what the bump keeps
    // never rises, never jumps, stays between 0 and 1, and is all of it while
    // the light is high.
    const glm::vec3 up(0.f, 1.f, 0.f);
    const glm::vec3 bump = BumpTowardLight(kBumpTiltRadians);
    float previous = 1.f;
    for (int step = 0; step <= kElevationSteps; ++step)
    {
        const float elevation = glm::half_pi<float>() * (1.f - static_cast<float>(step) / kElevationSteps);
        const float kept = BumpShadowing(up, bump, LightAt(elevation));
        CAPTURE(step);
        CHECK(kept >= 0.f);
        CHECK(kept <= 1.f);
        CHECK(kept <= previous);
        CHECK(previous - kept < kLargestFadeStep);
        previous = kept;
    }
    CHECK(previous == 0.f);
    CHECK(BumpShadowing(up, bump, LightAt(glm::half_pi<float>())) == 1.f);
}

TEST_CASE("BumpShadowing leaves a bump tilted away from the light all of it")
{
    // Such a bump gains nothing on the surface, so there is nothing to fade;
    // its own shading already darkens it.
    const glm::vec3 up(0.f, 1.f, 0.f);
    const glm::vec3 away = BumpTowardLight(-kBumpTiltRadians);
    constexpr float kLowLight = 0.6f;
    CHECK(BumpShadowing(up, away, LightAt(kLowLight)) == 1.f);
}
