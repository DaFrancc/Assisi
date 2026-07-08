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

    int *added = set.Add(e, 42);
    REQUIRE(added != nullptr);
    CHECK(*added == 42);

    CHECK(set.Has(e));
    REQUIRE(set.Get(e) != nullptr);
    CHECK(*set.Get(e) == 42);
    CHECK(set.Size() == 1);
}

TEST_CASE("SparseSet: duplicate add is rejected")
{
    SparseSet<int> set;
    const Entity e{.index = 1, .generation = 0};

    REQUIRE(set.Add(e, 1) != nullptr);
    CHECK(set.Add(e, 2) == nullptr); // rejected
    CHECK(*set.Get(e) == 1);         // original value untouched
}

TEST_CASE("SparseSet: remove keeps the dense array packed (swap-and-pop)")
{
    SparseSet<int> set;
    const Entity a{.index = 0, .generation = 0};
    const Entity b{.index = 1, .generation = 0};
    const Entity c{.index = 2, .generation = 0};

    REQUIRE(set.Add(a, 10) != nullptr);
    REQUIRE(set.Add(b, 20) != nullptr);
    REQUIRE(set.Add(c, 30) != nullptr);

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

    REQUIRE(set.Add(oldE, 100) != nullptr);

    // Simulate destruction: the registry removes the component, then the slot
    // is reused for a newer generation which adds its own component.
    set.Remove(oldE);
    REQUIRE(set.Add(newE, 200) != nullptr);

    CHECK(set.Has(newE));
    CHECK(*set.Get(newE) == 200);

    // The stale handle must report absence, not alias newE's component.
    CHECK_FALSE(set.Has(oldE));
    CHECK(set.Get(oldE) == nullptr);
}

// Regression test for the Add-corruption bug: a stale handle whose index slot
// is held by a live newer generation must be rejected, not silently written —
// otherwise it overwrites the live occupant's mapping and orphans a dense slot.
TEST_CASE("SparseSet: adding a stale handle over a live slot is rejected")
{
    SparseSet<int> set;
    const Entity live{.index = 5, .generation = 1};  // current occupant of slot 5
    const Entity stale{.index = 5, .generation = 0}; // destroyed predecessor

    REQUIRE(set.Add(live, 200) != nullptr);

    CHECK(set.Add(stale, 999) == nullptr); // rejected, not silently written

    // The live occupant's mapping must be untouched.
    CHECK(set.Has(live));
    REQUIRE(set.Get(live) != nullptr);
    CHECK(*set.Get(live) == 200);
    CHECK_FALSE(set.Has(stale));
    CHECK(set.Size() == 1);
}

TEST_CASE("SparseSet: removing a stale handle is a no-op")
{
    SparseSet<int> set;
    const Entity oldE{.index = 2, .generation = 0};
    const Entity newE{.index = 2, .generation = 1};

    REQUIRE(set.Add(oldE, 1) != nullptr);
    set.Remove(oldE);
    REQUIRE(set.Add(newE, 2) != nullptr);

    set.Remove(oldE); // stale — must not touch newE's live component
    CHECK(set.Has(newE));
    CHECK(*set.Get(newE) == 2);
    CHECK(set.Size() == 1);
}

TEST_CASE("SparseSet: removing the last dense element takes the no-swap path")
{
    SparseSet<int> set;
    const Entity a{.index = 0, .generation = 0};
    const Entity b{.index = 1, .generation = 0};

    REQUIRE(set.Add(a, 10) != nullptr);
    REQUIRE(set.Add(b, 20) != nullptr); // b is the last dense element

    set.Remove(b); // removedPos == lastPos: nothing is swapped in
    CHECK_FALSE(set.Has(b));
    CHECK(set.Size() == 1);
    REQUIRE(set.Get(a) != nullptr);
    CHECK(*set.Get(a) == 10); // untouched
}

TEST_CASE("SparseSet: the packed entity array parallels the dense array")
{
    SparseSet<int> set;
    const Entity a{.index = 4, .generation = 1};
    const Entity b{.index = 9, .generation = 0};

    REQUIRE(set.Add(a, 10) != nullptr);
    REQUIRE(set.Add(b, 20) != nullptr);

    const auto &entities = set.Entities();
    REQUIRE(entities.size() == 2);
    CHECK(entities[0] == a);
    CHECK(entities[1] == b);

    // After a swap-remove of the first, the last entity fills its slot.
    set.Remove(a);
    REQUIRE(set.Entities().size() == 1);
    CHECK(set.Entities()[0] == b);
}

TEST_CASE("SparseSet: const Get resolves a present component")
{
    SparseSet<int> set;
    const Entity e{.index = 2, .generation = 0};
    REQUIRE(set.Add(e, 77) != nullptr);

    const SparseSet<int> &constSet = set;
    REQUIRE(constSet.Get(e) != nullptr);
    CHECK(*constSet.Get(e) == 77);
    CHECK(constSet.Get(Entity{.index = 3, .generation = 0}) == nullptr);
}

TEST_CASE("SparseSet: a high index then a low index both resolve")
{
    SparseSet<int> set;
    const Entity high{.index = 100, .generation = 0};
    const Entity low{.index = 2, .generation = 0};

    REQUIRE(set.Add(high, 1) != nullptr); // grows the sparse array to 101
    REQUIRE(set.Add(low, 2) != nullptr);  // fits inside the existing sparse array

    CHECK(*set.Get(high) == 1);
    CHECK(*set.Get(low) == 2);
    CHECK(set.Size() == 2);
}

TEST_CASE("SparseSet: clear empties the set")
{
    SparseSet<int> set;
    REQUIRE(set.Add(Entity{.index = 0, .generation = 0}, 1) != nullptr);
    REQUIRE(set.Add(Entity{.index = 1, .generation = 0}, 2) != nullptr);

    set.Clear();
    CHECK(set.Empty());
    CHECK(set.Size() == 0);
    CHECK_FALSE(set.Has(Entity{.index = 0, .generation = 0}));
}
