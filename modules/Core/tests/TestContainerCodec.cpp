/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestContainerCodec.cpp
/// @brief Reflected containers through the binary codec: every permitted shape
/// round-trips, a map encodes in sorted key order whatever order it iterates in,
/// the layout text tells two container shapes apart, and a hostile count is
/// refused before anything is allocated.
///
/// The ComponentMeta is hand-built for the same reason TestBinaryCodec's is: this
/// suite must fail when the codec breaks, not when an engine component changes
/// shape. The specs come from ContainerSpecFor, which is what reflectgen emits.

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/ContainerOps.hpp>
#include <Assisi/Core/ShortString.hpp>

using Assisi::Core::BitReader;
using Assisi::Core::BitWriter;
using Assisi::Core::ShortString;
using Assisi::Core::Reflect::ComponentId;
using Assisi::Core::Reflect::ComponentMeta;
using Assisi::Core::Reflect::ContainerSpecFor;
using Assisi::Core::Reflect::FieldMeta;
using Assisi::Core::Reflect::FieldType;
using Assisi::Core::Reflect::kAllFields;
using Assisi::Core::Reflect::kMaxVectorElements;
using Assisi::Core::Reflect::ProtocolLayoutDescription;
using Assisi::Core::Reflect::ReadComponent;
using Assisi::Core::Reflect::ReadComponentId;
using Assisi::Core::Reflect::WriteComponent;

namespace
{

enum class Colour : std::uint8_t
{
    Red   = 0,
    Green = 7,
    Blue  = 9,
};

using BindingTable = std::unordered_map<ShortString, std::vector<Colour>>;

/// One component carrying every container shape the design permits: a flat
/// vector of each element kind, both map flavours, and the one nested level.
struct Containers
{
    std::vector<std::int32_t> numbers;
    std::vector<Colour> colours;
    std::vector<ShortString> labels;
    std::map<std::int32_t, float> weights;
    std::unordered_map<ShortString, std::int32_t> counts;
    BindingTable bindings;
};

template <typename T, typename M> std::size_t OffsetOf(M T::*member)
{
    static const T prototype{};
    return static_cast<std::size_t>(reinterpret_cast<const std::byte *>(&(prototype.*member)) -
                                    reinterpret_cast<const std::byte *>(&prototype));
}

/// The FieldMeta reflectgen would emit for a container member: the type comes
/// from the spec's own kind, so the test cannot disagree with the descriptor.
template <typename C> FieldMeta ContainerField(const char *name, std::size_t offset)
{
    FieldMeta field;
    field.name      = name;
    field.container = ContainerSpecFor<C>();
    field.offset    = offset;
    field.type      = Assisi::Core::Reflect::ContainerDepth<C>::kind;
    return field;
}

/// Enum metadata rides on the container field and describes its leaf element.
FieldMeta ColourField(const char *name, std::size_t offset, FieldMeta field)
{
    field.name          = name;
    field.offset        = offset;
    field.enumSize      = sizeof(Colour);
    field.enumSigned    = false;
    field.enumConstants = {{"Red", 0}, {"Green", 7}, {"Blue", 9}};
    return field;
}

ComponentMeta MakeContainersMeta()
{
    ComponentMeta meta{.name            = "Containers",
                       .typeIndex       = std::type_index(typeid(Containers)),
                       .fields          = {},
                       .serialize       = {},
                       .addToScene      = {},
                       .iterateEntities = {},
                       .getByEntity     = {},
                       .construct       = {},
                       .getMutable      = {},
                       .serializable    = true,
                       .tracksChanges   = true,
                       .replicable      = true,
                       .id              = ComponentId{11}};

    meta.fields.push_back(
        ContainerField<std::vector<std::int32_t>>("numbers", OffsetOf(&Containers::numbers)));
    meta.fields.push_back(ColourField("colours", OffsetOf(&Containers::colours),
                                      ContainerField<std::vector<Colour>>("colours", 0)));
    meta.fields.push_back(
        ContainerField<std::vector<ShortString>>("labels", OffsetOf(&Containers::labels)));
    meta.fields.push_back(
        ContainerField<std::map<std::int32_t, float>>("weights", OffsetOf(&Containers::weights)));
    meta.fields.push_back(
        ContainerField<std::unordered_map<ShortString, std::int32_t>>("counts",
                                                                      OffsetOf(&Containers::counts)));
    meta.fields.push_back(ColourField("bindings", OffsetOf(&Containers::bindings),
                                      ContainerField<BindingTable>("bindings", 0)));
    return meta;
}

ShortString Str(std::string_view text)
{
    return ShortString{text};
}

Containers MakePopulated()
{
    Containers value;
    value.numbers = {-7, 0, 13, 2147483647};
    value.colours = {Colour::Blue, Colour::Red, Colour::Green};
    value.labels  = {Str("alpha"), Str(""), Str("omega")};
    value.weights = {{-1, 0.5f}, {4, -2.25f}, {9, 1024.f}};
    value.counts  = {{Str("one"), 1}, {Str("two"), 2}};

    // The shape the whole nesting level exists for: one action, several inputs.
    value.bindings[Str("MoveForward")] = {Colour::Red, Colour::Blue};
    value.bindings[Str("Jump")]        = {Colour::Green};
    value.bindings[Str("Crouch")]      = {};
    return value;
}

bool RoundTrip(const ComponentMeta &meta, const Containers &source, Containers &destination)
{
    BitWriter writer;
    if (!WriteComponent(meta, &source, writer, kAllFields))
    {
        return false;
    }
    BitReader reader(writer.Data());
    if (ReadComponentId(reader) != meta.id)
    {
        return false;
    }
    return ReadComponent(meta, &destination, reader) && reader.Ok();
}

std::vector<std::byte> Encode(const ComponentMeta &meta, const Containers &value)
{
    BitWriter writer;
    REQUIRE(WriteComponent(meta, &value, writer, kAllFields));
    const std::span<const std::byte> bytes = writer.Data();
    return {bytes.begin(), bytes.end()};
}

} // namespace

TEST_CASE("ContainerCodec: every permitted container shape round-trips")
{
    const ComponentMeta meta = MakeContainersMeta();
    const Containers source  = MakePopulated();
    Containers decoded;

    REQUIRE(RoundTrip(meta, source, decoded));

    CHECK(decoded.numbers == source.numbers);
    CHECK(decoded.colours == source.colours);
    CHECK(decoded.labels == source.labels);
    CHECK(decoded.weights == source.weights);
    CHECK(decoded.counts == source.counts);
    CHECK(decoded.bindings == source.bindings);

    // The nested level specifically: a key whose value is a list, including the
    // empty list, which is the case a "skip falsy values" bug would drop.
    REQUIRE(decoded.bindings.size() == 3);
    CHECK(decoded.bindings.at(Str("MoveForward")) == std::vector<Colour>{Colour::Red, Colour::Blue});
    CHECK(decoded.bindings.at(Str("Jump")) == std::vector<Colour>{Colour::Green});
    CHECK(decoded.bindings.at(Str("Crouch")).empty());
}

TEST_CASE("ContainerCodec: decoding clears what was there rather than appending")
{
    const ComponentMeta meta = MakeContainersMeta();
    const Containers source  = MakePopulated();

    // A destination already holding more entries than the stream carries. Without
    // the clear, the tail of the old value survives and the decoded vector is
    // longer than the one that was sent.
    Containers decoded;
    decoded.numbers = {1, 2, 3, 4, 5, 6, 7, 8};
    decoded.labels  = {Str("stale"), Str("stale"), Str("stale")};
    decoded.counts  = {{Str("gone"), 99}};

    REQUIRE(RoundTrip(meta, source, decoded));

    CHECK(decoded.numbers == source.numbers);
    CHECK(decoded.labels == source.labels);
    CHECK(decoded.counts == source.counts);
    CHECK(decoded.counts.find(Str("gone")) == decoded.counts.end());
}

TEST_CASE("ContainerCodec: a map encodes in key order, not in iteration order")
{
    const ComponentMeta meta = MakeContainersMeta();

    // Same contents, opposite insertion order, and one forced to rehash. An
    // unordered_map iterates in bucket order, so all three of these iterate
    // differently — and must still produce identical bytes, or the same component
    // encodes differently between two runs and delta replication breaks.
    Containers forward;
    forward.counts.emplace(Str("alpha"), 1);
    forward.counts.emplace(Str("mike"), 2);
    forward.counts.emplace(Str("zulu"), 3);

    Containers backward;
    backward.counts.emplace(Str("zulu"), 3);
    backward.counts.emplace(Str("mike"), 2);
    backward.counts.emplace(Str("alpha"), 1);

    Containers rehashed;
    rehashed.counts.reserve(512);
    rehashed.counts.emplace(Str("mike"), 2);
    rehashed.counts.emplace(Str("zulu"), 3);
    rehashed.counts.emplace(Str("alpha"), 1);

    const std::vector<std::byte> a = Encode(meta, forward);
    const std::vector<std::byte> b = Encode(meta, backward);
    const std::vector<std::byte> c = Encode(meta, rehashed);

    CHECK(a == b);
    CHECK(a == c);
}

TEST_CASE("ContainerCodec: a nested map encodes in key order too")
{
    const ComponentMeta meta = MakeContainersMeta();

    Containers forward;
    forward.bindings.emplace(Str("Attack"), std::vector<Colour>{Colour::Red});
    forward.bindings.emplace(Str("Zoom"), std::vector<Colour>{Colour::Blue, Colour::Green});

    Containers backward;
    backward.bindings.emplace(Str("Zoom"), std::vector<Colour>{Colour::Blue, Colour::Green});
    backward.bindings.emplace(Str("Attack"), std::vector<Colour>{Colour::Red});

    CHECK(Encode(meta, forward) == Encode(meta, backward));
}

TEST_CASE("ContainerCodec: a container describes itself for a reader holding only a FieldMeta")
{
    const ComponentMeta meta = MakeContainersMeta();
    Containers value;
    value.numbers = {1, -2, 3};
    value.colours = {Colour::Green, Colour::Red};
    value.counts.emplace(Str("zulu"), 3);
    value.counts.emplace(Str("alpha"), 1);
    value.bindings[Str("Jump")] = {Colour::Blue};

    const auto describe = [&](std::size_t fieldIndex, const void *address)
                          {
                              return Assisi::Core::Reflect::DescribeContainer(meta.fields[fieldIndex],
                                                                              address);
                          };

    CHECK(describe(0, &value.numbers) == "[1, -2, 3]");

    // By enumerator name, because the field carries the leaf's table.
    CHECK(describe(1, &value.colours) == "[Green, Red]");

    // Sorted, like everything else that reads a map through the ops.
    CHECK(describe(4, &value.counts) == "{ alpha: 1, zulu: 3 }");

    // The nested level reads as a nested list rather than as a placeholder.
    CHECK(describe(5, &value.bindings) == "{ Jump: [Blue] }");

    // An empty container is still a container, not "[unsupported type]".
    const Containers empty;
    CHECK(describe(0, &empty.numbers) == "[]");
    CHECK(describe(4, &empty.counts) == "{  }");
}

TEST_CASE("ContainerCodec: a long container is summarised rather than printed whole")
{
    const ComponentMeta meta = MakeContainersMeta();
    Containers value;
    for (std::int32_t i = 0; i < 25; ++i)
    {
        value.numbers.push_back(i);
    }

    const std::string text = Assisi::Core::Reflect::DescribeContainer(meta.fields[0], &value.numbers);

    // An inspector row is one line; twenty-five values on it are less readable
    // than ten and a count.
    CHECK(text.find("0, 1, 2") != std::string::npos);
    CHECK(text.find("15 more") != std::string::npos);
    CHECK(text.find("24") == std::string::npos);
}

TEST_CASE("ContainerCodec: the layout text distinguishes container shapes")
{
    const std::array<ComponentMeta, 1> table{MakeContainersMeta()};
    const std::string text = ProtocolLayoutDescription(table);

    // Each shape spelled out, so two builds that disagree can diff the text and
    // see which field moved rather than only that the hash did.
    CHECK(text.find("numbers vector<i32>") != std::string::npos);
    CHECK(text.find("colours vector<enum>") != std::string::npos);
    CHECK(text.find("labels vector<str>") != std::string::npos);
    CHECK(text.find("weights map<i32,f32>") != std::string::npos);
    CHECK(text.find("counts map<str,i32>") != std::string::npos);
    CHECK(text.find("bindings map<str,vector<enum>>") != std::string::npos);

    // A bare "vector" would hash identically for every element type.
    CHECK(text.find("numbers vector ") == std::string::npos);

    // The leaf enum's values ride along wherever the enum sits, because
    // renumbering one keeps the layout identical while changing what the bits
    // mean.
    CHECK(text.find("Green=7") != std::string::npos);
}

TEST_CASE("ContainerCodec: an element type change moves the protocol layout")
{
    std::array<ComponentMeta, 1> ints{MakeContainersMeta()};
    std::array<ComponentMeta, 1> floats{MakeContainersMeta()};

    // Same field name, same container kind, different element. Nothing but the
    // element type distinguishes these, which is the case a bare "vector" in the
    // layout text would let through.
    floats[0].fields[0].container = ContainerSpecFor<std::vector<float>>();

    CHECK(ProtocolLayoutDescription(ints) != ProtocolLayoutDescription(floats));
}

/// A block naming only the first field (`numbers`), whose payload the caller
/// supplies — so a test can hand the decoder a count it never wrote.
std::vector<std::byte> BlockWithFirstFieldCount(const ComponentMeta &meta, std::uint64_t count,
                                                std::size_t trailingBytes)
{
    BitWriter writer;
    writer.WriteVarUInt32(meta.id.value);
    writer.WriteBits64(1, static_cast<std::uint32_t>(meta.fields.size())); // only field 0 present
    writer.WriteVarUInt64(count);
    for (std::size_t i = 0; i < trailingBytes; ++i)
    {
        writer.WriteUInt8(1);
    }
    const std::span<const std::byte> bytes = writer.Data();
    return {bytes.begin(), bytes.end()};
}

TEST_CASE("ContainerCodec: a hostile element count is refused before allocating")
{
    const ComponentMeta meta = MakeContainersMeta();

    // A count past the hard cap, in a buffer holding nothing. The guard must
    // reject it on the count alone; without it this is a request to allocate
    // four thousand and one entries from an empty stream.
    const std::vector<std::byte> block = BlockWithFirstFieldCount(meta, kMaxVectorElements + 1, 0);

    BitReader reader(block);
    Containers decoded;
    decoded.numbers = {1, 2, 3};

    REQUIRE(ReadComponentId(reader) == meta.id);
    ReadComponent(meta, &decoded, reader);

    CHECK(reader.Failed());

    // Untouched, which is what proves the guard ran *before* the container was
    // cleared and grown. A hostile count must cost nothing at all — not an
    // allocation, and not the data that was already there.
    CHECK(decoded.numbers == std::vector<std::int32_t>{1, 2, 3});
}

TEST_CASE("ContainerCodec: a count larger than the bytes present is refused")
{
    const ComponentMeta meta = MakeContainersMeta();

    // Under the cap, so the cap alone does not catch it — but claiming far more
    // elements than the remaining bits could encode.
    const std::vector<std::byte> block = BlockWithFirstFieldCount(meta, 4000, 1);

    BitReader reader(block);
    Containers decoded;
    decoded.numbers = {42};

    REQUIRE(ReadComponentId(reader) == meta.id);
    ReadComponent(meta, &decoded, reader);

    CHECK(reader.Failed());
    CHECK(decoded.numbers == std::vector<std::int32_t>{42});
}

TEST_CASE("ContainerCodec: a truncated stream leaves the reader failed, not the container wrong")
{
    const ComponentMeta meta = MakeContainersMeta();
    const Containers source  = MakePopulated();

    const std::vector<std::byte> whole = Encode(meta, source);

    // Every prefix of a valid block. None may read out of bounds, and none may
    // come back reporting success.
    for (std::size_t cut = 1; cut < whole.size(); ++cut)
    {
        const std::span<const std::byte> prefix{whole.data(), cut};
        BitReader reader(prefix);
        Containers decoded;

        if (ReadComponentId(reader) != meta.id)
        {
            continue; // the id itself was cut; nothing further to test here
        }
        const bool ok       = ReadComponent(meta, &decoded, reader);
        const bool accepted = ok && reader.Ok();
        CHECK_FALSE(accepted);
    }
}
