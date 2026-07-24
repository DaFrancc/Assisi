/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// Tests for App::WorldManager — the invariants the rest of the app relies on:
/// stable addresses, deterministic iteration, and roles that cannot be
/// destroyed out from under their holders. See docs/multi-scene-design-notes.md.

#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetSystem.hpp>
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
    if (std::thread::hardware_concurrency() > 1u)
    {
        REQUIRE(afterFirst > baseline);
    }

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
    start.simulate = true;
    for (int32_t i = 0; i < 200 && !worlds.PendingLoadReady(); ++i)
    {
        start.physics.Update(1.f / 60.f);
        start.physics.CaptureState();
    }

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
    CHECK(worlds.Active() == nullptr);
    CHECK(worlds.Edited() == nullptr);
}
