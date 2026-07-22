/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/glm.hpp>

#include <Assisi/Runtime/LightingSystem.hpp>

using Assisi::Runtime::LightingSystem;

namespace
{
// Component-wise compare; the direction is normalized so an exact match is not
// expected, only agreement to float tolerance.
bool Approx3(const glm::vec3 &a, const glm::vec3 &b)
{
    return a.x == doctest::Approx(b.x) && a.y == doctest::Approx(b.y) && a.z == doctest::Approx(b.z);
}
} // namespace

// Round-6 M2 follow-on: a spot light's direction is local and must be rotated by
// the entity's world matrix, so a light parented to something that turns aims
// with it. Position already behaved this way; direction did not.
TEST_CASE("WorldSpotDirection rotates a spot's local aim into world space")
{
    const glm::vec3 forward{0.f, 0.f, -1.f};

    SUBCASE("identity leaves the local direction alone")
    {
        CHECK(Approx3(LightingSystem::WorldSpotDirection(glm::mat4(1.f), forward), forward));
    }

    SUBCASE("a yawed parent turns the beam with it")
    {
        // 90 degrees about +Y takes -Z to -X.
        const glm::mat4 yaw = glm::rotate(glm::mat4(1.f), glm::radians(90.f), glm::vec3(0.f, 1.f, 0.f));
        CHECK(Approx3(LightingSystem::WorldSpotDirection(yaw, forward), glm::vec3(-1.f, 0.f, 0.f)));
    }

    SUBCASE("translation alone does not steer the beam")
    {
        const glm::mat4 moved = glm::translate(glm::mat4(1.f), glm::vec3(10.f, -3.f, 7.f));
        CHECK(Approx3(LightingSystem::WorldSpotDirection(moved, forward), forward));
    }

    SUBCASE("the result is normalized despite scale in the matrix")
    {
        const glm::mat4 scaled = glm::scale(glm::mat4(1.f), glm::vec3(5.f));
        const glm::vec3 out    = LightingSystem::WorldSpotDirection(scaled, forward);
        CHECK(glm::length(out) == doctest::Approx(1.f));
        CHECK(Approx3(out, forward));
    }

    SUBCASE("a degenerate local direction falls back instead of producing NaN")
    {
        const glm::vec3 out = LightingSystem::WorldSpotDirection(glm::mat4(1.f), glm::vec3(0.f));
        CHECK(glm::length(out) == doctest::Approx(1.f));
        CHECK(out.x == out.x); // not NaN
    }
}
