/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/ECS/SparseSet.hpp>

using namespace Assisi::ECS;

TEST_CASE("SparseSet: add / get / has round-trip")
{
    SparseSet<int> set;
    const Entity e{.index = 3, .generation = 0};

    CHECK_FALSE(set.Has(e));
    CHECK(set.Get(e) == nullptr);

    auto added = set.Add(e, 42);
    REQUIRE(added.has_value());
    CHECK(**added == 42);

    CHECK(set.Has(e));
    REQUIRE(set.Get(e) != nullptr);
    CHECK(*set.Get(e) == 42);
    CHECK(set.Size() == 1);
}

TEST_CASE("SparseSet: duplicate add is rejected")
{
    SparseSet<int> set;
    const Entity e{.index = 1, .generation = 0};

    REQUIRE(set.Add(e, 1).has_value());
    auto again = set.Add(e, 2);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == SparseSetError::AlreadyExists);
    CHECK(*set.Get(e) == 1); // original value untouched
}

TEST_CASE("SparseSet: remove keeps the dense array packed (swap-and-pop)")
{
    SparseSet<int> set;
    const Entity a{.index = 0, .generation = 0};
    const Entity b{.index = 1, .generation = 0};
    const Entity c{.index = 2, .generation = 0};

    REQUIRE(set.Add(a, 10).has_value());
    REQUIRE(set.Add(b, 20).has_value());
    REQUIRE(set.Add(c, 30).has_value());

    set.Remove(a); // a's slot gets the last element (c) swapped in
    CHECK_FALSE(set.Has(a));
    CHECK(set.Size() == 2);

    // b and c must still resolve to their own values after the swap.
    REQUIRE(set.Get(b) != nullptr);
    REQUIRE(set.Get(c) != nullptr);
    CHECK(*set.Get(b) == 20);
    CHECK(*set.Get(c) == 30);
}

// Regression test for the generation-aliasing bug: a destroyed entity's slot
// reused by a newer entity must NOT let the stale handle read the new
// occupant's component.
TEST_CASE("SparseSet: stale handle does not alias a reused slot")
{
    SparseSet<int> set;
    const Entity oldE{.index = 5, .generation = 0};
    const Entity newE{.index = 5, .generation = 1}; // same slot, next generation

    REQUIRE(set.Add(oldE, 100).has_value());

    // Simulate destruction: the registry removes the component, then the slot
    // is reused for a newer generation which adds its own component.
    set.Remove(oldE);
    REQUIRE(set.Add(newE, 200).has_value());

    CHECK(set.Has(newE));
    CHECK(*set.Get(newE) == 200);

    // The stale handle must report absence, not alias newE's component.
    CHECK_FALSE(set.Has(oldE));
    CHECK(set.Get(oldE) == nullptr);
}

TEST_CASE("SparseSet: removing a stale handle is a no-op")
{
    SparseSet<int> set;
    const Entity oldE{.index = 2, .generation = 0};
    const Entity newE{.index = 2, .generation = 1};

    REQUIRE(set.Add(oldE, 1).has_value());
    set.Remove(oldE);
    REQUIRE(set.Add(newE, 2).has_value());

    set.Remove(oldE); // stale — must not touch newE's live component
    CHECK(set.Has(newE));
    CHECK(*set.Get(newE) == 2);
    CHECK(set.Size() == 1);
}

TEST_CASE("SparseSet: clear empties the set")
{
    SparseSet<int> set;
    REQUIRE(set.Add(Entity{.index = 0, .generation = 0}, 1).has_value());
    REQUIRE(set.Add(Entity{.index = 1, .generation = 0}, 2).has_value());

    set.Clear();
    CHECK(set.Empty());
    CHECK(set.Size() == 0);
    CHECK_FALSE(set.Has(Entity{.index = 0, .generation = 0}));
}
