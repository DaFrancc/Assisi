/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <algorithm>
#include <type_traits>
#include <vector>

#include <Assisi/ECS/Registry.hpp>
#include <Assisi/ECS/SparseSet.hpp>

using namespace Assisi::ECS;

// Registry holds non-owning back-pointers to Scene-owned pools; copying or
// moving one would share or dangle them. Guard the deleted special members at
// compile time (a Registry only ever lives pinned as a Scene member).
static_assert(!std::is_copy_constructible_v<Registry>, "Registry must not be copy-constructible");
static_assert(!std::is_copy_assignable_v<Registry>, "Registry must not be copy-assignable");
static_assert(!std::is_move_constructible_v<Registry>, "Registry must not be move-constructible");
static_assert(!std::is_move_assignable_v<Registry>, "Registry must not be move-assignable");

TEST_CASE("Registry: create hands out distinct live entities")
{
    Registry reg;
    const Entity a = reg.Create();
    const Entity b = reg.Create();

    CHECK(a.index != b.index);
    CHECK(reg.IsAlive(a));
    CHECK(reg.IsAlive(b));
    CHECK(reg.AliveCount() == 2);
}

TEST_CASE("Registry: destroy invalidates the handle and bumps generation on reuse")
{
    Registry reg;
    const Entity a = reg.Create();
    reg.Destroy(a);

    CHECK_FALSE(reg.IsAlive(a));
    CHECK(reg.AliveCount() == 0);

    // The next create reuses the slot but with a higher generation.
    const Entity b = reg.Create();
    CHECK(b.index == a.index);
    CHECK(b.generation != a.generation);
    CHECK(reg.IsAlive(b));
    CHECK_FALSE(reg.IsAlive(a)); // stale handle stays dead
}

TEST_CASE("Registry: double destroy is a no-op")
{
    Registry reg;
    const Entity a = reg.Create();
    reg.Destroy(a);
    reg.Destroy(a); // must not underflow the alive count
    CHECK(reg.AliveCount() == 0);
}

TEST_CASE("Registry: NullEntity and out-of-range handles are never alive")
{
    Registry reg;
    CHECK_FALSE(reg.IsAlive(NullEntity));
    CHECK_FALSE(reg.IsAlive(Entity{.index = 999, .generation = 0}));
}

TEST_CASE("Registry: destroy removes the entity from registered pools")
{
    Registry reg;
    SparseSet<int> pool;
    reg.RegisterPool(&pool);

    const Entity a = reg.Create();
    REQUIRE(pool.Add(a, 7) != nullptr);
    CHECK(pool.Has(a));

    reg.Destroy(a);
    CHECK_FALSE(pool.Has(a)); // Destroy() drove pool.Remove()
}

TEST_CASE("Registry: destroy removes the entity from every registered pool")
{
    Registry reg;
    SparseSet<int> a;
    SparseSet<float> b;
    reg.RegisterPool(&a);
    reg.RegisterPool(&b);

    const Entity e = reg.Create();
    REQUIRE(a.Add(e, 1) != nullptr);
    REQUIRE(b.Add(e, 2.0f) != nullptr);

    reg.Destroy(e);
    CHECK_FALSE(a.Has(e));
    CHECK_FALSE(b.Has(e));
}

TEST_CASE("Registry: an unregistered pool is no longer touched by destroy")
{
    Registry reg;
    SparseSet<int> pool;
    reg.RegisterPool(&pool);

    const Entity e = reg.Create();
    REQUIRE(pool.Add(e, 7) != nullptr);

    reg.UnregisterPool(&pool);
    reg.Destroy(e);

    // Destroy no longer drives this pool, so the component lingers (the pool is
    // now the caller's responsibility).
    CHECK(pool.Has(e));
}

TEST_CASE("Registry: EntityAt resolves a slot index to its live handle")
{
    Registry reg;
    const Entity a = reg.Create();
    const Entity b = reg.Create();

    // A live slot resolves to the full handle (index + current generation).
    CHECK(reg.EntityAt(a.index) == a);
    CHECK(reg.EntityAt(b.index) == b);

    // Out-of-range indices have no occupant.
    CHECK(reg.EntityAt(999) == NullEntity);
}

TEST_CASE("Registry: EntityAt reports a freed slot as empty, then follows reuse")
{
    Registry reg;
    const Entity a = reg.Create();
    reg.Destroy(a);

    // Freed but not yet reused: the slot has no live occupant.
    CHECK(reg.EntityAt(a.index) == NullEntity);

    // Reuse bumps the generation; EntityAt resolves to the new occupant, not the
    // stale handle.
    const Entity b = reg.Create();
    REQUIRE(b.index == a.index);
    CHECK(reg.EntityAt(a.index) == b);
    CHECK(reg.EntityAt(a.index) != a);
}

TEST_CASE("Registry: ForEachLive visits every live entity and skips freed slots")
{
    Registry reg;
    const Entity a = reg.Create();
    const Entity b = reg.Create();
    const Entity c = reg.Create();
    reg.Destroy(b); // leaves a hole in the middle

    std::vector<Entity> visited;
    reg.ForEachLive([&](Entity e) { visited.push_back(e); });

    REQUIRE(visited.size() == 2);
    CHECK(visited[0] == a);
    CHECK(visited[1] == c);

    // A slot reused after the hole shows up with its new generation, not the old.
    const Entity d = reg.Create();
    REQUIRE(d.index == b.index);
    visited.clear();
    reg.ForEachLive([&](Entity e) { visited.push_back(e); });
    CHECK(visited.size() == 3);
    CHECK(std::ranges::find(visited, d) != visited.end());
    CHECK(std::ranges::find(visited, b) == visited.end());
}

TEST_CASE("Registry: reset returns to a pristine state")
{
    Registry reg;
    reg.Create();
    reg.Create();
    reg.Reset();

    CHECK(reg.AliveCount() == 0);
    const Entity fresh = reg.Create();
    CHECK(fresh.index == 0);
    CHECK(fresh.generation == 0);
}
