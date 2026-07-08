/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/ECS/Registry.hpp>
#include <Assisi/ECS/SparseSet.hpp>

using namespace Assisi::ECS;

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
    REQUIRE(pool.Add(a, 7).has_value());
    CHECK(pool.Has(a));

    reg.Destroy(a);
    CHECK_FALSE(pool.Has(a)); // Destroy() drove pool.Remove()
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
