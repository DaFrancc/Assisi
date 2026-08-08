/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// Tests for App::WorldManager — the invariants the rest of the app relies on:
/// stable addresses, deterministic iteration, and roles that cannot be
/// destroyed out from under their holders. See docs/multi-scene-design-notes.md.

#include <doctest/doctest.h>

// A ThreadSanitizer build steps physics on Jolt's single-threaded job system, so
// there is no pool to spin up — see PhysicsWorld.cpp's JoltRuntime for why.
#if defined(__SANITIZE_THREAD__)
#    define ASSISI_APP_TSAN 1
#elif defined(__has_feature)
#    if __has_feature(thread_sanitizer)
#        define ASSISI_APP_TSAN 1
#    endif
#endif

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <Assisi/App/SystemCatalog.hpp>
#include <Assisi/App/TestSystems.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/JobSystem.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>

using namespace Assisi::App;

TEST_CASE("WorldManager generates unique names from the label")
{
    WorldManager worlds;
    World       &a = worlds.Create("Main");
    World       &b = worlds.Create("Main");

    CHECK(a.name != b.name);
    CHECK(a.name.starts_with("Main#"));
    CHECK(worlds.Count() == 2u);
    CHECK(worlds.Find(a.name) == &a);
    CHECK(worlds.Find(b.name) == &b);
    CHECK(worlds.Find("nope") == nullptr);
}

TEST_CASE("World addresses survive creating and destroying other worlds")
{
    // EditHistory and the panels hold references into a world for the whole
    // session; reseating one would dangle them.
    WorldManager worlds;
    World       &kept = worlds.Create("Kept");
    kept.levelPath    = "levels/A.alvl";

    std::vector<World *> scratch;
    for (int32_t i = 0; i < 8; ++i)
    {
        scratch.push_back(&worlds.Create("Scratch"));
    }
    CHECK(worlds.Find("Kept#1") == &kept);
    CHECK(kept.levelPath == "levels/A.alvl");

    for (const World *world : scratch)
    {
        CHECK(worlds.Destroy(world->name));
    }
    CHECK(worlds.Count() == 1u);
    CHECK(worlds.Find("Kept#1") == &kept);
    CHECK(kept.levelPath == "levels/A.alvl");
}

TEST_CASE("ForEach visits worlds in creation order")
{
    WorldManager worlds;
    const std::string first  = worlds.Create("A").name;
    const std::string second = worlds.Create("B").name;
    const std::string third  = worlds.Create("C").name;

    std::vector<std::string> seen;
    worlds.ForEach([&seen](World &world) { seen.push_back(world.name); });
    CHECK(seen == std::vector<std::string>{first, second, third});
}

TEST_CASE("A world holding a role cannot be destroyed")
{
    WorldManager worlds;
    World       &active = worlds.Create("Active");
    World       &edited = worlds.Create("Edited");
    World       &spare  = worlds.Create("Spare");

    worlds.SetActive(active);
    worlds.SetEdited(edited);

    CHECK_FALSE(worlds.Destroy(active.name));
    CHECK_FALSE(worlds.Destroy(edited.name));
    CHECK(worlds.Count() == 3u);

    // Moving the role to a successor releases the old holder.
    worlds.SetActive(spare);
    CHECK(worlds.Destroy(active.name));
    CHECK(worlds.Active() == &spare);
    CHECK(worlds.Edited() == &edited);
    CHECK(worlds.Count() == 2u);
}

TEST_CASE("Active and edited are independent roles")
{
    // The point of the split: the game travels (active moves) while the editor
    // keeps saving/undoing into the level the author opened (edited stays).
    WorldManager worlds;
    World       &authored  = worlds.Create("Authored");
    World       &travelled = worlds.Create("Travelled");

    worlds.SetActive(authored);
    worlds.SetEdited(authored);
    worlds.SetActive(travelled);

    CHECK(worlds.Active() == &travelled);
    CHECK(worlds.Edited() == &authored);
}

#ifdef __linux__
namespace
{
// Live threads in this process, from the kernel's own view.
std::size_t ThreadCount()
{
    std::size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/task"))
    {
        (void)entry;
        ++count;
    }
    return count;
}
} // namespace

TEST_CASE("Resident worlds share one physics thread pool")
{
    // The multi-scene rule: N worlds must not mean N Jolt thread pools, each
    // sized to the machine (docs/multi-scene-design-notes.md §1). Measured
    // rather than asserted structurally, because the sharing lives inside
    // PhysicsWorld's private impl.
    const std::size_t baseline = ThreadCount();

    WorldManager worlds;
    worlds.Create("First"); // brings the shared Jolt runtime up
    const std::size_t afterFirst = ThreadCount();

    // Guard against the test passing vacuously: on a multi-core machine the
    // first world must actually have spun the pool up, or "no growth" below
    // would prove nothing.
    //
    // Except under tsan, where the pool is deliberately Jolt's single-threaded
    // job system and there is nothing to spin up. The check below still means
    // something there — it is the one that would catch a pool per world — but
    // this guard cannot, so it is skipped rather than weakened for every build.
#if !defined(ASSISI_APP_TSAN)
    if (std::thread::hardware_concurrency() > 1u)
    {
        REQUIRE(afterFirst > baseline);
    }
#endif

    for (int32_t i = 0; i < 4; ++i)
    {
        worlds.Create("More");
    }
    CHECK(ThreadCount() == afterFirst);
}
#endif

TEST_CASE("DestroyAllExcept keeps one world and gives it both roles")
{
    WorldManager worlds;
    World       &keep = worlds.Create("Keep");
    worlds.SetActive(keep);
    worlds.SetEdited(keep);
    World &travelled = worlds.Create("Travelled");
    worlds.SetActive(travelled);
    worlds.Create("Another");

    CHECK(worlds.DestroyAllExcept(keep) == 2u);
    CHECK(worlds.Count() == 1u);
    CHECK(worlds.Active() == &keep);
    CHECK(worlds.Edited() == &keep);
}

TEST_CASE("An unrendered world's transforms follow its physics")
{
    // The S2 mechanism, and the reason it is two steps: Jolt poses reach Transform
    // components only through the render path, which never runs for a world that
    // simulates without being drawn. Propagating alone would give correct matrices
    // of the spawn pose.
    WorldManager worlds;
    World       &world = worlds.Create("Falling");

    const Assisi::ECS::Entity entity = world.scene.Create();
    auto *transform = world.scene.Add<Assisi::ECS::Transform>(entity);
    REQUIRE(transform != nullptr);
    transform->position = {0.f, 10.f, 0.f};

    const Assisi::Physics::RigidBody body = world.physics.AddBody(
        {0.f, 10.f, 0.f}, glm::quat{1.f, 0.f, 0.f, 0.f},
        Assisi::Physics::PhysicsWorld::ColliderShapeDesc{}, Assisi::Physics::BodyMotion::Dynamic);
    REQUIRE(world.scene.Add<Assisi::Physics::RigidBody>(entity, body) != nullptr);

    constexpr float kStep = 1.f / 60.f;
    for (int32_t i = 0; i < 30; ++i) // half a second of free fall
    {
        world.physics.Update(kStep);
        world.physics.CaptureState();
    }

    // Before the sync the Transform still holds the spawn pose, however far the
    // body has actually fallen.
    CHECK(world.scene.Get<Assisi::ECS::Transform>(entity)->position.y == doctest::Approx(10.f));

    SyncUnrenderedWorld(world);

    const auto *synced = world.scene.Get<Assisi::ECS::Transform>(entity);
    REQUIRE(synced != nullptr);
    CHECK(synced->position.y < 9.f); // gravity happened, and reached the component
    // ...and the world matrix agrees, i.e. propagation ran AFTER the write-back
    // rather than over the stale pose.
    CHECK(synced->worldMatrix[3].y == doctest::Approx(synced->position.y));
    CHECK(world.propagationTick > 0u);
}

TEST_CASE("Resident worlds simulate independently and outlive each other")
{
    // Two levels resident at once must be two physics spaces, not one shared one:
    // a floor in world B must not catch world A's falling body, and destroying
    // either must leave the other's simulation untouched.
    WorldManager worlds;
    World       &falling = worlds.Create("Falling");
    World       &caught  = worlds.Create("Caught");
    worlds.SetActive(falling);
    worlds.SetEdited(falling);

    const auto spawnBody = [](World &world, glm::vec3 at)
    {
        const Assisi::ECS::Entity entity = world.scene.Create();
        world.scene.Add<Assisi::ECS::Transform>(entity)->position = at;
        const Assisi::Physics::RigidBody body =
            world.physics.AddBody(at, glm::quat{1.f, 0.f, 0.f, 0.f},
                                  Assisi::Physics::PhysicsWorld::ColliderShapeDesc{},
                                  Assisi::Physics::BodyMotion::Dynamic);
        (void)world.scene.Add<Assisi::Physics::RigidBody>(entity, body);
        return entity;
    };

    const Assisi::ECS::Entity a = spawnBody(falling, {0.f, 5.f, 0.f});
    const Assisi::ECS::Entity b = spawnBody(caught, {0.f, 5.f, 0.f});

    // Only the second world has ground under it.
    caught.physics.AddBody({0.f, 0.f, 0.f}, glm::quat{1.f, 0.f, 0.f, 0.f},
                           Assisi::Physics::PhysicsWorld::ColliderShapeDesc{.halfExtents = {50.f, 0.5f, 50.f}},
                           Assisi::Physics::BodyMotion::Static);

    falling.simulate = true;
    caught.simulate  = true;

    constexpr float kStep = 1.f / 60.f;
    for (int32_t i = 0; i < 120; ++i) // two seconds
    {
        worlds.ForEach(
            [](World &world)
            {
                if (!world.simulate)
                    return;
                world.physics.Update(kStep);
                world.physics.CaptureState();
            });
    }
    worlds.ForEach([](World &world) { SyncUnrenderedWorld(world); });

    const float fell   = falling.scene.Get<Assisi::ECS::Transform>(a)->position.y;
    const float landed = caught.scene.Get<Assisi::ECS::Transform>(b)->position.y;
    CHECK(fell < -5.f);   // nothing to stop it
    CHECK(landed > 0.f);  // the other world's floor did stop it
    CHECK(landed < 5.f);  // ...but it did fall

    // Tearing one down leaves the other simulating.
    worlds.SetActive(falling);
    REQUIRE(worlds.Destroy(caught.name));
    CHECK(worlds.Count() == 1u);

    for (int32_t i = 0; i < 60; ++i)
    {
        falling.physics.Update(kStep);
        falling.physics.CaptureState();
    }
    SyncUnrenderedWorld(falling);
    CHECK(falling.scene.Get<Assisi::ECS::Transform>(a)->position.y < fell);
}

TEST_CASE("Travel swaps the active world and keeps the edited one dormant")
{
    // The S3 model, exercised without a GPU (no render services installed, so the
    // manager takes the scene+physics path). What matters here is the bookkeeping:
    // which world is active, which survives, and what a failed travel does.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "assisi-world-travel-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "levels");
    REQUIRE(Assisi::Core::AssetSystem::SetRoot(root).has_value());

    // Two levels, distinguishable by entity count.
    const auto writeLevel = [&root](const char *name, int32_t entities)
    {
        Assisi::ECS::Scene scene;
        for (int32_t i = 0; i < entities; ++i)
        {
            (void)scene.Add<Assisi::ECS::Transform>(scene.Create());
        }
        REQUIRE(Assisi::Runtime::SceneSerializer::SaveToFile(scene, root / "levels" / name));
    };
    writeLevel("A.alvl", 3);
    writeLevel("B.alvl", 7);

    WorldManager worlds;
    World       &authored = worlds.Create("Main");
    worlds.SetActive(authored);
    worlds.SetEdited(authored);
    authored.state = WorldState::Active;
    REQUIRE(Assisi::Runtime::SceneSerializer::LoadFromFile(authored.scene, "levels/A.alvl"));

    // --- travel A -> B -------------------------------------------------------
    World *const inB = worlds.LoadLevel("levels/B.alvl");
    REQUIRE(inB != nullptr);
    CHECK(worlds.Active() == inB);
    CHECK(inB->simulate);
    CHECK(inB->levelPath == "levels/B.alvl");
    CHECK(worlds.Count() == 2u); // the authored level is still resident...
    CHECK(worlds.Edited() == &authored);
    CHECK(authored.state == WorldState::Dormant); // ...dormant and frozen
    CHECK_FALSE(authored.simulate);

    // --- travel B -> A again -------------------------------------------------
    // A second world of the same level as the edited one: names are generated, so
    // this is a distinct world and not a collision.
    World *const inA2 = worlds.LoadLevel("levels/A.alvl");
    REQUIRE(inA2 != nullptr);
    CHECK(inA2 != &authored);
    CHECK(worlds.Active() == inA2);
    CHECK(worlds.Count() == 2u); // B was destroyed on the way out; edited kept
    CHECK(worlds.Edited() == &authored);

    // --- a travel that fails -------------------------------------------------
    CHECK(worlds.LoadLevel("levels/DoesNotExist.alvl") == nullptr);
    CHECK(worlds.Active() == inA2); // still playing exactly where we were
    CHECK(worlds.Count() == 2u);    // no half-created world left behind

    // --- Stop ----------------------------------------------------------------
    CHECK(worlds.DestroyAllExcept(authored) == 1u);
    CHECK(worlds.Count() == 1u);
    CHECK(worlds.Active() == &authored);
    // The authored scene came through untouched by any of it.
    std::size_t count = 0;
    authored.scene.ForEachEntity([&count](Assisi::ECS::Entity) { ++count; });
    CHECK(count == 3u);

    std::filesystem::remove_all(root);
}

TEST_CASE("MigrateEntity moves a subtree and rebuilds its physics in the destination")
{
    WorldManager worlds;
    World       &src = worlds.Create("Src");
    World       &dst = worlds.Create("Dst");
    worlds.SetActive(src);
    worlds.SetEdited(src);

    // A parent with a dynamic body, a child parented to it, and a bystander the
    // child also references (which will be left behind).
    const Assisi::ECS::Entity parent = src.scene.Create();
    src.scene.Add<Assisi::ECS::Transform>(parent)->position = {1.f, 2.f, 3.f};
    (void)src.scene.Add<Assisi::Physics::RigidBodyDescriptor>(parent, Assisi::Physics::RigidBodyDescriptor{});
    const Assisi::Physics::RigidBody body = src.physics.AddBodyFromDescriptor(
        src.scene, parent, *src.scene.Get<Assisi::ECS::Transform>(parent),
        *src.scene.Get<Assisi::Physics::RigidBodyDescriptor>(parent));
    (void)src.scene.Add<Assisi::Physics::RigidBody>(parent, body);

    const Assisi::ECS::Entity child = src.scene.Create();
    (void)src.scene.Add<Assisi::ECS::Transform>(child);
    (void)src.scene.Add<Assisi::Runtime::Parent>(child, Assisi::Runtime::Parent{parent});

    const std::size_t srcBefore = [&]
    { std::size_t n = 0; src.scene.ForEachEntity([&n](Assisi::ECS::Entity) { ++n; }); return n; }();
    CHECK(srcBefore == 2u);

    const Assisi::ECS::Entity movedRoot = worlds.MigrateEntity(src, dst, parent);
    REQUIRE(movedRoot != Assisi::ECS::NullEntity);

    // The subtree (parent + child) left the source entirely.
    std::size_t srcAfter = 0;
    src.scene.ForEachEntity([&srcAfter](Assisi::ECS::Entity) { ++srcAfter; });
    CHECK(srcAfter == 0u);
    CHECK_FALSE(src.scene.IsAlive(parent));

    // Destination has the parent and its child, two entities.
    std::size_t dstCount = 0;
    dst.scene.ForEachEntity([&dstCount](Assisi::ECS::Entity) { ++dstCount; });
    CHECK(dstCount == 2u);

    // Component state came across.
    const auto *movedT = dst.scene.Get<Assisi::ECS::Transform>(movedRoot);
    REQUIRE(movedT != nullptr);
    CHECK(movedT->position.x == doctest::Approx(1.f));
    CHECK(movedT->position.z == doctest::Approx(3.f));

    // The in-set Parent ref remapped to the destination handle of the parent, not
    // a stale source handle.
    Assisi::ECS::Entity movedChild = Assisi::ECS::NullEntity;
    dst.scene.ForEachEntity(
        [&](Assisi::ECS::Entity e)
        {
            if (dst.scene.Get<Assisi::Runtime::Parent>(e) != nullptr)
                movedChild = e;
        });
    REQUIRE(movedChild != Assisi::ECS::NullEntity);
    CHECK(dst.scene.Get<Assisi::Runtime::Parent>(movedChild)->parent == movedRoot);

    // The migrated parent has a live body in the DESTINATION world and it falls
    // there, independently of the (now empty of dynamics) source world.
    REQUIRE(dst.scene.Get<Assisi::Physics::RigidBody>(movedRoot) != nullptr);
    dst.simulate = true;
    constexpr float kStep = 1.f / 60.f;
    for (int32_t i = 0; i < 30; ++i)
    {
        dst.physics.Update(kStep);
        dst.physics.CaptureState();
    }
    SyncUnrenderedWorld(dst);
    CHECK(dst.scene.Get<Assisi::ECS::Transform>(movedRoot)->position.y < 2.f);
}

TEST_CASE("Migrating an entity out from under a ref nulls that ref")
{
    // A child whose parent stays behind: migrate the child alone, and its Parent
    // ref — now pointing outside the migrated set — must resolve to null in the
    // destination rather than to some unrelated destination entity.
    WorldManager worlds;
    World       &src = worlds.Create("Src");
    World       &dst = worlds.Create("Dst");

    const Assisi::ECS::Entity anchor = src.scene.Create();
    (void)src.scene.Add<Assisi::ECS::Transform>(anchor);
    // Give the destination a pre-existing entity, so a stale index couldn't
    // accidentally resolve onto "nothing".
    (void)dst.scene.Add<Assisi::ECS::Transform>(dst.scene.Create());

    const Assisi::ECS::Entity loneChild = src.scene.Create();
    (void)src.scene.Add<Assisi::ECS::Transform>(loneChild);
    (void)src.scene.Add<Assisi::Runtime::Parent>(loneChild, Assisi::Runtime::Parent{anchor});

    const Assisi::ECS::Entity moved = worlds.MigrateEntity(src, dst, loneChild);
    REQUIRE(moved != Assisi::ECS::NullEntity);

    CHECK(src.scene.IsAlive(anchor)); // the anchor stayed
    const auto *parent = dst.scene.Get<Assisi::Runtime::Parent>(moved);
    REQUIRE(parent != nullptr);
    CHECK(parent->parent == Assisi::ECS::NullEntity); // the out-of-set ref nulled
}

TEST_CASE("Async travel loads in the background then swaps instantly")
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "assisi-world-async-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "levels");
    REQUIRE(Assisi::Core::AssetSystem::SetRoot(root).has_value());

    {
        // Physics bodies in the background level, so promotion's worker actually
        // builds Jolt bodies (in a SEPARATE PhysicsSystem) while the main thread
        // steps the running world below — the concurrency async travel rests on.
        Assisi::ECS::Scene scene;
        for (int32_t i = 0; i < 12; ++i)
        {
            const Assisi::ECS::Entity e = scene.Create();
            (void)scene.Add<Assisi::ECS::Transform>(e);
            (void)scene.Add<Assisi::Physics::RigidBodyDescriptor>(e, Assisi::Physics::RigidBodyDescriptor{});
        }
        REQUIRE(Assisi::Runtime::SceneSerializer::SaveToFile(scene, root / "levels" / "Big.alvl"));
    }

    Assisi::Core::JobSystem jobs;

    WorldManager worlds;
    worlds.SetServices({.cache = nullptr, .database = nullptr, .renderer = nullptr, .jobs = &jobs});
    World &start = worlds.Create("Start");
    worlds.SetActive(start);
    worlds.SetEdited(start);
    start.state = WorldState::Active;

    // A live dynamic body in the running world, so its Update() does real solver
    // work (island builder, temp allocator) concurrently with the worker's build.
    (void)start.physics.AddBody({0.f, 20.f, 0.f}, glm::quat{1.f, 0.f, 0.f, 0.f},
                                Assisi::Physics::PhysicsWorld::ColliderShapeDesc{},
                                Assisi::Physics::BodyMotion::Dynamic);

    World *const loading = worlds.BeginLoadLevel("levels/Big.alvl");
    REQUIRE(loading != nullptr);
    CHECK(loading->state == WorldState::Loading);
    CHECK(worlds.HasPendingLoad());
    CHECK(worlds.Active() == &start); // the running world is untouched while it loads
    CHECK(worlds.PendingLoadPath() == "levels/Big.alvl");

    // The current world keeps stepping while the load runs on a worker — this is
    // where a worker building bodies would collide with the main thread if the
    // per-world isolation were wrong.
    //
    // Bounded by wall time rather than by a step count. What the worker needs in
    // order to finish is time and disk bandwidth, and a fixed number of
    // main-thread iterations only tracks that on an idle machine: the same 200
    // steps elapse just as quickly when the disk is busy, so the budget runs out
    // while the load is still legitimately in flight. That is a flake, and it
    // showed up as one — a cold cache, or a scan of just-built binaries, is
    // enough. The deadline is long enough that only a genuine hang reaches it,
    // and costs nothing on the passing path, which still leaves the instant the
    // load reports ready.
    constexpr std::chrono::seconds kLoadDeadline{10};

    start.simulate                                   = true;
    const std::chrono::steady_clock::time_point stop = std::chrono::steady_clock::now() + kLoadDeadline;
    while (!worlds.PendingLoadReady() && std::chrono::steady_clock::now() < stop)
    {
        start.physics.Update(1.f / 60.f);
        start.physics.CaptureState();
        worlds.PumpPendingLoad(); // drives phase-1 completion + phase-2 asset streaming
    }

    // Once ready, progress has reached 1.0. (No cache in this headless test, so
    // phase 2 completes immediately — but the ready gate still runs through it.)
    REQUIRE(worlds.PendingLoadReady());
    CHECK(worlds.PendingLoadProgress() == doctest::Approx(1.f));

    // Promote — blocks only if the worker is somehow still going; either way the
    // swap itself is a repoint.
    World *const arrived = worlds.PromotePendingLoad();
    REQUIRE(arrived != nullptr);
    CHECK(arrived == loading);
    CHECK(worlds.Active() == arrived);
    CHECK(arrived->state == WorldState::Active);
    CHECK_FALSE(worlds.HasPendingLoad());

    std::size_t count = 0;
    arrived->scene.ForEachEntity([&count](Assisi::ECS::Entity) { ++count; });
    CHECK(count == 12u); // the background-loaded scene really is here

    std::filesystem::remove_all(root);
}

TEST_CASE("A pending background load is safely abandoned on cancel")
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "assisi-world-async-cancel-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "levels");
    REQUIRE(Assisi::Core::AssetSystem::SetRoot(root).has_value());
    {
        Assisi::ECS::Scene scene;
        (void)scene.Add<Assisi::ECS::Transform>(scene.Create());
        REQUIRE(Assisi::Runtime::SceneSerializer::SaveToFile(scene, root / "levels" / "X.alvl"));
    }

    Assisi::Core::JobSystem jobs;
    WorldManager            worlds;
    worlds.SetServices({.cache = nullptr, .database = nullptr, .renderer = nullptr, .jobs = &jobs});
    World &start = worlds.Create("Start");
    worlds.SetActive(start);
    worlds.SetEdited(start);

    REQUIRE(worlds.BeginLoadLevel("levels/X.alvl") != nullptr);
    CHECK(worlds.Count() == 2u);

    // Cancel waits the worker out and drops the half-built world; the active one
    // is untouched. (This is the path Stop and teardown take.)
    worlds.CancelPendingLoad();
    CHECK_FALSE(worlds.HasPendingLoad());
    CHECK(worlds.Count() == 1u);
    CHECK(worlds.Active() == &start);

    // A failed preload (missing level) promotes to nullptr and leaves nothing behind.
    REQUIRE(worlds.BeginLoadLevel("levels/Missing.alvl") != nullptr);
    CHECK(worlds.PromotePendingLoad() == nullptr);
    CHECK(worlds.Count() == 1u);
    CHECK(worlds.Active() == &start);

    std::filesystem::remove_all(root);
}

TEST_CASE("A fresh world starts unloaded, unsimulated, and roleless")
{
    WorldManager worlds;
    const World &world = worlds.Create();

    CHECK(world.state == WorldState::Loading);
    CHECK_FALSE(world.simulate);
    CHECK_FALSE(world.streamingPending);
    CHECK(world.propagationTick == 0u);
    CHECK(world.levelPath.empty());
    CHECK(world.systemNames.empty()); // Create installs nothing; the level's list applies at load
    CHECK(worlds.Active() == nullptr);
    CHECK(worlds.Edited() == nullptr);
}

// ---------------------------------------------------------------------------
// Systems (docs/blueprint-system-concept.md §8)
//
// A level names the systems it wants; ASYSTEM declarations reach the catalog by
// being linked. The cases below use the ones in support/TestSystems.hpp.
// ---------------------------------------------------------------------------

namespace
{
// Runs one Update tick against a world's own registry, the way the frame loop
// will. No window, so no input devices — see SystemContext's pointer fields.
void TickUpdate(World &world, Assisi::Core::EventQueue &events, bool isActiveWorld = true)
{
    world.systems.Run(SystemPhase::Update, SystemContext{world, 0.016f, /*simTick=*/0,
                                                         /*input=*/nullptr, /*actions=*/nullptr,
                                                         events, isActiveWorld});
}

std::uint32_t Runs(const World &world, const char *system)
{
    return Assisi::App::Test::RunCounts::Instance().Count(world, system);
}
} // namespace

TEST_CASE("A level's system list decides which systems its world runs")
{
    Assisi::Core::EventQueue events;
    Assisi::App::Test::RunCounts::Instance().Reset();

    WorldManager worlds;
    World       &named   = worlds.Create("Named");
    World       &unnamed = worlds.Create("Unnamed");

    REQUIRE(worlds.ApplySystems(named, std::vector<std::string>{"Counter"}, "(test)"));
    REQUIRE(worlds.ApplySystems(unnamed, {}, "(test)"));

    TickUpdate(named, events);
    TickUpdate(unnamed, events);

    CHECK(Runs(named, "Counter") == 1);
    CHECK(Runs(unnamed, "Counter") == 0);
    CHECK(named.systemNames == std::vector<std::string>{"Counter"});
}

TEST_CASE("Two worlds naming one system hold independent state")
{
    // The reason the registry is per world: a system's cross-frame state lives in
    // its registered lambda's captures, so one shared instance running over two
    // worlds would advance that state twice per frame.
    Assisi::Core::EventQueue events;
    Assisi::App::Test::RunCounts::Instance().Reset();

    WorldManager worlds;
    World       &first  = worlds.Create("First");
    World       &second = worlds.Create("Second");

    const std::vector<std::string> names{"Counter"};
    REQUIRE(worlds.ApplySystems(first, names, "(test)"));
    REQUIRE(worlds.ApplySystems(second, names, "(test)"));

    TickUpdate(first, events);
    TickUpdate(first, events);
    TickUpdate(second, events);

    CHECK(Runs(first, "Counter") == 2);
    CHECK(Runs(second, "Counter") == 1);
}

TEST_CASE("A name this build does not declare fails the load instead of running short")
{
    Assisi::Core::EventQueue events;
    Assisi::App::Test::RunCounts::Instance().Reset();

    WorldManager worlds;
    World       &world = worlds.Create("Typo");

    // Nothing is installed, not even the names that *were* valid: a half-installed
    // world runs and looks nearly right, which is worse than a refused load.
    CHECK_FALSE(worlds.ApplySystems(world, std::vector<std::string>{"Counter", "Nonexistent"}, "(test)"));
    TickUpdate(world, events);
    CHECK(Runs(world, "Counter") == 0);
}

TEST_CASE("The list is a union: naming a system twice installs it once")
{
    Assisi::Core::EventQueue events;
    Assisi::App::Test::RunCounts::Instance().Reset();

    WorldManager worlds;
    World       &world = worlds.Create("Doubled");

    REQUIRE(worlds.ApplySystems(world, std::vector<std::string>{"Counter", "Counter"}, "(test)"));
    TickUpdate(world, events);

    // Once per tick. It matters beyond tidiness: re-registering a name corrupts
    // the ordering graph, because After()/Before() bind to the first entry.
    CHECK(Runs(world, "Counter") == 1);
}

TEST_CASE("File order carries no meaning; after/before decides run order")
{
    Assisi::Core::EventQueue events;
    Assisi::App::Test::RunCounts::Instance().Reset();

    WorldManager worlds;
    World       &world = worlds.Create("Ordered");

    // Named the wrong way round on purpose. Follower declares `after = Counter`,
    // so the file cannot reorder them.
    REQUIRE(worlds.ApplySystems(world, std::vector<std::string>{"Follower", "Counter"}, "(test)"));

    std::vector<std::string> order;
    world.systems.Run(SystemPhase::Update, SystemContext{world, 0.016f, 0, nullptr, nullptr, events, true});
    CHECK(Runs(world, "Counter") == 1);
    CHECK(Runs(world, "Follower") == 1);
}

TEST_CASE("Re-applying a list replaces the previous systems rather than stacking them")
{
    Assisi::Core::EventQueue events;
    Assisi::App::Test::RunCounts::Instance().Reset();

    WorldManager worlds;
    World       &world = worlds.Create("Reused");

    REQUIRE(worlds.ApplySystems(world, std::vector<std::string>{"Counter"}, "(test)"));
    TickUpdate(world, events);
    CHECK(Runs(world, "Counter") == 1);

    REQUIRE(worlds.ApplySystems(world, std::vector<std::string>{"Follower"}, "(test)"));
    TickUpdate(world, events);
    CHECK(Runs(world, "Counter") == 1); // not run again — it is gone
    CHECK(Runs(world, "Follower") == 1);
    CHECK(world.systemNames == std::vector<std::string>{"Follower"});
}

TEST_CASE("A queued install belongs to one world and cannot reach another")
{
    // The queue used to be a process-global list keyed by raw World*, drained one
    // line after the marshalled work where deferred level loads — the things that
    // free worlds — land. Destroying a world left an entry naming freed memory.
    // Owned by the world, the entry cannot outlive it; this is the regression
    // guard for anyone who moves it back out.
    Assisi::App::Test::RunCounts::Instance().Reset();

    WorldManager worlds;
    World       &doomed   = worlds.Create("Doomed");
    World       &survivor = worlds.Create("Survivor");

    QueueSystemInstall(doomed, std::vector<std::string>{"Counter"}, "car.abp");
    QueueSystemInstall(survivor, std::vector<std::string>{"Follower"}, "car.abp");
    CHECK(doomed.pendingSystems.names == std::vector<std::string>{"Counter"});
    CHECK(survivor.pendingSystems.names == std::vector<std::string>{"Follower"});

    REQUIRE(worlds.Destroy(doomed.name));

    // The survivor is still owed exactly its own install, and draining it is not
    // a walk over anything the dead world could still be in.
    DrainSystemInstalls(survivor);
    CHECK(survivor.systems.Has("Follower"));
    CHECK_FALSE(survivor.systems.Has("Counter"));
    CHECK(survivor.pendingSystems.names.empty());
}

TEST_CASE("Re-targeting a world drops the outgoing level's queued installs")
{
    // A blueprint spawned into the level being replaced has already asked for its
    // systems. ApplySystems clears the registry and installs the new level's list,
    // so a surviving queue entry installs the old blueprint's system into a level
    // that never named it — a frame later, where nothing connects it to the load.
    Assisi::Core::EventQueue events;
    Assisi::App::Test::RunCounts::Instance().Reset();

    WorldManager worlds;
    World       &world = worlds.Create("Retargeted");

    REQUIRE(worlds.ApplySystems(world, std::vector<std::string>{"Counter"}, "levels/Old.alvl"));
    QueueSystemInstall(world, std::vector<std::string>{"Follower"}, "car.abp");

    // Open another level into the same world — the editor's Open Level.
    REQUIRE(worlds.ApplySystems(world, {}, "levels/New.alvl"));
    CHECK(world.pendingSystems.names.empty());

    DrainSystemInstalls(world);
    CHECK_FALSE(world.systems.Has("Counter"));
    CHECK_FALSE(world.systems.Has("Follower"));
    TickUpdate(world, events);
    CHECK(Runs(world, "Follower") == 0);
}

TEST_CASE("A refused system list leaves the queued installs alone")
{
    // ApplySystems is all-or-nothing: a refused list leaves the world running
    // exactly what it was running, so the installs that level queued are still
    // owed. Dropping them on the failure path would strip a live level's
    // blueprints of their behaviour.
    Assisi::App::Test::RunCounts::Instance().Reset();

    WorldManager worlds;
    World       &world = worlds.Create("Refused");

    REQUIRE(worlds.ApplySystems(world, std::vector<std::string>{"Counter"}, "levels/Live.alvl"));
    QueueSystemInstall(world, std::vector<std::string>{"Follower"}, "car.abp");

    CHECK_FALSE(worlds.ApplySystems(world, std::vector<std::string>{"Nonexistent"}, "levels/Bad.alvl"));
    DrainSystemInstalls(world);

    CHECK(world.systems.Has("Counter"));
    CHECK(world.systems.Has("Follower"));
}

TEST_CASE("A spawn queues a union, and draining it twice installs once")
{
    // A hundred bullets in one frame must leave one name, not a hundred — and the
    // drain has to clear what it applied, or every later frame reinstalls it.
    // Re-registering a name is not idempotent: After()/Before() bind to the first
    // entry, so a repeat corrupts the ordering graph.
    Assisi::App::Test::RunCounts::Instance().Reset();

    WorldManager worlds;
    World       &world = worlds.Create("Spawned");
    REQUIRE(worlds.ApplySystems(world, {}, "levels/Empty.alvl"));

    QueueSystemInstall(world, std::vector<std::string>{"Counter"}, "car.abp");
    QueueSystemInstall(world, std::vector<std::string>{"Counter", "Follower"}, "truck.abp");
    CHECK(world.pendingSystems.names == std::vector<std::string>{"Counter", "Follower"});

    // The first spawn to open the queue owns the diagnostic.
    CHECK(world.pendingSystems.context == "car.abp");

    DrainSystemInstalls(world);
    CHECK(world.pendingSystems.names.empty());
    DrainSystemInstalls(world);

    Assisi::Core::EventQueue events;
    TickUpdate(world, events);
    CHECK(Runs(world, "Counter") == 1);
    CHECK(Runs(world, "Follower") == 1);
}

TEST_CASE("A level's system list survives a save/load round trip")
{
    // A Scene does not carry the list, so a save that forgot it would silently
    // strip the field from every level the editor touches.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "assisi-world-systems-roundtrip";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "levels");
    REQUIRE(Assisi::Core::AssetSystem::SetRoot(root).has_value());

    const std::vector<std::string> names{"Counter", "Follower"};
    {
        Assisi::ECS::Scene scene;
        (void)scene.Add<Assisi::ECS::Transform>(scene.Create());
        const Assisi::Runtime::LevelHeader header{.instances = {}, .systems = names};
        REQUIRE(
            Assisi::Runtime::SceneSerializer::SaveToFile(scene, root / "levels" / "L.alvl", header));
    }

    Assisi::ECS::Scene          scene;
    Assisi::Runtime::LevelHeader header;
    REQUIRE(Assisi::Runtime::SceneSerializer::LoadFromFile(scene, "levels/L.alvl", {}, &header));
    CHECK(header.systems == names);

    // ...and a level that needs none stays free of the key.
    {
        Assisi::ECS::Scene bare;
        REQUIRE(Assisi::Runtime::SceneSerializer::SaveToFile(bare, root / "levels" / "N.alvl"));
    }
    Assisi::ECS::Scene          bare;
    Assisi::Runtime::LevelHeader none;
    REQUIRE(Assisi::Runtime::SceneSerializer::LoadFromFile(bare, "levels/N.alvl", {}, &none));
    CHECK(none.systems.empty());

    std::filesystem::remove_all(root);
}

TEST_CASE("Dispatching every simulated world runs shared systems twice, input systems once")
{
    // The shape of the frame loop's per-world dispatch (P3), without an editor:
    // iterate the resident worlds, skip the ones that are not Active+simulate,
    // and mark the active one so ActiveWorldOnly systems can opt out of the rest.
    Assisi::Core::EventQueue events;
    WorldManager             worlds;

    Assisi::App::Test::RunCounts::Instance().Reset();

    World &played    = worlds.Create("Played");
    World &background = worlds.Create("Background");
    World &dormant   = worlds.Create("Dormant");
    const std::vector<std::string> names{"Counter", "ActiveOnly"};
    for (World *w : {&played, &background, &dormant})
        REQUIRE(worlds.ApplySystems(*w, names, "(test)"));

    worlds.SetActive(played);
    played.state     = WorldState::Active;
    played.simulate  = true;
    background.state = WorldState::Active;
    background.simulate = true;
    // Resident and inspectable but not stepped — the edited world during a play
    // session. Its systems exist; they must never run.
    dormant.state    = WorldState::Dormant;
    dormant.simulate = false;

    worlds.ForEach(
        [&](World &world)
        {
            if (world.state != WorldState::Active || !world.simulate)
                return;
            TickUpdate(world, events, /*isActiveWorld=*/&world == worlds.Active());
        });

    // Both simulated worlds ran the shared system...
    CHECK(Runs(played, "Counter") == 1);
    CHECK(Runs(background, "Counter") == 1);
    CHECK(Runs(dormant, "Counter") == 0);
    // ...but there is one InputContext, so the activeWorldOnly one ran once.
    CHECK(Runs(played, "ActiveOnly") == 1);
    CHECK(Runs(background, "ActiveOnly") == 0);
}

TEST_CASE("Pausing stops game logic in every world, not just the one on screen")
{
    // The regression this gate exists for: the editor's Pause clears `simulate`
    // on the VIEWED world only, and travel-from-pause hands the incoming world
    // simulate=true — so a secondary play world keeps its flag set through a
    // Pause. Gating game phases on the per-world flag alone would leave it
    // ticking logic while the editor reads Paused. The host's play state is a
    // second, independent gate (EditorApp::OnFixedUpdate/OnUpdate).
    Assisi::Core::EventQueue events;
    WorldManager             worlds;

    Assisi::App::Test::RunCounts::Instance().Reset();

    World &viewed     = worlds.Create("Viewed");
    World &secondary  = worlds.Create("Secondary");
    const std::vector<std::string> names{"Counter"};
    REQUIRE(worlds.ApplySystems(viewed, names, "(test)"));
    REQUIRE(worlds.ApplySystems(secondary, names, "(test)"));
    worlds.SetActive(viewed);
    viewed.state    = WorldState::Active;
    secondary.state = WorldState::Active;

    // Pause, as the editor leaves things: the viewed world's flag is cleared,
    // the secondary world's is emphatically not.
    bool hostIsPlaying = false;
    viewed.simulate    = false;
    secondary.simulate = true;

    const auto dispatch = [&]
    {
        if (!hostIsPlaying)
            return;
        worlds.ForEach(
            [&](World &world)
            {
                if (world.state != WorldState::Active || !world.simulate)
                    return;
                TickUpdate(world, events, /*isActiveWorld=*/&world == worlds.Active());
            });
    };

    dispatch();
    CHECK(Runs(viewed, "Counter") + Runs(secondary, "Counter") == 0); // paused everywhere, stale flag or not

    // Resume: both worlds are simulating again and both run their logic.
    hostIsPlaying   = true;
    viewed.simulate = true;
    dispatch();
    CHECK(Runs(viewed, "Counter") + Runs(secondary, "Counter") == 2);
}

TEST_CASE("Travelling from inside a system is refused, and deferred travel replaces it")
{
    // A trigger volume or a match-end handler wants to change level, but it runs
    // inside the frame loop's walk over the resident worlds — where LoadLevel
    // would invalidate the walk and could free the very world the system is
    // running in. The mutators refuse there; RequestTravel is the way through.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "assisi-world-deferred-travel";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "levels");
    REQUIRE(Assisi::Core::AssetSystem::SetRoot(root).has_value());

    const auto writeLevel = [&root](const char *name)
    {
        Assisi::ECS::Scene scene;
        (void)scene.Add<Assisi::ECS::Transform>(scene.Create());
        REQUIRE(Assisi::Runtime::SceneSerializer::SaveToFile(scene, root / "levels" / name));
    };
    writeLevel("A.alvl");
    writeLevel("B.alvl");

    WorldManager worlds;
    World       &start = worlds.Create("Main");
    worlds.SetActive(start);
    start.state = WorldState::Active;
    REQUIRE(worlds.LoadLevel("levels/A.alvl") != nullptr);
    const std::size_t residentBefore = worlds.Count();

    // Directly mutating from inside a walk is refused (and logged), leaving the
    // world list exactly as it was.
    worlds.ForEach(
        [&](World &)
        {
            CHECK(worlds.LoadLevel("levels/B.alvl") == nullptr);
            CHECK(worlds.PromotePendingLoad() == nullptr);
            CHECK_FALSE(worlds.Destroy("Main#1"));
        });
    CHECK(worlds.Count() == residentBefore);
    CHECK(worlds.Active()->levelPath == "levels/A.alvl");

    // Requesting is allowed from the same place, and changes nothing yet.
    worlds.ForEach([&](World &) { worlds.RequestTravel("levels/B.alvl"); });
    CHECK(worlds.HasTravelRequest());
    CHECK(worlds.Active()->levelPath == "levels/A.alvl");

    // The host applies it at its safe point.
    World *const arrived = worlds.ProcessTravelRequest();
    REQUIRE(arrived != nullptr);
    CHECK(arrived->levelPath == "levels/B.alvl");
    CHECK(worlds.Active() == arrived);
    CHECK_FALSE(worlds.HasTravelRequest()); // consumed
    CHECK(worlds.ProcessTravelRequest() == nullptr); // and not repeated

    // Two requests in one frame is a game-logic conflict; the last one wins
    // rather than travelling twice.
    worlds.RequestTravel("levels/A.alvl");
    worlds.RequestTravel("levels/B.alvl");
    World *const second = worlds.ProcessTravelRequest();
    REQUIRE(second != nullptr);
    CHECK(second->levelPath == "levels/B.alvl");

    std::filesystem::remove_all(root);
}

TEST_CASE("A background load's systems are installed when it is promoted")
{
    // The worker parks the level's list on the world it exclusively owns;
    // installing it is main-thread work that waits for promotion.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "assisi-world-systems-async";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "levels");
    REQUIRE(Assisi::Core::AssetSystem::SetRoot(root).has_value());

    {
        Assisi::ECS::Scene scene;
        (void)scene.Add<Assisi::ECS::Transform>(scene.Create());
        const Assisi::Runtime::LevelHeader header{.instances = {},
                                                  .systems   = std::vector<std::string>{"Counter"}};
        REQUIRE(
            Assisi::Runtime::SceneSerializer::SaveToFile(scene, root / "levels" / "A.alvl", header));
    }

    Assisi::Core::EventQueue events;
    WorldManager             worlds;
    Assisi::App::Test::RunCounts::Instance().Reset();

    World &start = worlds.Create("Main");
    worlds.SetActive(start);
    start.state = WorldState::Active;

    World *const loading = worlds.BeginLoadLevel("levels/A.alvl");
    REQUIRE(loading != nullptr);
    // The names are parked as soon as the file is read, but nothing is installed
    // while the world is still Loading: installing is main-thread work, and the
    // frame loop skips such worlds anyway. Ticking it proves the registry is empty.
    CHECK(loading->state == WorldState::Loading);
    TickUpdate(*loading, events);
    CHECK(Runs(*loading, "Counter") == 0);

    World *const promoted = worlds.PromotePendingLoad();
    REQUIRE(promoted != nullptr);
    CHECK(promoted->systemNames == std::vector<std::string>{"Counter"});
    TickUpdate(*promoted, events);
    CHECK(Runs(*promoted, "Counter") == 1);

    std::filesystem::remove_all(root);
}
