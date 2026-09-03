/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/glm.hpp>

#include <Assisi/Runtime/LightComponents.hpp>
#include <Assisi/Runtime/LightingSystem.hpp>

#include <algorithm>
#include <limits>

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

// A spot light's direction is local and is rotated by the entity's world matrix,
// so a light parented to something that turns aims with it — the same rule its
// position follows.
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

TEST_CASE("A daylight cycle turns the sun about world up")
{
    using Assisi::Runtime::AdvanceDaylight;
    using Assisi::Runtime::DirectionalLight;

    DirectionalLight light;
    light.daylightCycle = true;
    light.daylightPeriodSeconds = 100.f;
    // Aimed down and to one side: a sun straight down is parallel to the axis it
    // turns about, which is the one aim a day cannot move.
    light.direction = glm::normalize(glm::vec3(1.f, -1.f, 0.f));

    SUBCASE("the default sun, aimed straight down, still moves")
    {
        // The case that made this look broken: a new DirectionalLight points
        // straight down, and turning about world *up* leaves that aim exactly
        // where it is — the aim is the axis. A day has to move the sun a author
        // has not touched, because that is every sun on the first run.
        DirectionalLight fresh;
        fresh.daylightCycle = true;
        fresh.daylightPeriodSeconds = 100.f;
        CHECK(fresh.direction == glm::vec3(0.f, -1.f, 0.f));

        const glm::vec3 turned = AdvanceDaylight(fresh, 25.f);
        CHECK(glm::length(turned) == doctest::Approx(1.f));
        CHECK_FALSE(Approx3(turned, fresh.direction));
    }

    SUBCASE("the sun sets, which is what makes it a day rather than a circuit")
    {
        // Turning about world up sweeps the sun around at whatever elevation it
        // was authored at and never takes it below the horizon — a polar summer,
        // with no night in it. Over a whole cycle the vertical component has to
        // reach both signs: light travelling downward is day, upward is night.
        DirectionalLight sun;
        sun.daylightCycle = true;
        sun.daylightPeriodSeconds = 100.f;
        sun.direction = glm::vec3(0.f, -1.f, 0.f);

        float lowest = 1.f;
        float highest = -1.f;
        for (int step = 0; step < 16; ++step)
        {
            const float t = static_cast<float>(step) * (sun.daylightPeriodSeconds / 16.f);
            const float vertical = AdvanceDaylight(sun, t).y;
            lowest = std::min(lowest, vertical);
            highest = std::max(highest, vertical);
        }
        CHECK(lowest < -0.5f); // overhead, shining down
        CHECK(highest > 0.5f); // under the world, shining up — night
    }

    SUBCASE("a whole period comes back to where it started")
    {
        const glm::vec3 turned = AdvanceDaylight(light, light.daylightPeriodSeconds);
        CHECK(Approx3(turned, light.direction));
    }

    SUBCASE("half a period is the opposite aim, about the horizon axis")
    {
        // Turning half a revolution about world X reverses the two components
        // perpendicular to it and leaves the one along it alone.
        const glm::vec3 turned = AdvanceDaylight(light, light.daylightPeriodSeconds * 0.5f);
        CHECK(turned.x == doctest::Approx(light.direction.x));
        CHECK(turned.y == doctest::Approx(-light.direction.y));
        CHECK(turned.z == doctest::Approx(-light.direction.z));
    }

    SUBCASE("the cycle off leaves the aim exactly as authored")
    {
        // Not merely close: an author who switched the cycle off expects the
        // direction they typed, and a rotation by zero that renormalises would
        // still perturb the last bit.
        DirectionalLight fixed = light;
        fixed.daylightCycle = false;
        const glm::vec3 turned = AdvanceDaylight(fixed, 12.5f);
        CHECK(turned == fixed.direction);
    }

    SUBCASE("an absurd period is floored rather than divided by")
    {
        // A level file is hand-editable and this one divides. Zero would be a
        // sun with no day at all; the floor turns it into the fastest day the
        // cycle will run instead of a NaN that reaches the cascade fit.
        DirectionalLight strobe = light;
        strobe.daylightPeriodSeconds = 0.f;
        const glm::vec3 turned = AdvanceDaylight(strobe, Assisi::Runtime::kMinDaylightPeriodSeconds);
        CHECK(glm::length(turned) == doctest::Approx(1.f));
        // A whole revolution at the floor, so it lands back where it started.
        CHECK(Approx3(turned, strobe.direction));
    }

    SUBCASE("a non-finite step leaves the aim alone")
    {
        const glm::vec3 turned = AdvanceDaylight(light, std::numeric_limits<float>::quiet_NaN());
        CHECK(turned == light.direction);
    }
}
