/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestQueryMut.cpp
/// @brief Scene::QueryMut and its Mut<T> write proxies: which access spellings
/// stamp change detection, which deliberately do not, and the hazard QueryMut
/// exists to close (a tracked write through a plain Query is invisible).

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/TestComponents.hpp>

using namespace Assisi::ECS;

namespace
{

/// Takes its argument by mutable reference, so passing a Mut<Tracked> to it
/// exercises the implicit `operator T&` conversion (which stamps).
void Bump(Tracked &tracked)
{
    ++tracked.value;
}

/// Takes its argument by const reference — a Mut<Tracked> reaches it through
/// the const conversion path only when the proxy itself is const.
int32_t Read(const Tracked &tracked)
{
    return tracked.value;
}

} // namespace

TEST_CASE("QueryMut: a tracked component written through the proxy reports Changed")
{
    Scene scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add<Tracked>(e, {1}) != nullptr);

    // Bookmark after the Add's own stamp, so only the loop's write can move it.
    const uint64_t bookmark = scene.CurrentChangeTick();
    REQUIRE_FALSE(scene.Changed<Tracked>(e, bookmark));

    for (auto [entity, tracked] : scene.QueryMut<Tracked>())
    {
        CHECK(entity == e);
        tracked->value += 41; // operator-> : the canonical write spelling
    }

    CHECK(scene.Get<Tracked>(e)->value == 42); // the write landed
    CHECK(scene.Changed<Tracked>(e, bookmark)); // and was recorded
    CHECK(scene.CurrentChangeTick() > bookmark);
}

TEST_CASE("QueryMut: every mutable spelling stamps")
{
    Scene scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add<Tracked>(e, {0}) != nullptr);

    SUBCASE("operator*")
    {
        const uint64_t bookmark = scene.CurrentChangeTick();
        for (auto [entity, tracked] : scene.QueryMut<Tracked>())
        {
            (void)entity;
            (*tracked).value = 7;
        }
        CHECK(scene.Get<Tracked>(e)->value == 7);
        CHECK(scene.Changed<Tracked>(e, bookmark));
    }

    SUBCASE("implicit conversion to T&")
    {
        const uint64_t bookmark = scene.CurrentChangeTick();
        for (auto [entity, tracked] : scene.QueryMut<Tracked>())
        {
            (void)entity;
            Bump(tracked); // binds Tracked& through operator T&
        }
        CHECK(scene.Get<Tracked>(e)->value == 1);
        CHECK(scene.Changed<Tracked>(e, bookmark));
    }

    SUBCASE("GetMut()")
    {
        const uint64_t bookmark = scene.CurrentChangeTick();
        for (auto [entity, tracked] : scene.QueryMut<Tracked>())
        {
            (void)entity;
            tracked.GetMut().value = 9;
        }
        CHECK(scene.Get<Tracked>(e)->value == 9);
        CHECK(scene.Changed<Tracked>(e, bookmark));
    }

    SUBCASE("MarkChanged() stamps without touching the value")
    {
        const uint64_t bookmark = scene.CurrentChangeTick();
        for (auto [entity, tracked] : scene.QueryMut<Tracked>())
        {
            (void)entity;
            tracked.MarkChanged();
        }
        CHECK(scene.Get<Tracked>(e)->value == 0);
        CHECK(scene.Changed<Tracked>(e, bookmark));
    }
}

TEST_CASE("QueryMut: const access through the proxy does not stamp")
{
    Scene scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add<Tracked>(e, {5}) != nullptr);

    const uint64_t bookmark = scene.CurrentChangeTick();

    int32_t sum = 0;
    for (auto [entity, tracked] : scene.QueryMut<Tracked>())
    {
        (void)entity;
        sum += tracked.Get().value;                  // Get() const
        const Mut<Tracked> &readOnly = tracked;      // const proxy: only const accessors are viable
        sum += readOnly->value;                      // operator-> const
        sum += (*readOnly).value;                    // operator* const
        sum += Read(readOnly);                       // operator const T& const
    }

    CHECK(sum == 20);
    CHECK_FALSE(scene.Changed<Tracked>(e, bookmark)); // pure reads stayed invisible
    CHECK(scene.CurrentChangeTick() == bookmark);     // and burned no tick
}

TEST_CASE("QueryMut: the same write through a plain Query is a missed change")
{
    // This documents the hazard QueryMut exists to close. Plain Query hands out
    // raw Ts& and cannot tell a read from a write, so a tracked write through it
    // is silently invisible to Changed() — exactly like writing through Get<T>.
    Scene scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add<Tracked>(e, {1}) != nullptr);

    const uint64_t bookmark = scene.CurrentChangeTick();

    for (auto [entity, tracked] : scene.Query<Tracked>())
    {
        (void)entity;
        tracked.value += 41;
    }

    CHECK(scene.Get<Tracked>(e)->value == 42);        // the write landed…
    CHECK_FALSE(scene.Changed<Tracked>(e, bookmark)); // …and nothing recorded it
    CHECK(scene.CurrentChangeTick() == bookmark);
}

TEST_CASE("QueryMut: exclusions yield the same entity set as Query")
{
    Scene scene;
    const Entity plain   = scene.Create();
    const Entity tagged  = scene.Create();
    const Entity another = scene.Create();

    REQUIRE(scene.Add<Tracked>(plain, {1}) != nullptr);
    REQUIRE(scene.Add<Tracked>(tagged, {2}) != nullptr);
    REQUIRE(scene.Add<Tag>(tagged) != nullptr);
    REQUIRE(scene.Add<Tracked>(another, {3}) != nullptr);

    std::vector<Entity> fromQuery;
    for (auto [entity, tracked] : scene.Query<Tracked>(Without<Tag>{}))
    {
        (void)tracked;
        fromQuery.push_back(entity);
    }

    std::vector<Entity> fromQueryMut;
    for (auto [entity, tracked] : scene.QueryMut<Tracked>(Without<Tag>{}))
    {
        fromQueryMut.push_back(entity);
        tracked->value += 10;
    }

    CHECK(fromQuery == fromQueryMut);
    REQUIRE(fromQueryMut.size() == 2);
    CHECK(scene.Get<Tracked>(plain)->value == 11);
    CHECK(scene.Get<Tracked>(another)->value == 13);
    CHECK(scene.Get<Tracked>(tagged)->value == 2); // excluded, untouched
}

TEST_CASE("QueryMut: an untracked component behaves like a plain reference and burns no tick")
{
    Scene scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add<Position>(e, {1.0f}) != nullptr);

    // Position is not ACOMP(tracked): its pool has no tick lane, so the proxy's
    // TracksChanges() gate short-circuits and the scene tick never advances —
    // the same "untracked pays nothing" contract Scene::GetMut honours.
    CHECK(scene.CurrentChangeTick() == 0);

    for (auto [entity, pos] : scene.QueryMut<Position>())
    {
        (void)entity;
        pos->x += 4.0f;
    }

    CHECK(scene.Get<Position>(e)->x == doctest::Approx(5.0f)); // the write still lands
    CHECK(scene.ChangeTick<Position>(e) == 0);
    CHECK(scene.CurrentChangeTick() == 0);
}

TEST_CASE("QueryMut: a mixed signature stamps only the tracked pool")
{
    Scene scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add<Tracked>(e, {1}) != nullptr);
    REQUIRE(scene.Add<Position>(e, {1.0f}) != nullptr);

    const uint64_t bookmark = scene.CurrentChangeTick();

    for (auto [entity, tracked, pos] : scene.QueryMut<Tracked, Position>())
    {
        (void)entity;
        tracked->value += 1;
        pos->x += 1.0f;
    }

    CHECK(scene.Changed<Tracked>(e, bookmark));
    CHECK(scene.ChangeTick<Position>(e) == 0);
    // Exactly one tick was allocated: the untracked write must not consume one.
    CHECK(scene.CurrentChangeTick() == bookmark + 1);
}

TEST_CASE("QueryMut: a missing pool yields nothing and stamps nothing")
{
    Scene scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add<Tracked>(e, {1}) != nullptr);

    // Velocity has never been added in this scene, so its pool does not exist and
    // the intersection is empty — the view must be inert, not crash on a null pool.
    const uint64_t bookmark = scene.CurrentChangeTick();
    int32_t count = 0;
    for (auto [entity, tracked, vel] : scene.QueryMut<Tracked, Velocity>())
    {
        (void)entity;
        (void)tracked;
        (void)vel;
        ++count;
    }

    CHECK(count == 0);
    CHECK(scene.CurrentChangeTick() == bookmark);
}

TEST_CASE("QueryMut: stamping is per entity, not per pool")
{
    Scene scene;
    const Entity a = scene.Create();
    const Entity b = scene.Create();
    REQUIRE(scene.Add<Tracked>(a, {1}) != nullptr);
    REQUIRE(scene.Add<Tracked>(b, {2}) != nullptr);
    REQUIRE(scene.Add<Tag>(b) != nullptr);

    const uint64_t bookmark = scene.CurrentChangeTick();

    // Write only the untagged half; b must stay unchanged so a consumer filtering
    // on Changed() processes exactly the entities the loop touched.
    for (auto [entity, tracked] : scene.QueryMut<Tracked>(Without<Tag>{}))
    {
        (void)entity;
        tracked->value = 100;
    }

    CHECK(scene.Changed<Tracked>(a, bookmark));
    CHECK_FALSE(scene.Changed<Tracked>(b, bookmark));
}
