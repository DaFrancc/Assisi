/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <vector>

#include <Assisi/ECS/Scene.hpp>

using namespace Assisi::ECS;

namespace
{
struct Position
{
    float x = 0.0f;
};
struct Velocity
{
    float x = 0.0f;
};
} // namespace

TEST_CASE("Query: yields only entities that have every requested component")
{
    Scene scene;
    const Entity both = scene.Create();
    const Entity posOnly = scene.Create();
    const Entity velOnly = scene.Create();

    REQUIRE(scene.Add<Position>(both, {1.0f}) != nullptr);
    REQUIRE(scene.Add<Velocity>(both, {2.0f}) != nullptr);
    REQUIRE(scene.Add<Position>(posOnly, {3.0f}) != nullptr);
    REQUIRE(scene.Add<Velocity>(velOnly, {4.0f}) != nullptr);

    std::vector<Entity> seen;
    for (auto [e, pos, vel] : scene.Query<Position, Velocity>())
    {
        seen.push_back(e);
        pos.x += vel.x; // mutation through the query must stick
    }

    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == both);
    CHECK(scene.Get<Position>(both)->x == doctest::Approx(3.0f));
}

TEST_CASE("Query: a single-component query yields every holder")
{
    Scene scene;
    const Entity a = scene.Create();
    const Entity b = scene.Create();
    const Entity c = scene.Create();
    REQUIRE(scene.Add<Position>(a, {1.0f}) != nullptr);
    REQUIRE(scene.Add<Position>(b, {2.0f}) != nullptr);
    REQUIRE(scene.Add<Velocity>(c, {3.0f}) != nullptr); // c has no Position

    float sum = 0.0f;
    int count = 0;
    for (auto [e, pos] : scene.Query<Position>())
    {
        (void)e;
        sum += pos.x;
        ++count;
    }
    CHECK(count == 2);
    CHECK(sum == doctest::Approx(3.0f));
}

TEST_CASE("Query: reports every entity that has the full signature")
{
    Scene scene;
    const Entity a = scene.Create();
    const Entity b = scene.Create();
    REQUIRE(scene.Add<Position>(a, {1.0f}) != nullptr);
    REQUIRE(scene.Add<Velocity>(a, {1.0f}) != nullptr);
    REQUIRE(scene.Add<Position>(b, {1.0f}) != nullptr);
    REQUIRE(scene.Add<Velocity>(b, {1.0f}) != nullptr);

    std::vector<Entity> seen;
    for (auto [e, pos, vel] : scene.Query<Position, Velocity>())
    {
        (void)pos;
        (void)vel;
        seen.push_back(e);
    }
    REQUIRE(seen.size() == 2);
    CHECK((seen[0] == a || seen[1] == a));
    CHECK((seen[0] == b || seen[1] == b));
}

TEST_CASE("Query: empty when a requested pool has never been created")
{
    Scene scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add<Position>(e, {1.0f}) != nullptr);

    int count = 0;
    for (auto [ent, pos, vel] : scene.Query<Position, Velocity>())
    {
        (void)ent;
        (void)pos;
        (void)vel;
        ++count;
    }
    CHECK(count == 0);
}

TEST_CASE("Query: a destroyed entity is skipped even if the slot is reused")
{
    Scene scene;
    const Entity a = scene.Create();
    REQUIRE(scene.Add<Position>(a, {1.0f}) != nullptr);
    REQUIRE(scene.Add<Velocity>(a, {1.0f}) != nullptr);

    scene.Destroy(a);

    // Reuse the slot with a fresh entity that only has Position.
    const Entity b = scene.Create();
    CHECK(b.index == a.index);
    REQUIRE(scene.Add<Position>(b, {9.0f}) != nullptr);

    int count = 0;
    for (auto [e, pos, vel] : scene.Query<Position, Velocity>())
    {
        (void)e;
        (void)pos;
        (void)vel;
        ++count;
    }
    CHECK(count == 0); // stale 'a' must not match; 'b' lacks Velocity
}
