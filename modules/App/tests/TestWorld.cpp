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
#include <Assisi/Runtime/SceneSerializer.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>

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
