/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/TestComponents.hpp>
#include <Assisi/Testing/ThrowOnContractViolation.hpp>

using namespace Assisi::ECS;

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
    int32_t count = 0;
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

    int32_t count = 0;
    for (auto [ent, pos, vel] : scene.Query<Position, Velocity>())
    {
        (void)ent;
        (void)pos;
        (void)vel;
        ++count;
    }
    CHECK(count == 0);
}

TEST_CASE("Query: Without excludes entities that hold the unwanted component")
{
    Scene scene;
    const Entity posOnly = scene.Create();
    const Entity both = scene.Create();
    REQUIRE(scene.Add<Position>(posOnly, {1.0f}) != nullptr);
    REQUIRE(scene.Add<Position>(both, {2.0f}) != nullptr);
    REQUIRE(scene.Add<Velocity>(both, {9.0f}) != nullptr);

    std::vector<Entity> seen;
    for (auto [e, pos] : scene.Query<Position>(Without<Velocity>{}))
    {
        (void)pos;
        seen.push_back(e);
    }
    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == posOnly); // 'both' is rejected by its Velocity
}

TEST_CASE("Query: Without a never-created pool excludes nobody")
{
    Scene scene;
    const Entity a = scene.Create();
    const Entity b = scene.Create();
    REQUIRE(scene.Add<Position>(a, {1.0f}) != nullptr);
    REQUIRE(scene.Add<Position>(b, {2.0f}) != nullptr);
    // The Velocity pool was never created; excluding it must reject no one.

    int32_t count = 0;
    for (auto [e, pos] : scene.Query<Position>(Without<Velocity>{}))
    {
        (void)e;
        (void)pos;
        ++count;
    }
    CHECK(count == 2);
}

TEST_CASE("Query: multiple exclusions reject an entity holding any of them")
{
    Scene scene;
    const Entity clean = scene.Create();  // Position only
    const Entity hasVel = scene.Create(); // Position + Velocity
    const Entity hasTag = scene.Create(); // Position + Tag
    REQUIRE(scene.Add<Position>(clean, {1.0f}) != nullptr);
    REQUIRE(scene.Add<Position>(hasVel, {2.0f}) != nullptr);
    REQUIRE(scene.Add<Velocity>(hasVel, {2.0f}) != nullptr);
    REQUIRE(scene.Add<Position>(hasTag, {3.0f}) != nullptr);
    REQUIRE(scene.Add<Tag>(hasTag, {}) != nullptr);

    std::vector<Entity> seen;
    for (auto [e, pos] : scene.Query<Position>(Without<Velocity, Tag>{}))
    {
        (void)pos;
        seen.push_back(e);
    }
    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == clean); // only the entity with neither excluded component survives
}

TEST_CASE("Query: a destroyed entity is skipped even if the slot is reused")
{
    Scene scene;
    const Entity a = scene.Create();
    REQUIRE(scene.Add<Position>(a, {1.0f}) != nullptr);
    REQUIRE(scene.Add<Velocity>(a, {1.0f}) != nullptr);

    scene.Destroy(a);
    scene.FlushDestroyed(); // apply the deferred destroy so the slot is freed for reuse

    // Reuse the slot with a fresh entity that only has Position.
    const Entity b = scene.Create();
    CHECK(b.index == a.index);
    REQUIRE(scene.Add<Position>(b, {9.0f}) != nullptr);

    int32_t count = 0;
    for (auto [e, pos, vel] : scene.Query<Position, Velocity>())
    {
        (void)e;
        (void)pos;
        (void)vel;
        ++count;
    }
    CHECK(count == 0); // stale 'a' must not match; 'b' lacks Velocity
}

// The point of deferring Destroy: calling it mid-Query touches no pool, so the
// iterator stays valid and every entity is still visited this pass. The removals
// land only at FlushDestroyed(). Enough entities to force the dense array to have
// reallocated had the destroy been immediate.
TEST_CASE("Query: Destroy during iteration is deferred and does not invalidate")
{
    Scene scene;
    constexpr int32_t kCount = 128;
    for (int32_t i = 0; i < kCount; ++i)
    {
        const Entity e = scene.Create();
        REQUIRE(scene.Add<Position>(e, {static_cast<float>(i)}) != nullptr);
    }

    int32_t visited = 0;
    for (auto [e, pos] : scene.Query<Position>())
    {
        (void)pos;
        ++visited;
        scene.Destroy(e); // deferred — must not disturb this iteration
    }

    CHECK(visited == kCount);            // every entity was still visited
    CHECK(scene.AliveCount() == kCount); // nothing removed yet

    scene.FlushDestroyed();
    CHECK(scene.AliveCount() == 0); // now all applied

    int32_t survivors = 0;
    for (auto [e, pos] : scene.Query<Position>())
    {
        (void)e;
        (void)pos;
        ++survivors;
    }
    CHECK(survivors == 0);
}

// The mid-iteration mutation guard. With a throwing contract handler installed,
// a fired ASSISI_ASSERT surfaces as a catchable ContractViolation, so both that
// it *fires* on a real violation and that it does *not* fire on a safe operation
// are checked in-process. Debug-only — the guard compiles out in release.
#ifndef NDEBUG
namespace
{
// Scene is non-movable, so fill by reference rather than return by value.
void FillWithPositions(Scene &scene, int32_t count)
{
    for (int32_t i = 0; i < count; ++i)
    {
        const Entity e = scene.Create();
        REQUIRE(scene.Add<Position>(e, {static_cast<float>(i)}) != nullptr);
    }
}
} // namespace

TEST_CASE("Query guard: adding a queried component mid-iteration is caught")
{
    Assisi::Testing::ThrowOnContractViolation guard;
    Scene scene;
    FillWithPositions(scene, 8);

    CHECK_THROWS_AS(([&]
    {
        for (auto [e, pos] : scene.Query<Position>())
        {
            (void)e;
            (void)pos;
            const Entity fresh = scene.Create();
            (void)scene.Add<Position>(fresh, {0.0f});                  // grows the queried pool
        }
    }()),
                    Assisi::Core::ContractViolation);
}

TEST_CASE("Query guard: removing a queried component mid-iteration is caught")
{
    Assisi::Testing::ThrowOnContractViolation guard;
    Scene scene;
    FillWithPositions(scene, 8);

    CHECK_THROWS_AS(([&]
    {
        for (auto [e, pos] : scene.Query<Position>())
        {
            (void)pos;
            scene.Remove<Position>(e);                  // swap-removes from the queried pool
        }
    }()),
                    Assisi::Core::ContractViolation);
}

TEST_CASE("Query guard: mutating a non-queried pool mid-iteration is allowed")
{
    Assisi::Testing::ThrowOnContractViolation guard;
    Scene scene;
    FillWithPositions(scene, 8);

    int32_t seen = 0;
    // Adding Velocity is not a change to the Position pool being iterated, so it
    // must not trip the guard.
    CHECK_NOTHROW(([&]
    {
        for (auto [e, pos] : scene.Query<Position>())
        {
            (void)pos;
            ++seen;
            (void)scene.Add<Velocity>(e, {1.0f});
        }
    }()));
    CHECK(seen == 8);
}

TEST_CASE("Query guard: mutating an excluded pool mid-iteration is allowed")
{
    Assisi::Testing::ThrowOnContractViolation guard;
    Scene scene;
    FillWithPositions(scene, 8);

    // Pre-create the Tag pool (and capture a non-null excluded pointer) with an
    // entity that has Tag but no Position, so it never appears in the query.
    const Entity tagOnly = scene.Create();
    REQUIRE(scene.Add<Tag>(tagOnly, {}) != nullptr);

    int32_t seen = 0;
    // Query<Position>(Without<Tag>): the excluded Tag pool is re-probed each step
    // through its stable address, so growing it mid-iteration is safe. This is a
    // regression guard — the check once summed excluded pools and would abort here.
    CHECK_NOTHROW(([&]
    {
        for (auto [e, pos] : scene.Query<Position>(Without<Tag>{}))
        {
            (void)pos;
            ++seen;
            (void)scene.Add<Tag>(e, {});                // mutates the excluded pool
        }
    }()));
    CHECK(seen == 8);
}

TEST_CASE("Query guard: destroying entities mid-iteration is allowed (deferred)")
{
    Assisi::Testing::ThrowOnContractViolation guard;
    Scene scene;
    FillWithPositions(scene, 64);

    int32_t seen = 0;
    // Destroy is deferred, so it never touches the pool mid-loop — must not trip.
    CHECK_NOTHROW(([&]
    {
        for (auto [e, pos] : scene.Query<Position>())
        {
            (void)pos;
            ++seen;
            scene.Destroy(e);
        }
    }()));
    CHECK(seen == 64);
    CHECK(scene.AliveCount() == 64); // still deferred
    scene.FlushDestroyed();
    CHECK(scene.AliveCount() == 0);
}

TEST_CASE("Query guard: a normal full iteration never trips")
{
    Assisi::Testing::ThrowOnContractViolation guard;
    Scene scene;
    FillWithPositions(scene, 100);

    float sum = 0.0f;
    CHECK_NOTHROW(([&]
    {
        for (auto [e, pos] : scene.Query<Position>())
        {
            (void)e;
            sum += pos.x;
        }
    }()));
    CHECK(sum == doctest::Approx(100.0f * 99.0f / 2.0f)); // 0 + 1 + ... + 99
}
#endif // !NDEBUG
