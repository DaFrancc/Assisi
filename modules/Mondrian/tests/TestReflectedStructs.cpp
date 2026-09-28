/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestReflectedStructs.cpp
/// @brief A field added to a struct deep inside a document shaped like a cooked
/// screen round-trips and moves the layout, with no line anywhere but the
/// struct naming it — which is what a cooked screen now gets from reflection.

#include <Assisi/Mondrian/TestReflectedStructs.hpp>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/StructMeta.hpp>

#include <doctest/doctest.h>

ASSISI_REFLECTED_STRUCT(Assisi::Mondrian::Testing::ProbeDocument);
ASSISI_REFLECTED_STRUCT(Assisi::Mondrian::Testing::GrownDocument);

using namespace Assisi::Mondrian::Testing;
using Assisi::Core::InternedString;
using Assisi::Core::Reflect::StructTraits;

namespace
{

/// Every field away from its default, the added one included.
GrownDocument MakeGrown()
{
    GrownDocument document;
    document.systems = {InternedString{"Probe"}};
    GrownNode node;
    node.style.sizes = {ProbeLength{.value = 2.f, .unit = ProbeUnit::Share},
                        ProbeLength{.value = 3.f, .unit = ProbeUnit::Pixels}};
    node.style.gap = ProbeLength{.value = 4.f, .unit = ProbeUnit::Share};
    node.style.scrolls = {false, true};
    node.style.opacity = 0.25f;
    node.text = document.pool.Add("probe");
    node.name = InternedString{"first"};
    node.parent = 7;
    document.nodes = {node, node};
    document.nodes[1].style.opacity = 0.75f;
    return document;
}

} // namespace

TEST_CASE("ReflectedStructs: a field added deep in a document round-trips with no other edit")
{
    const GrownDocument written = MakeGrown();

    Assisi::Core::BitWriter writer;
    REQUIRE(Assisi::Core::Reflect::WriteStruct(*StructTraits<GrownDocument>::Spec(), &written, writer));

    GrownDocument read;
    Assisi::Core::BitReader reader(writer.Data());
    REQUIRE(Assisi::Core::Reflect::ReadStruct(*StructTraits<GrownDocument>::Spec(), &read, reader));

    CHECK(read == written);
    CHECK(read.nodes[0].style.opacity == 0.25f);
    CHECK(read.nodes[1].style.opacity == 0.75f);
    CHECK(read.pool.View(read.nodes[1].text) == "probe");
}

TEST_CASE("ReflectedStructs: a field added deep in a document moves its layout hash")
{
    // The hash a cooked screen carries and its cook is keyed by: a blob written
    // before the field was added is refused rather than read into the wrong
    // fields, and the cook knows to write it again.
    CHECK(Assisi::Core::Reflect::StructLayoutHash(*StructTraits<ProbeDocument>::Spec()) !=
          Assisi::Core::Reflect::StructLayoutHash(*StructTraits<GrownDocument>::Spec()));

    const std::string grown = Assisi::Core::Reflect::StructLayoutDescription(*StructTraits<GrownDocument>::Spec());
    CHECK(grown.find("opacity f32") != std::string::npos);
}

TEST_CASE("ReflectedStructs: bytes written before a field was added do not read as the grown document")
{
    ProbeDocument before;
    before.nodes.emplace_back();
    before.nodes[0].parent = 3;

    Assisi::Core::BitWriter writer;
    REQUIRE(Assisi::Core::Reflect::WriteStruct(*StructTraits<ProbeDocument>::Spec(), &before, writer));

    // Without the hash, the reader walks the new layout over the old bytes: the
    // parent lands in the wrong place or the stream runs out. Either way it is
    // not the document that was written.
    GrownDocument read;
    Assisi::Core::BitReader reader(writer.Data());
    const bool accepted = Assisi::Core::Reflect::ReadStruct(*StructTraits<GrownDocument>::Spec(), &read, reader);
    CHECK_FALSE((accepted && read.nodes.size() == 1 && read.nodes[0].parent == 3));
}
