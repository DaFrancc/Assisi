/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestQueryWrites.cpp
/// @brief What a query lets a loop write: a plain element is const, a Mut<T>
/// element is a T& stamped for every entity the loop visits, and only the Mut
/// elements stamp.

#include <doctest/doctest.h>

#include <cstdint>
#include <tuple>
#include <type_traits>
#include <vector>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/TestComponents.hpp>

using namespace Assisi::ECS;

namespace
{

template <typename... Es> concept CanQuery = requires(Scene &scene) {
    scene.Query<Es...>();
};

template <typename... Es> concept CanQueryConst = requires(const Scene &scene) {
    scene.Query<Es...>();
};

template <typename X> concept CanExclude = requires(Scene &scene) {
    scene.Query<Tracked, Without<X>>();
};

/// What one step of `Query<Es...>()` yields.
template <typename... Es>
using YieldOf = decltype(*std::declval<QueryView<std::tuple<Es...>, std::tuple<>> &>().begin());

} // namespace

static_assert(std::is_same_v<YieldOf<Tracked, Mut<TrackedToo>>, std::tuple<Entity, const Tracked &, TrackedToo &>>,
              "a plain element is read-only and a Mut element is writable");
static_assert(std::is_same_v<YieldOf<Tracked>, std::tuple<Entity, const Tracked &>>,
              "a query of plain elements writes nothing");

static_assert(CanQueryConst<Tracked, Position>, "a const Scene can be read through a query");
static_assert(!CanQueryConst<Mut<Tracked>>, "a const Scene cannot be written through a query");
static_assert(!CanQuery<Tracked, Mut<Tracked>>, "a component is named once per query");
static_assert(!CanExclude<Mut<Tag>>, "an excluded component is never yielded, so it cannot be Mut");
static_assert(CanExclude<Tag>);

TEST_CASE("Query: writing a Mut element reports Changed, reading a plain one beside it does not")
{
    Scene scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add<Tracked>(e, {1}) != nullptr);
    REQUIRE(scene.Add<TrackedToo>(e, {2}) != nullptr);

    const uint64_t bookmark = scene.CurrentChangeTick();

    for (auto [entity, read, written] : scene.Query<Tracked, Mut<TrackedToo>>())
    {
        CHECK(entity == e);
        written.value += read.value;
    }

    CHECK(scene.Get<TrackedToo>(e)->value == 3);
    CHECK(scene.Changed<TrackedToo>(e, bookmark));
    CHECK_FALSE(scene.Changed<Tracked>(e, bookmark));
    CHECK(scene.CurrentChangeTick() == bookmark + 1);
}

TEST_CASE("Query: a Mut element is stamped for every entity the loop visits")
{
    Scene scene;
    const Entity a = scene.Create();
    const Entity b = scene.Create();
    REQUIRE(scene.Add<Tracked>(a, {1}) != nullptr);
    REQUIRE(scene.Add<Tracked>(b, {2}) != nullptr);

    const uint64_t bookmark = scene.CurrentChangeTick();

    // b is visited and left alone; it is marked all the same. A loop that
    // changes only some of what it visits reads with a plain element and
    // writes those few through GetMut.
    for (auto [entity, tracked] : scene.Query<Mut<Tracked>>())
    {
        if (entity == a)
        {
            tracked.value = 10;
        }
    }

    CHECK(scene.Changed<Tracked>(a, bookmark));
    CHECK(scene.Changed<Tracked>(b, bookmark));
}

TEST_CASE("Query: plain elements burn no tick")
{
    Scene scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add<Tracked>(e, {5}) != nullptr);
    REQUIRE(scene.Add<TrackedToo>(e, {6}) != nullptr);

    const uint64_t bookmark = scene.CurrentChangeTick();

    int32_t sum = 0;
    for (auto [entity, tracked, too] : scene.Query<Tracked, TrackedToo>())
    {
        (void)entity;
        sum += tracked.value + too.value;
    }

    CHECK(sum == 11);
    CHECK(scene.CurrentChangeTick() == bookmark);
}

TEST_CASE("Query: a const Scene yields the same entities")
{
    Scene scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add<Tracked>(e, {7}) != nullptr);

    const Scene &readOnly = scene;
    std::vector<Entity> seen;
    for (auto [entity, tracked] : readOnly.Query<Tracked>())
    {
        CHECK(tracked.value == 7);
        seen.push_back(entity);
    }

    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == e);
}

TEST_CASE("Query: exclusions apply to Mut elements, and an excluded entity is not stamped")
{
    Scene scene;
    const Entity plain = scene.Create();
    const Entity tagged = scene.Create();
    REQUIRE(scene.Add<Tracked>(plain, {1}) != nullptr);
    REQUIRE(scene.Add<Tracked>(tagged, {2}) != nullptr);
    REQUIRE(scene.Add<Tag>(tagged) != nullptr);

    const uint64_t bookmark = scene.CurrentChangeTick();

    std::vector<Entity> seen;
    for (auto [entity, tracked] : scene.Query<Mut<Tracked>, Without<Tag>>())
    {
        seen.push_back(entity);
        tracked.value += 10;
    }

    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == plain);
    CHECK(scene.Get<Tracked>(plain)->value == 11);
    CHECK(scene.Get<Tracked>(tagged)->value == 2);
    CHECK(scene.Changed<Tracked>(plain, bookmark));
    CHECK_FALSE(scene.Changed<Tracked>(tagged, bookmark));
}

TEST_CASE("Query: a Mut element of an untracked component burns no tick")
{
    Scene scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add<Position>(e, {1.0f}) != nullptr);
    REQUIRE(scene.Add<Tracked>(e, {1}) != nullptr);

    const uint64_t bookmark = scene.CurrentChangeTick();

    for (auto [entity, pos, tracked] : scene.Query<Mut<Position>, Mut<Tracked>>())
    {
        (void)entity;
        pos.x += 4.0f;
        tracked.value += 1;
    }

    CHECK(scene.Get<Position>(e)->x == doctest::Approx(5.0f));
    CHECK(scene.ChangeTick<Position>(e) == 0);
    CHECK(scene.Changed<Tracked>(e, bookmark));
    // One tick, for Tracked: the untracked Position must not consume one.
    CHECK(scene.CurrentChangeTick() == bookmark + 1);
}

TEST_CASE("Query: a missing pool yields nothing and stamps nothing")
{
    Scene scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add<Tracked>(e, {1}) != nullptr);

    // No Velocity was ever added, so its pool does not exist.
    const uint64_t bookmark = scene.CurrentChangeTick();
    int32_t count = 0;
    for (auto [entity, tracked, vel] : scene.Query<Mut<Tracked>, Velocity>())
    {
        (void)entity;
        (void)tracked;
        (void)vel;
        ++count;
    }

    CHECK(count == 0);
    CHECK(scene.CurrentChangeTick() == bookmark);
}
