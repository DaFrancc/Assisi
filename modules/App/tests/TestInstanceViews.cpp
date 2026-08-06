/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestInstanceViews.cpp
/// @brief The generated typed view, checked against the runtime it describes.
///
/// The Python tests next to the generator pin what it *emits*. This pins the
/// only thing that actually matters at the far end: that a generated field and
/// the member name a spawn produces are the same entity. They are produced by
/// two different implementations of one set of naming rules — reflectgen's
/// blueprint_views.py at build time, Blueprint.cpp's FlattenInto at run time —
/// and if those ever drift, the field resolves to NullEntity and a game gets a
/// handle to nothing. That failure is invisible: it compiles, it spawns, and the
/// car simply has no wheels.
///
/// So every case here compares the typed path against the stringly one. The
/// fixtures are the *same files the build generated from*, reached through
/// ASSISI_INSTANCE_VIEW_ASSETS, because a copy of them would drift too.

#include <doctest/doctest.h>

#include <ostream>

#include <filesystem>
#include <optional>

#include <Assisi/App/BlueprintVerbs.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/InstanceViews.hpp>

using namespace Assisi;

namespace
{

/// The directory the build generated the views from. Handed in by CMake rather
/// than written down twice: a test that read its own copy of the fixtures would
/// pass while the real files said something else, which is the one failure this
/// file exists to catch.
void UseGeneratedAssets()
{
    REQUIRE(Core::AssetSystem::SetRoot(std::filesystem::path{ASSISI_INSTANCE_VIEW_ASSETS}).has_value());
    Runtime::ClearBlueprintCache();
}

} // namespace

TEST_CASE("Instance view: every generated field is the member of that name")
{
    UseGeneratedAssets();

    App::World     world;
    ECS::Transform at;
    at.position = {10.f, 0.f, 0.f};

    const auto lot = App::SpawnBlueprint<Blueprints::ParkingLot>(world, at);
    REQUIRE(lot.has_value());

    // The agreement, member by member. Nothing here reads a field without
    // checking it against the name the runtime knows it by.
    CHECK(lot->sign == App::FindMember(world, lot->instanceId, "sign"));
    CHECK(lot->car_body == App::FindMember(world, lot->instanceId, "car_body"));
    CHECK(lot->car.body == App::FindMember(world, lot->instanceId, "car/body"));
    CHECK(lot->car.wheel_fl == App::FindMember(world, lot->instanceId, "car/wheel_fl"));
    CHECK(lot->car.wheel_fr == App::FindMember(world, lot->instanceId, "car/wheel_fr"));
    CHECK(lot->spare.body == App::FindMember(world, lot->instanceId, "spare/body"));
    CHECK(lot->spare.wheel_fl == App::FindMember(world, lot->instanceId, "spare/wheel_fl"));

    // ...and none of them is the null handle a drifted name would produce.
    CHECK(lot->sign != ECS::NullEntity);
    CHECK(lot->car_body != ECS::NullEntity);
    CHECK(lot->car.body != ECS::NullEntity);
    CHECK(lot->car.wheel_fl != ECS::NullEntity);
    CHECK(lot->car.wheel_fr != ECS::NullEntity);
    CHECK(lot->spare.body != ECS::NullEntity);
    CHECK(lot->spare.wheel_fl != ECS::NullEntity);
}

TEST_CASE("Instance view: a top-level member and a nested one of the same name stay apart")
{
    UseGeneratedAssets();

    App::World world;
    const auto lot = App::SpawnBlueprint<Blueprints::ParkingLot>(world, {});
    REQUIRE(lot.has_value());

    // The whole reason the view nests instead of flattening to `car_body`: the
    // lot declares an entity by that name *and* an instance `car` with a member
    // `body`. Flattening would have produced two fields with one name.
    CHECK(lot->car_body != lot->car.body);
}

TEST_CASE("Instance view: two instances of one file are separate members")
{
    UseGeneratedAssets();

    App::World world;
    const auto lot = App::SpawnBlueprint<Blueprints::ParkingLot>(world, {});
    REQUIRE(lot.has_value());

    CHECK(lot->car.body != lot->spare.body);

    // `spare` removed a wheel, so the generated struct has no `spare.wheel_fr`
    // at all — the removal is enforced by the field not existing, which is a
    // compile error at any call site rather than a null at runtime. Its sibling
    // survived, and that is what is checkable here.
    CHECK(lot->spare.wheel_fl != ECS::NullEntity);
    CHECK(App::FindMember(world, lot->instanceId, "spare/wheel_fr") == ECS::NullEntity);
}

TEST_CASE("Instance view: the placement composes exactly as the untyped path composes it")
{
    UseGeneratedAssets();

    App::World     world;
    ECS::Transform at;
    at.position = {100.f, 0.f, 0.f};

    const auto lot = App::SpawnBlueprint<Blueprints::ParkingLot>(world, at);
    REQUIRE(lot.has_value());

    // The nested instance sits at +4 in the lot's space, so its body lands at
    // 104 — the typed path is the same expansion, not a second one.
    const ECS::Transform *body = world.scene.Get<ECS::Transform>(lot->car.body);
    REQUIRE(body != nullptr);
    CHECK(body->position.x == doctest::Approx(104.f));
}

TEST_CASE("Instance view: FindInstance<T> re-resolves the same members")
{
    UseGeneratedAssets();

    App::World world;
    const auto spawned = App::SpawnBlueprint<Blueprints::ParkingLot>(world, {});
    REQUIRE(spawned.has_value());

    // The id is what may be kept; this is how a caller holding only that gets
    // handles again.
    const auto found = App::FindInstance<Blueprints::ParkingLot>(world, spawned->instanceId);
    REQUIRE(found.has_value());
    CHECK(found->instanceId == spawned->instanceId);
    CHECK(found->sign == spawned->sign);
    CHECK(found->car.body == spawned->car.body);
}

TEST_CASE("Instance view: a view is refused over an instance of another blueprint")
{
    UseGeneratedAssets();

    App::World world;
    const auto lot = App::SpawnBlueprint<Blueprints::ParkingLot>(world, {});
    REQUIRE(lot.has_value());

    // Without the source check this would return a Car whose every field was
    // NullEntity — a spawn that looks like it half-worked. This is why the
    // instance table has to exist.
    CHECK_FALSE(App::FindInstance<Blueprints::Car>(world, lot->instanceId).has_value());
    CHECK_FALSE(App::FindInstance<Blueprints::ParkingLot>(world, ECS::InstanceId{999}).has_value());
}

TEST_CASE("Instance view: a typed spawn takes no string, and names its own file")
{
    UseGeneratedAssets();

    App::World world;
    const auto car = App::SpawnBlueprint<Blueprints::Car>(world, {});
    REQUIRE(car.has_value());

    // The source came from the traits, so the file the view was generated from
    // and the file it spawned cannot be different files.
    CHECK(App::FindInstance(world, car->instanceId, "car.abp") != nullptr);
    CHECK(App::FindInstance(world, car->instanceId, "parking_lot.abp") == nullptr);
}

TEST_CASE("Instance view: a member that dies leaves the id answering and the field null")
{
    UseGeneratedAssets();

    App::World world;
    const auto car = App::SpawnBlueprint<Blueprints::Car>(world, {});
    REQUIRE(car.has_value());

    world.scene.Destroy(car->wheel_fl);
    world.scene.FlushDestroyed();

    // The handles in the old view are stale — that is the receipt rule, and why
    // only instanceId may outlive the call. Asking again is how you find out.
    const auto again = App::FindInstance<Blueprints::Car>(world, car->instanceId);
    REQUIRE(again.has_value());
    CHECK(again->body != ECS::NullEntity);
    CHECK(again->wheel_fl == ECS::NullEntity);
}
