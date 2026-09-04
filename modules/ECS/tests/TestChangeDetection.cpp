/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestChangeDetection.cpp
/// @brief Change-tick behaviour for ACOMP(tracked) components: Add/GetMut stamp,
/// Get does not, Changed() transitions, MarkChanged, and untracked control.

#include <doctest/doctest.h>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/TestComponents.hpp>

#include <vector>

using namespace Assisi::ECS;

TEST_CASE("ChangeDetection: Add stamps a tracked component, and the tick advances")
{
    Scene scene;
    CHECK(scene.CurrentChangeTick() == 0);

    const Entity e = scene.Create();
    REQUIRE(scene.Add<Tracked>(e, {1}) != nullptr);

    // A fresh component counts as changed: its tick is nonzero and equals the
    // scene's current tick.
    CHECK(scene.ChangeTick<Tracked>(e) > 0);
    CHECK(scene.ChangeTick<Tracked>(e) == scene.CurrentChangeTick());
}

TEST_CASE("ChangeDetection: GetMut stamps, Get does not")
{
    Scene scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add<Tracked>(e) != nullptr);

    const uint64_t afterAdd = scene.CurrentChangeTick();

    // A read via Get must not advance the tick.
    Tracked *readOnly = scene.Get<Tracked>(e);
    REQUIRE(readOnly != nullptr);
    CHECK(scene.ChangeTick<Tracked>(e) == afterAdd);
    CHECK(scene.CurrentChangeTick() == afterAdd);

    // A mutable access via GetMut stamps a newer tick.
    Tracked *writable = scene.GetMut<Tracked>(e);
    REQUIRE(writable != nullptr);
    CHECK(scene.ChangeTick<Tracked>(e) > afterAdd);
    CHECK(scene.CurrentChangeTick() > afterAdd);
}

TEST_CASE("ChangeDetection: Changed(entity, since) tracks a consumer bookmark")
{
    Scene scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add<Tracked>(e) != nullptr);

    // A consumer that has just observed the scene records the current tick.
    uint64_t bookmark = scene.CurrentChangeTick();
    CHECK_FALSE(scene.Changed<Tracked>(e, bookmark)); // nothing written since

    scene.GetMut<Tracked>(e)->value = 42;
    CHECK(scene.Changed<Tracked>(e, bookmark)); // a write since the bookmark shows

    // After re-observing, it is unchanged again until the next write.
    bookmark = scene.CurrentChangeTick();
    CHECK_FALSE(scene.Changed<Tracked>(e, bookmark));
}

TEST_CASE("ChangeDetection: MarkChanged stamps by ComponentId (type-erased path)")
{
    Scene scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add<Tracked>(e) != nullptr);

    const uint64_t bookmark = scene.CurrentChangeTick();
    const Assisi::Core::Reflect::ComponentId id = Assisi::Core::Reflect::ComponentIdOf<Tracked>();

    scene.MarkChanged(e, id);
    CHECK(scene.Changed<Tracked>(e, bookmark));
}

TEST_CASE("ChangeDetection: untracked components never stamp")
{
    Scene scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add<Position>(e, {1.0f}) != nullptr);

    // Position is not ACOMP(tracked): Add, GetMut, and MarkChanged are all inert.
    CHECK(scene.ChangeTick<Position>(e) == 0);
    CHECK(scene.CurrentChangeTick() == 0);

    scene.GetMut<Position>(e)->x = 5.0f;
    CHECK(scene.ChangeTick<Position>(e) == 0);
    CHECK(scene.CurrentChangeTick() == 0);

    scene.MarkChanged(e, Assisi::Core::Reflect::ComponentIdOf<Position>());
    CHECK(scene.CurrentChangeTick() == 0);
}

TEST_CASE("ChangeDetection: ChangedSince names what was written and nothing else")
{
    Scene scene;
    const Entity still = scene.Create();
    const Entity mover = scene.Create();
    REQUIRE(scene.Add<Tracked>(still, {1}) != nullptr);
    REQUIRE(scene.Add<Tracked>(mover, {2}) != nullptr);

    const uint64_t bookmark = scene.CurrentChangeTick();

    // A frame in which nothing was written names nobody. This is the case the
    // shadow cache is built on: a still scene costs one scan and no work.
    std::vector<Entity> changed;
    scene.ChangedSince<Tracked>(bookmark, changed);
    CHECK(changed.empty());

    scene.GetMut<Tracked>(mover)->value = 20;
    scene.ChangedSince<Tracked>(bookmark, changed);
    REQUIRE(changed.size() == 1);
    CHECK(changed[0] == mover);

    // Appended, not assigned: one set collects several pools' changes, so a
    // second ask must leave the first ask's answer in place.
    scene.ChangedSince<Tracked>(bookmark, changed);
    CHECK(changed.size() == 2);
}

TEST_CASE("ChangeDetection: ChangedSince follows a component through a swap-remove")
{
    Scene scene;
    const Entity a = scene.Create();
    const Entity b = scene.Create();
    REQUIRE(scene.Add<Tracked>(a, {1}) != nullptr);
    REQUIRE(scene.Add<Tracked>(b, {2}) != nullptr);

    const uint64_t bookmark = scene.CurrentChangeTick();
    scene.GetMut<Tracked>(b)->value = 20;

    // Removing a swap-pops b into a's dense slot. The scan reads the tick lane
    // and the entity lane by the same position, so naming a here would be a
    // shadow tile invalidated for something that never moved.
    scene.Remove<Tracked>(a);

    std::vector<Entity> changed;
    scene.ChangedSince<Tracked>(bookmark, changed);
    REQUIRE(changed.size() == 1);
    CHECK(changed[0] == b);
}

TEST_CASE("ChangeDetection: an untracked pool names nobody, whatever was written")
{
    Scene scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add<Position>(e, {1.0f}) != nullptr);
    scene.GetMut<Position>(e)->x = 5.0f;

    std::vector<Entity> changed;
    scene.ChangedSince<Position>(0, changed);
    CHECK(changed.empty());
}

TEST_CASE("ChangeDetection: swap-remove keeps ticks aligned with their components")
{
    Scene scene;
    const Entity a = scene.Create();
    const Entity b = scene.Create();
    REQUIRE(scene.Add<Tracked>(a, {1}) != nullptr);
    REQUIRE(scene.Add<Tracked>(b, {2}) != nullptr);

    // Give b a distinctly newer tick, then remove a (swap-pop moves b into a's
    // dense slot). b's tick must move with it, not stay at a's old slot value.
    scene.GetMut<Tracked>(b)->value = 20;
    const uint64_t bTick = scene.ChangeTick<Tracked>(b);

    scene.Remove<Tracked>(a);

    REQUIRE(scene.Get<Tracked>(b) != nullptr);
    CHECK(scene.Get<Tracked>(b)->value == 20);
    CHECK(scene.ChangeTick<Tracked>(b) == bTick);
}
