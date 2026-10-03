/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <array>
#include <optional>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/TestComponents.hpp>
#include <Assisi/Testing/ThrowOnContractViolation.hpp>

using namespace Assisi::ECS;
using Assisi::Core::Reflect::ComponentId;
using Assisi::Core::Reflect::ComponentIdOf;
using Assisi::Core::Reflect::kInvalidComponentId;

namespace
{
/// True when Add<T> is refused the way a broken rule is: an assert in debug
/// builds, a null result in release.
template <typename T> bool AddIsRefused(Scene &scene, Entity entity)
{
#ifndef NDEBUG
    Assisi::Testing::ThrowOnContractViolation guard;
    try
    {
        (void)scene.Add<T>(entity);
    }
    catch (const Assisi::Core::ContractViolation &)
    {
        return true;
    }
    return false;
#else
    return scene.Add<T>(entity) == nullptr;
#endif
}

/// True when Remove<T> is refused because something on the entity requires T.
template <typename T> bool RemoveIsRefused(Scene &scene, Entity entity)
{
#ifndef NDEBUG
    Assisi::Testing::ThrowOnContractViolation guard;
    try
    {
        (void)scene.Remove<T>(entity);
    }
    catch (const Assisi::Core::ContractViolation &)
    {
        return true;
    }
    return false;
#else
    return scene.Remove<T>(entity) == RemoveResult::Required;
#endif
}
} // namespace

TEST_CASE("Add brings the components a component requires")
{
    Scene scene;
    const Entity entity = scene.Create();

    SUBCASE("directly")
    {
        REQUIRE(scene.Add<Needs>(entity) != nullptr);
        CHECK(scene.Has<Position>(entity));
    }

    SUBCASE("through a requirement's own requirements")
    {
        REQUIRE(scene.Add<NeedsNeeds>(entity) != nullptr);
        CHECK(scene.Has<Needs>(entity));
        CHECK(scene.Has<Position>(entity));
    }

    SUBCASE("a transient one too")
    {
        REQUIRE(scene.Add<NeedsGhost>(entity) != nullptr);
        CHECK(scene.Has<Ghost>(entity));
    }

    SUBCASE("leaving one already there as it is")
    {
        constexpr float kAuthored = 7.0f;
        REQUIRE(scene.Add<Position>(entity, Position{kAuthored}) != nullptr);
        REQUIRE(scene.Add<Needs>(entity) != nullptr);
        CHECK(scene.Get<Position>(entity)->x == doctest::Approx(kAuthored));
    }
}

TEST_CASE("Add refuses a component an entity's others exclude")
{
    Scene scene;
    const Entity entity = scene.Create();

    SUBCASE("declared on the component being added")
    {
        REQUIRE(scene.Add<Position>(entity) != nullptr);
        CHECK(AddIsRefused<Shuns>(scene, entity));
        CHECK_FALSE(scene.Has<Shuns>(entity));
    }

    SUBCASE("declared on the component already there")
    {
        REQUIRE(scene.Add<Shuns>(entity) != nullptr);
        CHECK(AddIsRefused<Position>(scene, entity));
        CHECK_FALSE(scene.Has<Position>(entity));
    }

    SUBCASE("through a requirement, adding nothing at all")
    {
        REQUIRE(scene.Add<Position>(entity) != nullptr);
        CHECK(AddIsRefused<NeedsShuns>(scene, entity));
        CHECK_FALSE(scene.Has<NeedsShuns>(entity));
        CHECK_FALSE(scene.Has<Shuns>(entity));
    }

    SUBCASE("excluded by a requirement an earlier add brought")
    {
        REQUIRE(scene.Add<NeedsShuns>(entity) != nullptr);
        CHECK(AddIsRefused<Position>(scene, entity));
        CHECK_FALSE(scene.Has<Position>(entity));
    }

    SUBCASE("a Parent under a component that excludes it")
    {
        REQUIRE(scene.Add<Rooted>(entity) != nullptr);
        CHECK(AddIsRefused<Parent>(scene, entity));
        CHECK_FALSE(scene.Has<Parent>(entity));
    }
}

TEST_CASE("ConflictOf names the clash without refusing anything")
{
    Scene scene;
    const Entity entity = scene.Create();
    REQUIRE(scene.Add<Position>(entity) != nullptr);

    const std::optional<ComponentConflict> direct = scene.ConflictOf(entity, ComponentIdOf<Shuns>());
    REQUIRE(direct.has_value());
    CHECK(direct->wanted == ComponentIdOf<Shuns>());
    CHECK(direct->present == ComponentIdOf<Position>());

    // The requirement is what clashes, so it is the one named.
    const std::optional<ComponentConflict> pulled = scene.ConflictOf(entity, ComponentIdOf<NeedsShuns>());
    REQUIRE(pulled.has_value());
    CHECK(pulled->wanted == ComponentIdOf<Shuns>());

    CHECK_FALSE(scene.ConflictOf(entity, ComponentIdOf<Needs>()).has_value());
}

TEST_CASE("Removing a requirement is refused while its requirer stays")
{
    Scene scene;
    const Entity entity = scene.Create();
    REQUIRE(scene.Add<NeedsNeeds>(entity) != nullptr);

    CHECK(scene.RequirerOf(entity, ComponentIdOf<Position>()) != kInvalidComponentId);
    CHECK(RemoveIsRefused<Position>(scene, entity));
    CHECK(scene.Has<Position>(entity));

    // By id, as loaders and tooling remove, the refusal is reported, not asserted.
    CHECK(scene.RemoveById(entity, ComponentIdOf<Needs>()) == RemoveResult::Required);
    CHECK(scene.Has<Needs>(entity));

    SUBCASE("removing the requirer leaves its requirements")
    {
        CHECK(scene.Remove<NeedsNeeds>(entity) == RemoveResult::Removed);
        CHECK(scene.Has<Needs>(entity));
        CHECK(scene.Remove<Needs>(entity) == RemoveResult::Removed);
        CHECK(scene.Remove<Position>(entity) == RemoveResult::Removed);
        CHECK(scene.Remove<Position>(entity) == RemoveResult::Absent);
    }

    SUBCASE("RemoveManyById removes a whole set in any order")
    {
        const std::array<ComponentId, 3> all{ComponentIdOf<Position>(), ComponentIdOf<Needs>(),
                                             ComponentIdOf<NeedsNeeds>()};
        CHECK(scene.RemoveManyById(entity, all));
        CHECK_FALSE(scene.Has<Position>(entity));
        CHECK_FALSE(scene.Has<Needs>(entity));
        CHECK_FALSE(scene.Has<NeedsNeeds>(entity));
    }

    SUBCASE("RemoveManyById leaves a requirement whose requirer is not in the set")
    {
        const std::array<ComponentId, 1> partial{ComponentIdOf<Position>()};
        CHECK_FALSE(scene.RemoveManyById(entity, partial));
        CHECK(scene.Has<Position>(entity));
    }

    SUBCASE("destroying the entity takes everything")
    {
        scene.Destroy(entity);
        scene.FlushDestroyed();
        CHECK_FALSE(scene.IsAlive(entity));
        CHECK(scene.ComponentCount(ComponentIdOf<Position>()) == 0);
    }
}
