/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBlueprintVerbs.cpp
/// @brief Spawn, destroy, prune, explode — and the properties that make them
/// safe to hand to a game.
///
/// The one worth stating: **"the members of instance 7" is a query**, computed
/// when asked and discarded. Every case here is really a test of that — a member
/// destroyed on its own simply is not found next time, a pruned entity keeps
/// living and stops being reachable, and a loose neighbour is never touched
/// because it was never in the answer (docs/blueprint-system-concept.md §7).

#include <doctest/doctest.h>

#include <ostream>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <Assisi/App/BlueprintVerbs.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Blueprint.hpp>

using namespace Assisi;

namespace
{

std::filesystem::path FreshRoot(const std::string &name)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("assisi_bpv_" + name);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());
    Runtime::ClearBlueprintCache();
    return root;
}

void Write(const std::filesystem::path &root, const std::string &name, const nlohmann::json &doc)
{
    std::ofstream out(root / name, std::ios::binary);
    out << doc.dump(2);
    REQUIRE(out.good());
}

/// A body with a physics body, and a wheel parented to it.
nlohmann::json CarFile()
{
    return {{"version", 2},
        {"entities",
         nlohmann::json::array(
             {{{"name", "body"},
                 {"components",
                  {{"Transform",
                      {{"position", {0.f, 0.f, 0.f}}, {"rotation", {1.f, 0.f, 0.f, 0.f}}, {"scale", {1.f, 1.f, 1.f}}}},
                      {"RigidBodyDescriptor", {{"isStatic", true}}}}}},
                 {{"name", "wheel_fl"},
                     {"components",
                      {{"Transform",
                          {{"position", {1.f, 0.f, 0.f}}, {"rotation", {1.f, 0.f, 0.f, 0.f}}, {"scale", {1.f, 1.f, 1.f}}}},
                          {"Parent", {{"parent", "body"}}}}}}})}};
}

int32_t TaggedCount(ECS::Scene &scene, ECS::InstanceId instanceId)
{
    int32_t count = 0;
    for (auto [entity, tag] : scene.Query<ECS::BlueprintMember>())
    {
        if (tag.instanceId == instanceId)
            ++count;
    }
    return count;
}

} // namespace

TEST_CASE("Verbs: spawning creates a runnable instance and returns an id worth keeping")
{
    const std::filesystem::path root = FreshRoot("spawn");
    Write(root, "car.abp", CarFile());

    App::World world;
    ECS::Transform at;
    at.position = {12.f, 0.f, 0.f};

    const std::optional<ECS::InstanceId> id = App::SpawnBlueprint(world, "car.abp", at);
    REQUIRE(id.has_value());
    CHECK(world.scene.AliveCount() == 2);

    // Placed, and with a live Jolt body — the spawn runs the same physics build a
    // level load does, or a spawned car would have no collision at all.
    const ECS::Entity body = App::FindMember(world, *id, "body");
    REQUIRE(body != ECS::NullEntity);
    const Physics::RigidBody *rb = world.scene.Get<Physics::RigidBody>(body);
    REQUIRE(rb != nullptr);
    CHECK(world.physics.GetBodyTransform(*rb).first.x == doctest::Approx(12.f));

    // The source check is the whole reason the table exists.
    CHECK(App::FindInstance(world, *id, "car.abp") != nullptr);
    CHECK(App::FindInstance(world, *id, "crate.abp") == nullptr);
    CHECK(App::FindInstance(world, ECS::InstanceId{999}) == nullptr);

    CHECK(App::FindMember(world, *id, "nothing") == ECS::NullEntity);
}

TEST_CASE("Verbs: a failed spawn leaves nothing")
{
    const std::filesystem::path root = FreshRoot("failed");
    App::World world;
    const ECS::Entity loose = world.scene.Create();
    (void)world.scene.Add(loose, ECS::Transform{});

    CHECK_FALSE(App::SpawnBlueprint(world, "gone.abp", {}).has_value());
    CHECK(world.scene.AliveCount() == 1); // the pre-existing entity, and nothing else
    CHECK(world.instances.Size() == 0);
}

TEST_CASE("Verbs: destroy reaches only tagged members and spares the loose neighbour")
{
    const std::filesystem::path root = FreshRoot("destroy");
    Write(root, "car.abp", CarFile());

    App::World world;
    // Dropping a loose entity beside an instance is legal and common — levels hold
    // ordinary entities alongside instances — but it is not a member.
    const ECS::Entity loose = world.scene.Create();
    (void)world.scene.Add(loose, ECS::Transform{});

    const std::optional<ECS::InstanceId> id = App::SpawnBlueprint(world, "car.abp", {});
    REQUIRE(id.has_value());
    CHECK(world.scene.AliveCount() == 3);

    CHECK(App::DestroyInstance(world, *id));
    world.scene.FlushDestroyed();

    CHECK(world.scene.AliveCount() == 1);
    CHECK(world.scene.Get<ECS::Transform>(loose) != nullptr);
    CHECK(App::FindInstance(world, *id) == nullptr);

    // And it is gone rather than half-gone: destroying twice is a clean no-op.
    CHECK_FALSE(App::DestroyInstance(world, *id));
}

TEST_CASE("Verbs: destroy takes the Jolt bodies with it, not just the components")
{
    // A Jolt body is a handle in the physics world, not the RigidBody component
    // that names it. Destroying the entity drops the component and leaves the body
    // simulating, so the car keeps colliding after every last trace of it has left
    // the scene — invisible to any assertion about entities, which is why the
    // existing destroy case stays green with the RemoveBody call deleted.
    const std::filesystem::path root = FreshRoot("destroy_bodies");
    Write(root, "car.abp", CarFile());

    App::World world;

    const std::optional<ECS::InstanceId> id = App::SpawnBlueprint(world, "car.abp", {});
    REQUIRE(id.has_value());

    // The car's body is a static half-metre box at the origin, so a ball dropped
    // down the y axis lands on top of it at about y = 0.75.
    constexpr float kStep = 1.f / 60.f;
    constexpr Physics::PhysicsWorld::ColliderShapeDesc kBall{.shape = Physics::ColliderShape::Sphere,
                                                             .radius = 0.25f};
    const Physics::RigidBody probe =
        world.physics.AddBody({0.f, 3.f, 0.f}, glm::quat(1.f, 0.f, 0.f, 0.f), kBall, Physics::BodyMotion::Dynamic);

    for (int32_t i = 0; i < 180; ++i)
        world.physics.Update(kStep);

    // The control. Without it the case below would pass on a world where the ball
    // never had anything to land on in the first place.
    REQUIRE(world.physics.GetBodyTransform(probe).first.y > 0.f);

    REQUIRE(App::DestroyInstance(world, *id));
    world.scene.FlushDestroyed();

    // Same ball, same drop, nothing left to catch it.
    world.physics.SetBodyTransform(probe, {0.f, 3.f, 0.f}, glm::quat(1.f, 0.f, 0.f, 0.f));
    world.physics.SetBodyLinearVelocity(probe, {0.f, 0.f, 0.f});
    for (int32_t i = 0; i < 180; ++i)
        world.physics.Update(kStep);

    CHECK(world.physics.GetBodyTransform(probe).first.y < -5.f);
}

TEST_CASE("Verbs: a pruned member lives on, and destroy no longer reaches it")
{
    const std::filesystem::path root = FreshRoot("prune");
    Write(root, "car.abp", CarFile());

    App::World world;
    const std::optional<ECS::InstanceId> id = App::SpawnBlueprint(world, "car.abp", {});
    REQUIRE(id.has_value());

    const ECS::Entity wheel = App::FindMember(world, *id, "wheel_fl");
    REQUIRE(wheel != ECS::NullEntity);

    CHECK(App::PruneFromInstance(world, wheel));
    CHECK_FALSE(App::PruneFromInstance(world, wheel)); // already loose

    // Everything about it except the tag is unchanged: it is an ordinary entity
    // now, and a system reading its components cannot tell the difference.
    CHECK(world.scene.Get<ECS::Transform>(wheel) != nullptr);
    CHECK(world.scene.Get<ECS::BlueprintMember>(wheel) == nullptr);
    CHECK(App::FindMember(world, *id, "wheel_fl") == ECS::NullEntity);

    CHECK(App::DestroyInstance(world, *id));
    world.scene.FlushDestroyed();
    CHECK(world.scene.AliveCount() == 1);
    CHECK(world.scene.Get<ECS::Transform>(wheel) != nullptr);
}

TEST_CASE("Verbs: explode ends the instance and destroys nothing")
{
    const std::filesystem::path root = FreshRoot("explode");
    Write(root, "car.abp", CarFile());

    App::World world;
    const std::optional<ECS::InstanceId> id = App::SpawnBlueprint(world, "car.abp", {});
    REQUIRE(id.has_value());

    CHECK(App::ExplodeInstance(world, *id));
    CHECK(world.scene.AliveCount() == 2);
    CHECK(TaggedCount(world.scene, *id) == 0);
    CHECK(App::FindInstance(world, *id) == nullptr);

    CHECK_FALSE(App::ExplodeInstance(world, *id));
}

TEST_CASE("Verbs: the id outlives a member's death; the handle does not")
{
    const std::filesystem::path root = FreshRoot("receipts");
    Write(root, "car.abp", CarFile());

    App::World world;
    const std::optional<ECS::InstanceId> id = App::SpawnBlueprint(world, "car.abp", {});
    REQUIRE(id.has_value());

    // Destroying one member directly — no blueprint semantics anywhere in the
    // engine's ordinary destroy path.
    const ECS::Entity wheel = App::FindMember(world, *id, "wheel_fl");
    world.scene.Destroy(wheel);
    world.scene.FlushDestroyed();

    // The id still answers, because the members are a query and one of them simply
    // is not found any more. A stored member list would be holding a dead handle.
    CHECK(App::FindInstance(world, *id) != nullptr);
    CHECK(App::FindMember(world, *id, "wheel_fl") == ECS::NullEntity);
    CHECK(App::FindMember(world, *id, "body") != ECS::NullEntity);

    CHECK(App::DestroyInstance(world, *id));
    world.scene.FlushDestroyed();
    CHECK(world.scene.AliveCount() == 0);
}

TEST_CASE("Verbs: two spawns of one file are separate instances")
{
    const std::filesystem::path root = FreshRoot("two");
    Write(root, "car.abp", CarFile());

    App::World world;
    const std::optional<ECS::InstanceId> first  = App::SpawnBlueprint(world, "car.abp", {});
    const std::optional<ECS::InstanceId> second = App::SpawnBlueprint(world, "car.abp", {});
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first != *second);

    CHECK(App::DestroyInstance(world, *first));
    world.scene.FlushDestroyed();

    // The other one is untouched, which is the point of an id rather than a type.
    CHECK(TaggedCount(world.scene, *second) == 2);
    CHECK(App::FindMember(world, *second, "body") != ECS::NullEntity);
}
