/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestEntityRef.cpp
/// @brief A reflected entity field serializes through whichever entity-reference
/// codec is installed on the thread, and through none when none is.

#include <doctest/doctest.h>

#include <cstdint>

#include <nlohmann/json.hpp>

#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/EntityRef.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/TestComponents.hpp>

using namespace Assisi;

namespace
{

/// What the test codec writes for every entity, so a field that went through it
/// is told apart from one written any other way.
constexpr uint32_t kSentinel = 4242;

/// The entity the test codec reads every sentinel back as.
constexpr ECS::Entity kResolved{.index = 7, .generation = 3};

nlohmann::json SentinelToJson(ECS::Entity)
{
    return nlohmann::json(kSentinel);
}

ECS::Entity SentinelFromJson(const nlohmann::json &value)
{
    if (value.is_number_unsigned() && value.get<uint32_t>() == kSentinel)
    {
        return kResolved;
    }
    return ECS::NullEntity;
}

const Core::Reflect::ComponentMeta &LinkMeta()
{
    const Core::Reflect::ComponentMeta *meta = Core::Reflect::ComponentRegistry::Instance().Find("Link");
    REQUIRE(meta != nullptr);
    return *meta;
}

} // namespace

TEST_CASE("EntityRef: with no codec installed an entity field writes null and reads back as NullEntity")
{
    const Core::Reflect::ComponentMeta &meta = LinkMeta();
    const ECS::Link written{.target = ECS::Entity{.index = 1, .generation = 0}};

    const nlohmann::json json = meta.serialize(&written);
    CHECK(json.at("target").is_null());

    ECS::Scene scene;
    const ECS::Entity holder = scene.Create();
    REQUIRE(meta.addToScene(&scene, holder.index, holder.generation, json));
    const ECS::Link *read = scene.Get<ECS::Link>(holder);
    REQUIRE(read != nullptr);
    CHECK(read->target == ECS::NullEntity);
}

TEST_CASE("EntityRef: an installed codec carries an entity field, and its scope puts the previous one back")
{
    const Core::Reflect::ComponentMeta &meta = LinkMeta();
    const ECS::Link written{.target = ECS::Entity{.index = 1, .generation = 0}};

    nlohmann::json json;
    {
        const ECS::ScopedEntityRefCodec codec(ECS::EntityRefCodec{SentinelToJson, SentinelFromJson});
        json = meta.serialize(&written);
        CHECK(json.at("target") == nlohmann::json(kSentinel));

        ECS::Scene scene;
        const ECS::Entity holder = scene.Create();
        REQUIRE(meta.addToScene(&scene, holder.index, holder.generation, json));
        const ECS::Link *read = scene.Get<ECS::Link>(holder);
        REQUIRE(read != nullptr);
        CHECK(read->target == kResolved);
    }

    // Out of the scope: the field is back to what no codec writes.
    CHECK(meta.serialize(&written).at("target").is_null());
    CHECK(ECS::EntityRefFromJson(json) == ECS::NullEntity);
}

TEST_CASE("EntityRef: a null entity writes null even under a codec")
{
    const ECS::ScopedEntityRefCodec codec(ECS::EntityRefCodec{SentinelToJson, SentinelFromJson});
    CHECK(ECS::EntityRefToJson(ECS::NullEntity).is_null());
    CHECK(ECS::EntityRefFromJson(nlohmann::json(nullptr)) == ECS::NullEntity);
}

TEST_CASE("EntityRef: nested codec scopes restore the outer codec, not none")
{
    const ECS::ScopedEntityRefCodec outer(ECS::EntityRefCodec{SentinelToJson, SentinelFromJson});
    {
        const ECS::ScopedEntityRefCodec inner(
            ECS::EntityRefCodec{[](ECS::Entity) -> nlohmann::json { return nlohmann::json("inner"); },
                                [](const nlohmann::json &) -> ECS::Entity { return ECS::NullEntity; }});
        CHECK(ECS::EntityRefToJson(kResolved) == nlohmann::json("inner"));
    }
    CHECK(ECS::EntityRefToJson(kResolved) == nlohmann::json(kSentinel));
}
