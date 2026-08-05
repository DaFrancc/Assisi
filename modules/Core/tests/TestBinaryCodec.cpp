/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBinaryCodec.cpp
/// @brief Reflection-driven component codec: a hand-built ComponentMeta covering
/// every FieldType round-trips, partial field masks patch only what they name,
/// the protocol hash is stable and sensitive, and the decoder survives truncated
/// and corrupted blocks.
///
/// The ComponentMeta here is built by hand rather than taken from the reflection
/// codegen, deliberately: this suite must fail when the *codec* breaks, not when
/// an unrelated engine component changes shape, and Core's test binary has no
/// reflected components of its own to lean on. It also lets the fixture use plain
/// float arrays where the engine uses glm — which is exactly what the codec sees,
/// since Core does not link glm and encodes those fields by raw layout.

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/ContentHash.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/ShortString.hpp>
#include <Assisi/Testing/ThrowOnContractViolation.hpp>

using Assisi::Core::AssetId;
using Assisi::Core::AssetPath;
using Assisi::Core::BitReader;
using Assisi::Core::BitWriter;
using Assisi::Core::ShortString;
using Assisi::Core::Reflect::ComponentMeta;
using Assisi::Core::Reflect::FieldMask;
using Assisi::Core::Reflect::FieldMeta;
using Assisi::Core::Reflect::FieldType;
using Assisi::Core::Reflect::kAllFields;
using Assisi::Core::Reflect::kInvalidComponentId;
using Assisi::Core::Reflect::ProtocolHash;
using Assisi::Core::Reflect::ProtocolLayoutDescription;
using Assisi::Core::Reflect::ProtocolSummary;
using Assisi::Core::Reflect::ReadComponent;
using Assisi::Core::Reflect::ReadComponentId;
using Assisi::Core::Reflect::WriteComponent;

namespace
{

/// The ECS's entity handle, redeclared: Core cannot include the ECS header, and
/// the codec's whole contract for EntityRef is that the field is two uint32s
/// (index, then generation). Redeclaring it here is the test of that contract.
struct EntityHandle
{
    std::uint32_t index      = 0;
    std::uint32_t generation = 0;

    bool operator==(const EntityHandle &) const = default;
};

/// An enum stored at a narrow width, so the codec's enumSize handling is exercised
/// rather than assumed to be 4 bytes.
enum class Mode : std::uint16_t
{
    Off  = 0,
    Slow = 7,
    Fast = 4242,
};

/// One field of every FieldType, in declaration order, plus a transient one the
/// codec must skip entirely. glm-typed fields are plain float arrays: identical
/// layout, no glm dependency in Core.
struct AllTypes
{
    float         floatValue  = 0.f;
    double        doubleValue = 0.0;
    std::int32_t  int32Value  = 0;
    std::uint32_t uint32Value = 0;
    std::int64_t  int64Value  = 0;
    std::uint64_t uint64Value = 0;
    bool          boolValue   = false;

    std::array<float, 2>  vec2{};
    std::array<float, 3>  vec3{};
    std::array<float, 4>  vec4{};
    std::array<float, 4>  quat{};
    std::array<float, 16> mat4{};

    Mode         mode = Mode::Off;
    ShortString  name;
    EntityHandle target;
    AssetPath    path;

    std::vector<AssetPath> paths;
    AssetId                assetId;
    std::vector<AssetId>   assetIds;

    float notReplicated = 0.f; ///< transient: never on the wire.

    bool operator==(const AllTypes &) const = default;
};

/// Byte offset of a member, without offsetof: AllTypes holds std::vectors, so it
/// is not a standard-layout type and offsetof on it is only conditionally
/// supported (and warns). Subtracting addresses within a live object is exactly
/// what reflectgen's offsetof resolves to anyway.
template <typename T, typename M> std::size_t OffsetOf(M T::*member)
{
    static const T prototype{};
    return static_cast<std::size_t>(reinterpret_cast<const std::byte *>(&(prototype.*member)) -
                                    reinterpret_cast<const std::byte *>(&prototype));
}

FieldMeta Field(const char *name, FieldType type, std::size_t offset, bool transient = false, bool norep = false)
{
    FieldMeta field;
    field.name      = name;
    field.type      = type;
    field.offset    = offset;
    field.transient = transient;
    field.norep     = norep;
    return field;
}

/// A replicable component with one field held back from the wire — the shape
/// AFIELD(norep) exists for: server-side bookkeeping living inside a component
/// that otherwise replicates, without splitting the component in two.
struct Gated
{
    std::int32_t shared = 0;
    std::int32_t secret = 0;

    bool operator==(const Gated &) const = default;
};

ComponentMeta MakeGatedMeta()
{
    ComponentMeta meta{.name            = "Gated",
                       .typeIndex       = std::type_index(typeid(Gated)),
                       .fields          = {},
                       .serialize       = {},
                       .addToScene      = {},
                       .iterateEntities = {},
                       .getByEntity     = {},
                       .serializable    = true,
                       .tracksChanges   = true,
                       .replicable      = true,
                       .id              = 5};

    meta.fields.push_back(Field("shared", FieldType::Int32, OffsetOf(&Gated::shared)));
    meta.fields.push_back(Field("secret", FieldType::Int32, OffsetOf(&Gated::secret), false, true));
    return meta;
}

/// The descriptor reflectgen would emit for AllTypes. Built once per call so a
/// test that mutates it (the protocol-hash sensitivity cases) cannot leak into
/// the next.
ComponentMeta MakeAllTypesMeta()
{
    ComponentMeta meta{.name            = "AllTypes",
                       .typeIndex       = std::type_index(typeid(AllTypes)),
                       .fields          = {},
                       .serialize       = {},
                       .addToScene      = {},
                       .iterateEntities = {},
                       .getByEntity     = {},
                       .serializable    = true,
                       .tracksChanges   = true,
                       .replicable      = true,
                       .id              = 3};

    meta.fields.push_back(Field("floatValue", FieldType::Float, OffsetOf(&AllTypes::floatValue)));
    meta.fields.push_back(Field("doubleValue", FieldType::Double, OffsetOf(&AllTypes::doubleValue)));
    meta.fields.push_back(Field("int32Value", FieldType::Int32, OffsetOf(&AllTypes::int32Value)));
    meta.fields.push_back(Field("uint32Value", FieldType::UInt32, OffsetOf(&AllTypes::uint32Value)));
    meta.fields.push_back(Field("int64Value", FieldType::Int64, OffsetOf(&AllTypes::int64Value)));
    meta.fields.push_back(Field("uint64Value", FieldType::UInt64, OffsetOf(&AllTypes::uint64Value)));
    meta.fields.push_back(Field("boolValue", FieldType::Bool, OffsetOf(&AllTypes::boolValue)));
    meta.fields.push_back(Field("vec2", FieldType::Vec2, OffsetOf(&AllTypes::vec2)));
    meta.fields.push_back(Field("vec3", FieldType::Vec3, OffsetOf(&AllTypes::vec3)));
    meta.fields.push_back(Field("vec4", FieldType::Vec4, OffsetOf(&AllTypes::vec4)));
    meta.fields.push_back(Field("quat", FieldType::Quat, OffsetOf(&AllTypes::quat)));
    meta.fields.push_back(Field("mat4", FieldType::Mat4, OffsetOf(&AllTypes::mat4)));

    FieldMeta mode      = Field("mode", FieldType::Enum, OffsetOf(&AllTypes::mode));
    mode.enumSize       = sizeof(Mode);
    mode.enumSigned     = false;
    mode.enumConstants  = {{"Off", 0}, {"Slow", 7}, {"Fast", 4242}};
    meta.fields.push_back(mode);

    meta.fields.push_back(Field("name", FieldType::String, OffsetOf(&AllTypes::name)));
    meta.fields.push_back(Field("target", FieldType::EntityRef, OffsetOf(&AllTypes::target)));
    meta.fields.push_back(Field("path", FieldType::AssetPath, OffsetOf(&AllTypes::path)));
    meta.fields.push_back(Field("paths", FieldType::AssetPathVector, OffsetOf(&AllTypes::paths)));
    meta.fields.push_back(Field("assetId", FieldType::AssetId, OffsetOf(&AllTypes::assetId)));
    meta.fields.push_back(Field("assetIds", FieldType::AssetIdVector, OffsetOf(&AllTypes::assetIds)));
    meta.fields.push_back(Field("notReplicated", FieldType::Float, OffsetOf(&AllTypes::notReplicated), true));

    return meta;
}

/// Distinctive values in every field, so a codec that mixes two fields up or
/// silently zeroes one is caught by the equality check rather than passing on
/// defaults.
AllTypes MakePopulated()
{
    AllTypes value;
    value.floatValue  = -3.5f;
    value.doubleValue = 1.0 / 3.0;
    value.int32Value  = -2147483648;
    value.uint32Value = 4294967295u;
    value.int64Value  = -9007199254740993LL;
    value.uint64Value = 0xFEDCBA9876543210ULL;
    value.boolValue   = true;
    value.vec2        = {1.f, 2.f};
    value.vec3        = {3.f, 4.f, 5.f};
    value.vec4        = {6.f, 7.f, 8.f, 9.f};
    value.quat        = {0.f, 0.f, 0.7071068f, 0.7071068f};
    for (std::size_t i = 0; i < value.mat4.size(); ++i)
        value.mat4[i] = static_cast<float>(i) * 1.25f;
    value.mode   = Mode::Fast;
    value.name   = ShortString("player one");
    value.target = EntityHandle{42u, 7u};
    value.path   = AssetPath("meshes/crate.gltf");
    value.paths  = {AssetPath("a/one.png"), AssetPath("b/two.png"), AssetPath("")};
    value.assetId.bytes = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
                           0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10};
    AssetId second;
    second.bytes[15]  = 0x2A;
    value.assetIds    = {value.assetId, second};
    value.notReplicated = 99.f;
    return value;
}

/// Deterministic xorshift64* — see TestBitStream.cpp. Reproducibility is the
/// whole point of a committed fuzz harness.
class Rng
{
  public:
    explicit Rng(std::uint64_t seed) : _state(seed ? seed : 0x9E3779B97F4A7C15ULL) {}

    std::uint64_t Next()
    {
        _state ^= _state >> 12;
        _state ^= _state << 25;
        _state ^= _state >> 27;
        return _state * 0x2545F4914F6CDD1DULL;
    }

    std::uint32_t Below(std::uint32_t bound) { return static_cast<std::uint32_t>(Next() % bound); }

  private:
    std::uint64_t _state;
};

/// Encodes `source` and decodes it back over `destination`, asserting the block
/// framing (id prefix, then payload) along the way.
bool RoundTrip(const ComponentMeta &meta, const AllTypes &source, AllTypes &destination, FieldMask mask,
               FieldMask *appliedMask = nullptr)
{
    BitWriter writer;
    if (!WriteComponent(meta, &source, writer, mask))
        return false;

    BitReader reader(writer.Data());
    if (ReadComponentId(reader) != meta.id)
        return false;
    return ReadComponent(meta, &destination, reader, appliedMask) && reader.Ok();
}

} // namespace

TEST_CASE("BinaryCodec: a component with every field type round-trips at full state")
{
    const ComponentMeta meta     = MakeAllTypesMeta();
    const AllTypes      source   = MakePopulated();
    AllTypes            decoded;

    REQUIRE(RoundTrip(meta, source, decoded, kAllFields));

    CHECK(decoded.floatValue == doctest::Approx(source.floatValue));
    CHECK(decoded.doubleValue == doctest::Approx(source.doubleValue));
    CHECK(decoded.int32Value == source.int32Value);
    CHECK(decoded.uint32Value == source.uint32Value);
    CHECK(decoded.int64Value == source.int64Value);
    CHECK(decoded.uint64Value == source.uint64Value);
    CHECK(decoded.boolValue == source.boolValue);
    CHECK(decoded.vec2 == source.vec2);
    CHECK(decoded.vec3 == source.vec3);
    CHECK(decoded.vec4 == source.vec4);
    CHECK(decoded.quat == source.quat);
    CHECK(decoded.mat4 == source.mat4);
    CHECK(decoded.mode == source.mode);
    CHECK(decoded.name.View() == source.name.View());
    CHECK(decoded.target == source.target);
    CHECK(decoded.path.View() == source.path.View());
    REQUIRE(decoded.paths.size() == source.paths.size());
    for (std::size_t i = 0; i < source.paths.size(); ++i)
        CHECK(decoded.paths[i].View() == source.paths[i].View());
    CHECK(decoded.assetId == source.assetId);
    CHECK(decoded.assetIds == source.assetIds);

    // The transient field is never on the wire, so the destination keeps its own.
    CHECK(decoded.notReplicated == doctest::Approx(0.f));
}

TEST_CASE("BinaryCodec: the block starts with the component id and a mask of the non-transient fields")
{
    const ComponentMeta meta   = MakeAllTypesMeta();
    const AllTypes      source = MakePopulated();

    BitWriter writer;
    REQUIRE(WriteComponent(meta, &source, writer, kAllFields));

    const std::size_t codecFields = Assisi::Core::Reflect::CountCodecFields(meta);
    CHECK(codecFields == meta.fields.size() - 1); // the transient one does not count

    BitReader reader(writer.Data());
    CHECK(ReadComponentId(reader) == meta.id);
    CHECK(reader.ReadBits64(static_cast<std::uint32_t>(codecFields)) ==
          ((FieldMask{1} << codecFields) - 1u)); // kAllFields clipped to the real width
}

TEST_CASE("BinaryCodec: an empty mask writes only the header and patches nothing")
{
    const ComponentMeta meta   = MakeAllTypesMeta();
    const AllTypes      source = MakePopulated();

    AllTypes  decoded;
    FieldMask applied = kAllFields;
    REQUIRE(RoundTrip(meta, source, decoded, 0, &applied));

    CHECK(applied == 0);
    CHECK(decoded == AllTypes{}); // untouched
}

TEST_CASE("BinaryCodec: a partial mask patches only the named fields")
{
    const ComponentMeta meta   = MakeAllTypesMeta();
    const AllTypes      source = MakePopulated();

    // Bits are indexed over the *non-transient* fields in declaration order:
    // 0 floatValue, 2 int32Value, 13 name, 16 paths.
    const FieldMask mask = Assisi::Core::Reflect::FieldMaskBit(0) | Assisi::Core::Reflect::FieldMaskBit(2) |
                           Assisi::Core::Reflect::FieldMaskBit(13) | Assisi::Core::Reflect::FieldMaskBit(16);

    AllTypes  decoded;
    FieldMask applied = 0;
    REQUIRE(RoundTrip(meta, source, decoded, mask, &applied));
    CHECK(applied == mask);

    // Named fields arrived…
    CHECK(decoded.floatValue == doctest::Approx(source.floatValue));
    CHECK(decoded.int32Value == source.int32Value);
    CHECK(decoded.name.View() == source.name.View());
    REQUIRE(decoded.paths.size() == source.paths.size());
    CHECK(decoded.paths[0].View() == source.paths[0].View());

    // …and everything else kept the destination's prior value, which is what
    // makes a delta a delta.
    const AllTypes untouched{};
    CHECK(decoded.doubleValue == doctest::Approx(untouched.doubleValue));
    CHECK(decoded.uint32Value == untouched.uint32Value);
    CHECK(decoded.vec3 == untouched.vec3);
    CHECK(decoded.mode == untouched.mode);
    CHECK(decoded.target == untouched.target);
    CHECK(decoded.assetIds.empty());
}

TEST_CASE("BinaryCodec: a partial mask over a populated destination overwrites only its fields")
{
    const ComponentMeta meta   = MakeAllTypesMeta();
    const AllTypes      source = MakePopulated();

    AllTypes destination     = MakePopulated();
    destination.floatValue   = 111.f;
    destination.uint32Value  = 222u;
    destination.name         = ShortString("stale");

    // Send only floatValue (bit 0): the other two must keep the destination's
    // values, not be reset to the source's or to zero.
    REQUIRE(RoundTrip(meta, source, destination, Assisi::Core::Reflect::FieldMaskBit(0)));

    CHECK(destination.floatValue == doctest::Approx(source.floatValue));
    CHECK(destination.uint32Value == 222u);
    CHECK(destination.name.View() == "stale");
}

TEST_CASE("BinaryCodec: bits above the field count are ignored, not written")
{
    const ComponentMeta meta   = MakeAllTypesMeta();
    const AllTypes      source = MakePopulated();
    const std::size_t   fields = Assisi::Core::Reflect::CountCodecFields(meta);

    BitWriter all;
    REQUIRE(WriteComponent(meta, &source, all, kAllFields));

    BitWriter exact;
    REQUIRE(WriteComponent(meta, &source, exact, (FieldMask{1} << fields) - 1u));

    // kAllFields and the exactly-clipped mask must produce byte-identical blocks,
    // or a receiver would decode two different streams for the same state.
    REQUIRE(all.BitsWritten() == exact.BitsWritten());
    CHECK(std::vector<std::byte>(all.Data().begin(), all.Data().end()) ==
          std::vector<std::byte>(exact.Data().begin(), exact.Data().end()));
}

TEST_CASE("BinaryCodec: EntityRef routes through the remap hooks")
{
    const ComponentMeta meta   = MakeAllTypesMeta();
    AllTypes            source = MakePopulated();
    source.target              = EntityHandle{5u, 9u};

    // Stand-in for Stage 5's NetId map: the codec must not care what the hook
    // does, only that the value it wrote is the value the hook returned.
    Assisi::Core::Reflect::CodecContext context;
    context.entityToWire   = [](std::uint64_t handle) { return handle + 1000u; };
    context.entityFromWire = [](std::uint64_t wire) { return wire - 1000u; };

    BitWriter writer;
    REQUIRE(WriteComponent(meta, &source, writer, kAllFields, &context));

    SUBCASE("the inverse hook restores the original handle")
    {
        BitReader reader(writer.Data());
        REQUIRE(ReadComponentId(reader) == meta.id);
        AllTypes decoded;
        REQUIRE(ReadComponent(meta, &decoded, reader, nullptr, &context));
        CHECK(decoded.target == source.target);
    }

    SUBCASE("decoding without the hook exposes the remapped wire value")
    {
        BitReader reader(writer.Data());
        REQUIRE(ReadComponentId(reader) == meta.id);
        AllTypes decoded;
        REQUIRE(ReadComponent(meta, &decoded, reader, nullptr, nullptr));
        // index low, generation high: 5 + 1000 = 1005, generation untouched.
        CHECK(decoded.target.index == 1005u);
        CHECK(decoded.target.generation == 9u);
    }
}

TEST_CASE("BinaryCodec: a null context writes raw handles")
{
    const ComponentMeta meta   = MakeAllTypesMeta();
    const AllTypes      source = MakePopulated();
    AllTypes            decoded;
    REQUIRE(RoundTrip(meta, source, decoded, kAllFields));
    CHECK(decoded.target == source.target);
}

TEST_CASE("BinaryCodec: an unfinalized component id is refused")
{
    ComponentMeta  meta   = MakeAllTypesMeta();
    meta.id               = kInvalidComponentId;
    const AllTypes source = MakePopulated();

    BitWriter writer;
#ifndef NDEBUG
    // Refusing is a contract violation in debug (it means the caller queried the
    // registry too early), so the throwing handler is what makes it observable.
    Assisi::Testing::ThrowOnContractViolation guard;
    CHECK_THROWS_AS((void)WriteComponent(meta, &source, writer, kAllFields), Assisi::Core::ContractViolation);
#else
    CHECK_FALSE(WriteComponent(meta, &source, writer, kAllFields));
#endif
}

TEST_CASE("BinaryCodec: a FieldType::Unknown field is a loud encode failure")
{
    ComponentMeta meta = MakeAllTypesMeta();
    meta.fields.push_back(Field("mystery", FieldType::Unknown, OffsetOf(&AllTypes::notReplicated)));
    const AllTypes source = MakePopulated();

    BitWriter writer;
#ifndef NDEBUG
    Assisi::Testing::ThrowOnContractViolation guard;
    CHECK_THROWS_AS((void)WriteComponent(meta, &source, writer, kAllFields), Assisi::Core::ContractViolation);
#else
    // Release: no assert, but still a hard `false` and a logged error — never a
    // silently skipped field, which would desync every field after it.
    CHECK_FALSE(WriteComponent(meta, &source, writer, kAllFields));
#endif
}

TEST_CASE("BinaryCodec: strings and vectors at their edges round-trip")
{
    const ComponentMeta meta = MakeAllTypesMeta();

    AllTypes source;
    source.name = ShortString(std::string(Assisi::Core::kShortStringMax, 'n')); // exactly full
    source.path = AssetPath(std::string(Assisi::Core::kAssetPathMax, 'p'));
    source.paths.clear();  // empty vector
    source.assetIds.clear();

    AllTypes decoded = MakePopulated(); // non-empty, so the empty vectors must clear it
    REQUIRE(RoundTrip(meta, source, decoded, kAllFields));

    CHECK(decoded.name.View() == source.name.View());
    CHECK(decoded.name.Size() == Assisi::Core::kShortStringMax);
    CHECK(decoded.path.View() == source.path.View());
    CHECK(decoded.paths.empty());
    CHECK(decoded.assetIds.empty());
}

// ── Protocol hash ─────────────────────────────────────────────────────────────

TEST_CASE("BinaryCodec: the protocol hash is stable across calls")
{
    const std::array<ComponentMeta, 1> table{MakeAllTypesMeta()};

    const std::uint64_t first  = ProtocolHash(table);
    const std::uint64_t second = ProtocolHash(table);
    CHECK(first == second);
    CHECK(first != 0);

    // And identical for an independently built but identical table — this is the
    // property two machines depend on at handshake.
    const std::array<ComponentMeta, 1> rebuilt{MakeAllTypesMeta()};
    CHECK(ProtocolHash(rebuilt) == first);
}

TEST_CASE("BinaryCodec: the protocol hash changes when the wire layout changes")
{
    const std::array<ComponentMeta, 1> baseline{MakeAllTypesMeta()};
    const std::uint64_t                base = ProtocolHash(baseline);

    auto hashWith = [](auto &&mutate)
    {
        std::array<ComponentMeta, 1> table{MakeAllTypesMeta()};
        mutate(table[0]);
        return ProtocolHash(table);
    };

    SUBCASE("a renamed field") { CHECK(hashWith([](ComponentMeta &m) { m.fields[0].name = "renamed"; }) != base); }
    SUBCASE("a retyped field")
    {
        CHECK(hashWith([](ComponentMeta &m) { m.fields[0].type = FieldType::UInt32; }) != base);
    }
    SUBCASE("a reordered field")
    {
        CHECK(hashWith([](ComponentMeta &m) { std::swap(m.fields[0], m.fields[1]); }) != base);
    }
    SUBCASE("a removed field")
    {
        // end() - 2: the last entry is the transient one, which is not part of
        // the protocol at all (see the companion test below).
        CHECK(hashWith([](ComponentMeta &m) { m.fields.erase(m.fields.end() - 2); }) != base);
    }
    SUBCASE("a renamed component") { CHECK(hashWith([](ComponentMeta &m) { m.name = "Other"; }) != base); }
    SUBCASE("a reassigned id") { CHECK(hashWith([](ComponentMeta &m) { m.id = 4; }) != base); }
    SUBCASE("a field turning transient — the mask width changes")
    {
        CHECK(hashWith([](ComponentMeta &m) { m.fields[1].transient = true; }) != base);
    }
    SUBCASE("a field turning norep — the mask width changes the same way")
    {
        CHECK(hashWith([](ComponentMeta &m) { m.fields[1].norep = true; }) != base);
    }
    SUBCASE("a component that stops replicating")
    {
        // Nothing about the field descriptions moves, so this is the one wire
        // difference the layout text has to state outright: the two builds would
        // simply exchange different component sets.
        CHECK(hashWith([](ComponentMeta &m) { m.replicable = false; }) != base);
    }
    SUBCASE("a changed quantization bound — the silent-corruption case")
    {
        CHECK(hashWith(
                  [](ComponentMeta &m)
                  {
                      m.fields[0].hasMax   = true;
                      m.fields[0].maxValue = 10.f;
                  }) != base);
    }
    SUBCASE("a renumbered enumerator") { CHECK(hashWith([](ComponentMeta &m) { m.fields[12].enumConstants[1].value = 8; }) != base); }
    SUBCASE("a narrower enum") { CHECK(hashWith([](ComponentMeta &m) { m.fields[12].enumSize = 4; }) != base); }
}

TEST_CASE("BinaryCodec: the protocol hash ignores things the wire does not carry")
{
    const std::array<ComponentMeta, 1> baseline{MakeAllTypesMeta()};
    const std::uint64_t                base = ProtocolHash(baseline);

    // Field offsets are local memory layout: two builds whose padding differs are
    // still wire-compatible, and rejecting them would be a false positive.
    std::array<ComponentMeta, 1> repadded{MakeAllTypesMeta()};
    repadded[0].fields[0].offset += 64;
    CHECK(ProtocolHash(repadded) == base);

    // A transient field is not on the wire, so adding or dropping one is not a
    // protocol change either — only flipping an existing field's transient flag
    // is, because that moves every later mask bit.
    std::array<ComponentMeta, 1> withoutTransient{MakeAllTypesMeta()};
    withoutTransient[0].fields.pop_back();
    CHECK(ProtocolHash(withoutTransient) == base);

    // Same for a norep field: it is saved to disk, never sent, so gaining one
    // does not change what two builds must agree on.
    std::array<ComponentMeta, 1> withNorep{MakeAllTypesMeta()};
    withNorep[0].fields.push_back(Field("serverOnly", FieldType::Int32, 0, false, true));
    CHECK(ProtocolHash(withNorep) == base);
}

TEST_CASE("BinaryCodec: a norep field occupies no mask bit and never leaves the sender")
{
    const ComponentMeta meta = MakeGatedMeta();

    // One wire field of two: the mask narrows, which is why norep is inside the
    // protocol hash.
    CHECK(Assisi::Core::Reflect::CountCodecFields(meta) == 1);

    const Gated source{7, 1234};
    BitWriter   writer;
    REQUIRE(WriteComponent(meta, &source, writer, kAllFields));

    Gated     destination{0, -1};
    BitReader reader(writer.Data());
    REQUIRE(ReadComponentId(reader) == meta.id);
    REQUIRE(ReadComponent(meta, &destination, reader));

    CHECK(destination.shared == 7);
    // Untouched — the receiver keeps whatever it had, which for a real client is
    // the field's default. Nothing about the sender's value is recoverable from
    // the bytes.
    CHECK(destination.secret == -1);
}

TEST_CASE("BinaryCodec: the protocol summary is human-readable and carries the hash")
{
    const std::array<ComponentMeta, 1> table{MakeAllTypesMeta()};

    const std::string summary = ProtocolSummary(table);
    CHECK(summary.find("assisi-proto/1") != std::string::npos);
    CHECK(summary.find("components=1") != std::string::npos);
    CHECK(summary.find(Assisi::Core::ToHex64(ProtocolHash(table))) != std::string::npos);

    // The full description names fields, so a mismatch is diagnosable by diffing
    // two builds' descriptions rather than staring at two 64-bit numbers.
    const std::string description = ProtocolLayoutDescription(table);
    CHECK(description.find("AllTypes") != std::string::npos);
    CHECK(description.find("uint64Value") != std::string::npos);
    CHECK(description.find("notReplicated") == std::string::npos); // transient: not part of the protocol
    CHECK(description.find("AllTypes replicated") != std::string::npos);

    const std::array<ComponentMeta, 1> gated{MakeGatedMeta()};
    const std::string                  gatedText = ProtocolLayoutDescription(gated);
    CHECK(gatedText.find("shared") != std::string::npos);
    CHECK(gatedText.find("secret") == std::string::npos); // norep: not part of the protocol either
}

// ── Fuzzing the decoder ───────────────────────────────────────────────────────
// Committed test code, not aspiration: this codec is the first thing that touches
// bytes from an unauthenticated peer. Every case below also runs under ASan.

TEST_CASE("BinaryCodec: truncation at every length fails cleanly")
{
    const ComponentMeta meta   = MakeAllTypesMeta();
    const AllTypes      source = MakePopulated();

    BitWriter writer;
    REQUIRE(WriteComponent(meta, &source, writer, kAllFields));
    const std::vector<std::byte> full(writer.Data().begin(), writer.Data().end());

    for (std::size_t length = 0; length < full.size(); ++length)
    {
        BitReader reader(std::span{full.data(), length});
        (void)ReadComponentId(reader);

        AllTypes decoded;
        const bool ok = ReadComponent(meta, &decoded, reader, nullptr, nullptr);

        CAPTURE(length);
        // Every truncation drops bits the block needs, so the decode must report
        // failure — through the return value or the reader's sticky flag — and
        // must never read past the span it was given.
        CHECK((!ok || reader.Failed()));
        CHECK(reader.BitsRead() <= length * 8u);
    }
}

TEST_CASE("BinaryCodec: bit-flipped blocks never crash or read out of bounds")
{
    const ComponentMeta meta   = MakeAllTypesMeta();
    const AllTypes      source = MakePopulated();

    BitWriter writer;
    REQUIRE(WriteComponent(meta, &source, writer, kAllFields));
    const std::vector<std::byte> original(writer.Data().begin(), writer.Data().end());

    Rng rng(0xBADC0DE);
    for (std::int32_t iteration = 0; iteration < 3000; ++iteration)
    {
        std::vector<std::byte> corrupt = original;

        const std::uint32_t flips = 1u + rng.Below(4);
        for (std::uint32_t f = 0; f < flips; ++f)
        {
            const std::size_t bitIndex = rng.Below(static_cast<std::uint32_t>(corrupt.size() * 8u));
            corrupt[bitIndex / 8u] ^= static_cast<std::byte>(static_cast<std::uint8_t>(1u << (bitIndex % 8u)));
        }

        BitReader reader(corrupt);
        (void)ReadComponentId(reader);

        AllTypes decoded;
        (void)ReadComponent(meta, &decoded, reader, nullptr, nullptr);

        // A flipped payload bit decodes to a different-but-valid value; a flipped
        // length or count bit must be rejected. Either way: no crash, no hang, no
        // read outside the buffer, and the reader's state stays coherent.
        CAPTURE(iteration);
        CHECK(reader.BitsRead() <= corrupt.size() * 8u);
        CHECK(decoded.paths.size() <= Assisi::Core::Reflect::kMaxVectorElements);
        CHECK(decoded.assetIds.size() <= Assisi::Core::Reflect::kMaxVectorElements);
    }
}

TEST_CASE("BinaryCodec: random noise decoded as a component block stays in bounds")
{
    const ComponentMeta meta = MakeAllTypesMeta();
    Rng                 rng(0xF00D);

    for (std::int32_t iteration = 0; iteration < 2000; ++iteration)
    {
        const std::size_t      size = rng.Below(96);
        std::vector<std::byte> noise(size);
        for (std::byte &byte : noise)
            byte = static_cast<std::byte>(static_cast<std::uint8_t>(rng.Next()));

        BitReader reader(noise);
        (void)ReadComponentId(reader);

        AllTypes decoded;
        (void)ReadComponent(meta, &decoded, reader, nullptr, nullptr);

        CHECK(reader.BitsRead() <= noise.size() * 8u);
        CHECK(decoded.paths.size() <= Assisi::Core::Reflect::kMaxVectorElements);
        CHECK(decoded.assetIds.size() <= Assisi::Core::Reflect::kMaxVectorElements);
    }
}

TEST_CASE("BinaryCodec: an instanceRef UInt32 translates through the instance hooks")
{
    // A blueprint instance id is a per-world counter — a server's "instance 7"
    // names nothing on a client — so the wire carries the instance's baseNetId and
    // each side translates at the codec boundary. Same shape as EntityRef, applied
    // to a plain integer, which is why it needs a field flag to be told apart from
    // every other UInt32.
    ComponentMeta meta = MakeAllTypesMeta();

    // uint32Value stands in for BlueprintMember::instanceId.
    FieldMeta *tagged = nullptr;
    for (FieldMeta &field : meta.fields)
    {
        if (field.name == "uint32Value")
            tagged = &field;
    }
    REQUIRE(tagged != nullptr);
    tagged->type = FieldType::InstanceRef;

    AllTypes source     = MakePopulated();
    source.uint32Value  = 7u;

    Assisi::Core::Reflect::CodecContext context;
    context.instanceToWire   = [](std::uint32_t local) { return local + 5000u; };
    context.instanceFromWire = [](std::uint32_t wire) { return wire - 5000u; };

    BitWriter writer;
    REQUIRE(WriteComponent(meta, &source, writer, kAllFields, &context));

    SUBCASE("the inverse hook restores the local id")
    {
        BitReader reader(writer.Data());
        REQUIRE(ReadComponentId(reader) == meta.id);
        AllTypes decoded;
        REQUIRE(ReadComponent(meta, &decoded, reader, nullptr, &context));
        CHECK(decoded.uint32Value == 7u);
    }

    SUBCASE("decoding without the hook exposes the wire value")
    {
        BitReader reader(writer.Data());
        REQUIRE(ReadComponentId(reader) == meta.id);
        AllTypes decoded;
        REQUIRE(ReadComponent(meta, &decoded, reader, nullptr, nullptr));
        CHECK(decoded.uint32Value == 5007u);
    }

    SUBCASE("an unflagged UInt32 in the same component is untouched")
    {
        // The flag is per field, not per component: only the one that names an
        // instance is translated, or every counter in the engine would be.
        BitReader reader(writer.Data());
        REQUIRE(ReadComponentId(reader) == meta.id);
        AllTypes decoded;
        REQUIRE(ReadComponent(meta, &decoded, reader, nullptr, &context));
        CHECK(decoded.int32Value == source.int32Value);
        CHECK(decoded.uint64Value == source.uint64Value);
    }
}

TEST_CASE("BinaryCodec: instanceRef with no hook installed is a plain integer")
{
    // The same-process round trip — a save game, the editor, this test suite —
    // has one id space, so translating would be wrong. A null hook writes through.
    ComponentMeta meta = MakeAllTypesMeta();
    for (FieldMeta &field : meta.fields)
    {
        if (field.name == "uint32Value")
            field.type = FieldType::InstanceRef;
    }

    AllTypes source    = MakePopulated();
    source.uint32Value = 41u;
    AllTypes decoded;

    REQUIRE(RoundTrip(meta, source, decoded, kAllFields));
    CHECK(decoded.uint32Value == 41u);
}

TEST_CASE("BinaryCodec: instanceRef is part of the protocol layout")
{
    // Unlike AFIELD(controlled), which changes which messages are accepted and
    // never how bytes decode. A build that translates and one that does not would
    // exchange numbers from two different id spaces and agree about every one of
    // them — silent, and exactly what the handshake exists to prevent.
    ComponentMeta plain = MakeAllTypesMeta();

    ComponentMeta flagged = MakeAllTypesMeta();
    for (FieldMeta &field : flagged.fields)
    {
        if (field.name == "uint32Value")
            field.type = FieldType::InstanceRef;
    }

    const std::array<ComponentMeta, 1> plainSet{plain};
    const std::array<ComponentMeta, 1> flaggedSet{flagged};

    CHECK(ProtocolLayoutDescription(plainSet) != ProtocolLayoutDescription(flaggedSet));
}
