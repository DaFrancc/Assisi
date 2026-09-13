/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestWireGating.cpp
/// @brief Which render-facing components opt into replication, asserted against
/// the registry rather than against the headers.
///
/// Runtime is where the interesting *answers* live — MeshRenderer must travel or
/// mirrors draw nothing, Camera must not or a joining client's view is taken over
/// by whoever it joined — but the mechanism is exercised end-to-end in
/// modules/NetSync/tests. Here the question is only whether the annotations say
/// what the design decided, which is a registry lookup: NetSync deliberately does
/// not link Runtime, so no session-level test can ask it.

#include <doctest/doctest.h>

#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/Angles.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/LightComponents.hpp>
#include <Assisi/Runtime/NameComponent.hpp>

#include <algorithm>
#include <string_view>
#include <typeindex>

namespace
{

const Assisi::Core::Reflect::ComponentMeta *MetaOf(const std::type_info &type)
{
    Assisi::Core::Reflect::ComponentRegistry &registry = Assisi::Core::Reflect::ComponentRegistry::Instance();
    return registry.ById(registry.IdOf(type));
}

} // namespace

TEST_CASE("the components a mirror needs to exist and to draw are replicated")
{
    for (const std::type_info *type :
         {&typeid(Assisi::ECS::Transform), &typeid(Assisi::Runtime::MeshRenderer), &typeid(Assisi::Runtime::Name)})
    {
        const Assisi::Core::Reflect::ComponentMeta *meta = MetaOf(*type);
        REQUIRE(meta != nullptr);
        CAPTURE(meta->name);
        CHECK(meta->replicable);
        // ACOMP(replicable) implies ACOMP(tracked). Without it the pool has no
        // change-tick lane, every query reports "unchanged", and the component
        // goes out once at spawn and then never again.
        CHECK(meta->tracksChanges);
    }
}

TEST_CASE("Camera does not replicate")
{
    // The poster child for opt-in: an arriving `isActive` would let the host hand
    // a client a different view than the one that client chose.
    const Assisi::Core::Reflect::ComponentMeta *meta = MetaOf(typeid(Assisi::Runtime::Camera));
    REQUIRE(meta != nullptr);
    CHECK_FALSE(meta->replicable);
}

TEST_CASE("MeshRenderer puts its durable ids on the wire and its resolved pointers nowhere")
{
    const Assisi::Core::Reflect::ComponentMeta *meta = MetaOf(typeid(Assisi::Runtime::MeshRenderer));
    REQUIRE(meta != nullptr);

    // mesh + materialOverrides + castsShadows travel; meshBuffer + materials are
    // transient process-local pointers, and a resolved pointer is meaningless in
    // another process even when the id behind it is not.
    CHECK(Assisi::Core::Reflect::CountCodecFields(*meta) == 3);
    for (const Assisi::Core::Reflect::FieldMeta &field : meta->fields)
    {
        CAPTURE(field.name);
        const bool durable = field.name == "mesh" || field.name == "materialOverrides" || field.name == "castsShadows";
        CHECK(Assisi::Core::Reflect::IsWireField(field) == durable);
    }
}

TEST_CASE("a spot cone cannot be authored wider than its shadow map can be built for")
{
    // Two ceilings on one angle, and they must be the same one. Everything that
    // builds something from the cone clamps to Math::kMaxConeHalfAngleDegrees,
    // because a rim's radius and a frustum's tan(fov/2) both diverge at a right
    // angle; the inspector clamps to a literal, because an AFIELD bound takes a
    // number or a sibling field and cannot name a constant. Let them drift and a
    // cone is authorable whose shadow stops before its light does.
    const Assisi::Core::Reflect::ComponentMeta *meta = MetaOf(typeid(Assisi::Runtime::SpotLight));
    REQUIRE(meta != nullptr);

    const auto outer =
        std::find_if(meta->fields.begin(), meta->fields.end(), [](const Assisi::Core::Reflect::FieldMeta &field)
                     { return field.name == std::string_view("outerAngle"); });
    REQUIRE(outer != meta->fields.end());
    REQUIRE(outer->hasMax);
    CHECK(outer->maxValue == doctest::Approx(Assisi::Math::kMaxConeHalfAngleDegrees));
}
