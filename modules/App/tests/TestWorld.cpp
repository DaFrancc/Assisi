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
