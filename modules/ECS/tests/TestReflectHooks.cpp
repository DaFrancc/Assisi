/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestReflectHooks.cpp
/// @brief ComponentMeta's type-erased scene hooks — the generic path a binary
/// consumer (replication) uses when it has metadata and no compile-time type.
///
/// These live in the ECS suite rather than Core's because they can only be
/// exercised against a real Scene, and Core deliberately does not depend on ECS.

#include <doctest/doctest.h>

#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/TestComponents.hpp>

using namespace Assisi;

namespace
{

const Core::Reflect::ComponentMeta &MetaOf(std::string_view name)
{
    const Core::Reflect::ComponentMeta *meta = Core::Reflect::ComponentRegistry::Instance().Find(name);
    REQUIRE(meta != nullptr);
    return *meta;
}

} // namespace

TEST_CASE("construct default-builds a component and returns a writable pointer")
{
    ECS::Scene scene;
    const ECS::Entity entity = scene.Create();

    const Core::Reflect::ComponentMeta &meta = MetaOf("Position");
    REQUIRE(meta.construct != nullptr);

    CHECK(scene.Get<ECS::Position>(entity) == nullptr);

    void *created = meta.construct(&scene, entity.index, entity.generation);
    REQUIRE(created != nullptr);
    CHECK(scene.Get<ECS::Position>(entity) == created);

    // Writable, and writing through it is visible through the typed accessor —
    // this is the whole reason the hook exists.
    static_cast<ECS::Position *>(created)->x = 7.f;
    CHECK(scene.Get<ECS::Position>(entity)->x == doctest::Approx(7.f));
}

TEST_CASE("construct replaces an existing component with a default one")
{
    ECS::Scene scene;
    const ECS::Entity entity = scene.Create();

    ECS::Position seeded;
    seeded.x = 42.f;
    (void)scene.Add<ECS::Position>(entity, seeded);

    const Core::Reflect::ComponentMeta &meta = MetaOf("Position");
    void *rebuilt = meta.construct(&scene, entity.index, entity.generation);
    REQUIRE(rebuilt != nullptr);

    // "Construct" means default-valued, not "leave whatever was there". A
    // client materializing an entity from a snapshot needs the known-empty
    // starting point, since the payload only carries the fields that changed.
    CHECK(static_cast<ECS::Position *>(rebuilt)->x == doctest::Approx(ECS::Position{}.x));
}

TEST_CASE("getMutable finds what getByEntity finds, and nothing when absent")
{
    ECS::Scene scene;
    const ECS::Entity entity  = scene.Create();
    const ECS::Entity without = scene.Create();

    const Core::Reflect::ComponentMeta &meta = MetaOf("Position");
    REQUIRE(meta.getMutable != nullptr);

    CHECK(meta.getMutable(&scene, without.index, without.generation) == nullptr);

    (void)scene.Add<ECS::Position>(entity, ECS::Position{});
    void *mutablePtr = meta.getMutable(&scene, entity.index, entity.generation);
    const void *constPtr   = meta.getByEntity(&scene, entity.index, entity.generation);
    REQUIRE(mutablePtr != nullptr);
    CHECK(mutablePtr == constPtr);
}

TEST_CASE("the writing hooks stamp change detection and the reading one does not")
{
    ECS::Scene scene;
    const ECS::Entity entity = scene.Create();

    // Tracked is the ACOMP(tracked) fixture; an untracked type would report
    // nothing either way and prove nothing.
    const Core::Reflect::ComponentMeta &meta = MetaOf("Tracked");
    REQUIRE(meta.construct != nullptr);

    (void)meta.construct(&scene, entity.index, entity.generation);
    const std::uint64_t afterConstruct = scene.CurrentChangeTick();
    CHECK(scene.ChangeTick<ECS::Tracked>(entity) == afterConstruct);

    // Reading must not look like a write, or every consumer of Changed() would
    // see the whole world change every time anything inspected it.
    (void)meta.getByEntity(&scene, entity.index, entity.generation);
    CHECK(scene.CurrentChangeTick() == afterConstruct);
    CHECK_FALSE(scene.Changed<ECS::Tracked>(entity, afterConstruct));

    (void)meta.getMutable(&scene, entity.index, entity.generation);
    CHECK(scene.Changed<ECS::Tracked>(entity, afterConstruct));
}

TEST_CASE("a transient component carries no scene hooks at all")
{
    // ACOMP(transient) registers only to receive a stable ComponentId. Every
    // hook is null, and `serializable` is the documented gate — a consumer must
    // check that rather than probing each hook.
    const Core::Reflect::ComponentMeta &meta = MetaOf("TransientTag");
    CHECK_FALSE(meta.serializable);
    CHECK(meta.serialize == nullptr);
    CHECK(meta.addToScene == nullptr);
    CHECK(meta.iterateEntities == nullptr);
    CHECK(meta.getByEntity == nullptr);
    CHECK(meta.construct == nullptr);
    CHECK(meta.getMutable == nullptr);
}
