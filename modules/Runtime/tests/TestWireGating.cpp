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
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/NameComponent.hpp>

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
    for (const std::type_info *type : {&typeid(Assisi::ECS::Transform), &typeid(Assisi::Runtime::MeshRenderer),
                                       &typeid(Assisi::Runtime::Name)})
    {
        const Assisi::Core::Reflect::ComponentMeta *meta = MetaOf(*type);
        REQUIRE(meta != nullptr);
        CAPTURE(meta->name);
        CHECK(meta->replicated);
        // ACOMP(replicated) implies ACOMP(tracked). Without it the pool has no
        // change-tick lane, every query reports "unchanged", and the component
        // would be sent once at spawn and then never again — which is exactly
        // what MeshRenderer and Name did before they were marked.
        CHECK(meta->tracksChanges);
    }
}

TEST_CASE("Camera does not replicate")
{
    // The poster child for opt-in. Under "everything serializable travels" a
    // marked entity shipped its Camera, and `isActive` on the receiving side
    // meant the host could hand a client a different view than the one it chose.
    const Assisi::Core::Reflect::ComponentMeta *meta = MetaOf(typeid(Assisi::Runtime::Camera));
    REQUIRE(meta != nullptr);
    CHECK_FALSE(meta->replicated);
}

TEST_CASE("MeshRenderer puts its durable ids on the wire and its resolved pointers nowhere")
{
    const Assisi::Core::Reflect::ComponentMeta *meta = MetaOf(typeid(Assisi::Runtime::MeshRenderer));
    REQUIRE(meta != nullptr);

    // mesh + materialOverrides travel; meshBuffer + materials are transient
    // process-local pointers, and a resolved pointer is meaningless in another
    // process even when the id behind it is not.
    CHECK(Assisi::Core::Reflect::CountCodecFields(*meta) == 2);
    for (const Assisi::Core::Reflect::FieldMeta &field : meta->fields)
    {
        CAPTURE(field.name);
        const bool durable = field.name == "mesh" || field.name == "materialOverrides";
        CHECK(Assisi::Core::Reflect::IsWireField(field) == durable);
    }
}
