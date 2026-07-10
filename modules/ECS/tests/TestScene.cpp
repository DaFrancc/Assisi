/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <type_traits>

#include <Assisi/ECS/Scene.hpp>

using namespace Assisi::ECS;

// Scene owns its component pools as raw pointers and frees them in ~Scene, so
// copying or moving one would double-free (or dangle) those pools. These guard
// the deleted special members — a regression here is a compile error, not a
// runtime crash to chase down later.
static_assert(!std::is_copy_constructible_v<Scene>, "Scene must not be copy-constructible");
static_assert(!std::is_copy_assignable_v<Scene>, "Scene must not be copy-assignable");
static_assert(!std::is_move_constructible_v<Scene>, "Scene must not be move-constructible");
static_assert(!std::is_move_assignable_v<Scene>, "Scene must not be move-assignable");

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

TEST_CASE("Scene: add / has / get / remove single-component lifecycle")
{
    Scene scene;
    const Entity e = scene.Create();

    CHECK_FALSE(scene.Has<Position>(e));
    CHECK(scene.Get<Position>(e) == nullptr);

    REQUIRE(scene.Add<Position>(e, {3.0f}) != nullptr);
    CHECK(scene.Has<Position>(e));
    REQUIRE(scene.Get<Position>(e) != nullptr);
    CHECK(scene.Get<Position>(e)->x == doctest::Approx(3.0f));

    scene.Remove<Position>(e);
    CHECK_FALSE(scene.Has<Position>(e));
    CHECK(scene.Get<Position>(e) == nullptr);
}

TEST_CASE("Scene: querying or removing a never-created pool is safe")
{
    Scene scene;
    const Entity e = scene.Create();

    CHECK_FALSE(scene.Has<Position>(e)); // pool never created
    CHECK(scene.Get<Position>(e) == nullptr);
    scene.Remove<Position>(e); // must not crash
}

TEST_CASE("Scene: destroy removes the entity from every pool it belongs to")
{
    Scene scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add<Position>(e, {1.0f}) != nullptr);
    REQUIRE(scene.Add<Velocity>(e, {2.0f}) != nullptr);

    scene.Destroy(e);
    CHECK_FALSE(scene.IsAlive(e));
    CHECK_FALSE(scene.Has<Position>(e));
    CHECK_FALSE(scene.Has<Velocity>(e));
}

TEST_CASE("Scene: clear resets entity ids and leaves pools reusable")
{
    Scene scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add<Position>(e, {5.0f}) != nullptr);

    scene.Clear();
    CHECK(scene.AliveCount() == 0);

    const Entity fresh = scene.Create();
    CHECK(fresh.index == 0);
    CHECK(fresh.generation == 0);
    CHECK(scene.Get<Position>(fresh) == nullptr); // old data was cleared

    REQUIRE(scene.Add<Position>(fresh, {7.0f}) != nullptr); // pool still usable
    CHECK(scene.Get<Position>(fresh)->x == doctest::Approx(7.0f));
}

// A stale handle must never add a component, even when the reused slot is still
// empty in that pool — the SparseSet-level check alone can't catch this (it only
// fires once the live occupant populates the same pool), so Scene::Add gates on
// liveness. Regression for the "Scene::Add does not check IsAlive" gap.
TEST_CASE("Scene: a stale handle cannot add to a reused slot")
{
    Scene scene;
    const Entity a = scene.Create();
    REQUIRE(scene.Add<Position>(a, {1.0f}) != nullptr);

    scene.Destroy(a);
    const Entity b = scene.Create(); // reuses a's slot, newer generation
    REQUIRE(b.index == a.index);
    REQUIRE(b.generation != a.generation);

    // b has not populated the Position pool yet, so a's old slot is free there.
    CHECK(scene.Add<Position>(a, {9.0f}) == nullptr); // rejected: a is dead

    CHECK_FALSE(scene.Has<Position>(b));
    REQUIRE(scene.Add<Position>(b, {5.0f}) != nullptr); // b adds cleanly
    CHECK(scene.Get<Position>(b)->x == doctest::Approx(5.0f));
}

TEST_CASE("Scene: adding a component twice is rejected, original kept")
{
    Scene scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add<Position>(e, {1.0f}) != nullptr);
    CHECK(scene.Add<Position>(e, {2.0f}) == nullptr);
    CHECK(scene.Get<Position>(e)->x == doctest::Approx(1.0f));
}
