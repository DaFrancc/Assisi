/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Math/GLM.hpp>

#include <glm/gtc/matrix_transform.hpp>

using Assisi::Geometry::Aabb;
using Assisi::Geometry::TransformedAabb;

TEST_CASE("TransformedAabb: identity leaves the box unchanged")
{
    const Aabb local{.min = glm::vec3(-1.f, -2.f, -3.f), .max = glm::vec3(4.f, 5.f, 6.f)};
    const Aabb world = TransformedAabb(local, glm::mat4(1.f));
    CHECK(world.min.x == doctest::Approx(-1.f));
    CHECK(world.min.y == doctest::Approx(-2.f));
    CHECK(world.min.z == doctest::Approx(-3.f));
    CHECK(world.max.x == doctest::Approx(4.f));
    CHECK(world.max.y == doctest::Approx(5.f));
    CHECK(world.max.z == doctest::Approx(6.f));
}

TEST_CASE("TransformedAabb: translation shifts min and max together")
{
    const Aabb local{.min = glm::vec3(-1.f), .max = glm::vec3(1.f)};
    const glm::mat4 world = glm::translate(glm::mat4(1.f), glm::vec3(10.f, -5.f, 2.f));
    const Aabb moved = TransformedAabb(local, world);
    CHECK(moved.min.x == doctest::Approx(9.f));
    CHECK(moved.max.x == doctest::Approx(11.f));
    CHECK(moved.min.y == doctest::Approx(-6.f));
    CHECK(moved.max.y == doctest::Approx(-4.f));
    CHECK(moved.min.z == doctest::Approx(1.f));
    CHECK(moved.max.z == doctest::Approx(3.f));
}

TEST_CASE("TransformedAabb: uniform scale grows the extent about the scaled centre")
{
    const Aabb local{.min = glm::vec3(1.f, 1.f, 1.f), .max = glm::vec3(3.f, 3.f, 3.f)};      // centre (2,2,2)
    const glm::mat4 world = glm::scale(glm::mat4(1.f), glm::vec3(2.f));
    const Aabb scaled = TransformedAabb(local, world);
    // Centre 2 -> 4, half-extent 1 -> 2, so [2,6].
    CHECK(scaled.min.x == doctest::Approx(2.f));
    CHECK(scaled.max.x == doctest::Approx(6.f));
}

TEST_CASE("TransformedAabb: a 90-degree Z rotation swaps the X/Y extents")
{
    // A box wider in X than Y; after a 90 deg turn about Z the re-fit box is wider
    // in Y than X (the extents swap; exact because the rotation is axis-aligned).
    const Aabb local{.min = glm::vec3(-2.f, -1.f, -0.5f), .max = glm::vec3(2.f, 1.f, 0.5f)};
    const glm::mat4 world = glm::rotate(glm::mat4(1.f), glm::radians(90.f), glm::vec3(0.f, 0.f, 1.f));
    const Aabb turned = TransformedAabb(local, world);
    CHECK(turned.max.x == doctest::Approx(1.f)); // was the Y half-extent
    CHECK(turned.min.x == doctest::Approx(-1.f));
    CHECK(turned.max.y == doctest::Approx(2.f)); // was the X half-extent
    CHECK(turned.min.y == doctest::Approx(-2.f));
    CHECK(turned.max.z == doctest::Approx(0.5f)); // Z untouched
}
