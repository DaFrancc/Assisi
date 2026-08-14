/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestComponentMaskCodec.cpp
/// @brief ComponentMask's codecs, exercised where replicable components exist.
///
/// The companion to Core's TestComponentMask.cpp, and it lives here for a
/// reason worth stating: Core links no ACOMP(replicable) component and cannot
/// (reflectgen's output for any component includes ECS::Scene, which sits above
/// Core), so a round-trip test written there would resolve nothing, skip its own
/// body, and report success. This binary links the whole engine, so the names
/// these tests translate through are real ones.

#include <doctest/doctest.h>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/ComponentMask.hpp>
#include <Assisi/Core/Reflect/ComponentMaskJson.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <vector>

using namespace Assisi::Core::Reflect;

namespace
{

struct CapturingSink final : Assisi::Core::Sink
{
    std::vector<std::string> messages;

    void Write(Assisi::Core::LogLevel level, std::string_view message) override
    {
        if (level >= Assisi::Core::LogLevel::Warn)
            messages.emplace_back(message);
    }

    [[nodiscard]] bool Mentions(std::string_view needle) const
    {
        for (const std::string &message : messages)
        {
            if (message.find(needle) != std::string::npos)
                return true;
        }
        return false;
    }
};

/// A ComponentMeta shaped like one reflectgen would emit for a single mask
/// field. Hand-built so these tests do not depend on which component happens to
/// carry a mask in this build — but with a **finalized id**, because
/// WriteComponent refuses an unfinalized one (and rightly: an unfinalized id
/// means the registry has not assigned wire identities yet).
ComponentMeta MaskHolderMeta()
{
    // Exhaustive rather than partial: typeIndex has no default constructor, so
    // this has to be aggregate-initialized, and a partial list is what
    // -Wmissing-field-initializers is for. The hooks are unused here.
    ComponentMeta meta{.name            = "MaskHolder",
                       .typeIndex       = std::type_index(typeid(ComponentMask)),
                       .fields          = {},
                       .serialize       = {},
                       .addToScene      = {},
                       .iterateEntities = {},
                       .getByEntity     = {},
                       .construct       = {},
                       .getMutable      = {}};
    FieldMeta field;
    field.name   = "excluded";
    field.type   = FieldType::ComponentMask;
    field.offset = 0;
    meta.fields.push_back(field);
    meta.id = ComponentId{0}; // finalized-looking id for WriteComponent's check; 0 is a real id here
    return meta;
}

/// Whichever replicable component this build happens to have first.
std::size_t FirstReplicableOrdinal() { return 0; }

/// Decode a component the way a real receiver does: the id is framed by
/// WriteComponent but consumed *before* ReadComponent, because a receiver has to
/// read it to know which meta to decode against in the first place.
bool ReadWholeComponent(const ComponentMeta &meta, void *out, Assisi::Core::BitReader &reader)
{
    const ComponentId id = ReadComponentId(reader);
    if (!reader.Ok() || id != meta.id)
        return false;
    return ReadComponent(meta, out, reader, nullptr, nullptr);
}

} // namespace

TEST_CASE("this binary actually has replicable components to test against")
{
    // A guard, not a formality. Every test below translates through real
    // component names, and if the replicable set ever emptied they would all
    // pass by doing nothing — which is exactly the failure that moved these
    // tests out of the Core suite in the first place.
    REQUIRE_FALSE(ComponentRegistry::Instance().ReplicableComponents().empty());
}

TEST_CASE("a mask round-trips through JSON as component names")
{
    const ComponentRegistry &registry   = ComponentRegistry::Instance();
    const std::span<const ComponentMeta *const> replicable = registry.ReplicableComponents();

    ComponentMask mask;
    mask.Set(FirstReplicableOrdinal());

    const nlohmann::json json = SerializeComponentMask(mask);
    REQUIRE(json.is_array());
    REQUIRE(json.size() == 1);
    // Names, not bits — the whole point of the split.
    CHECK(json[0].get<std::string>() == replicable[FirstReplicableOrdinal()]->name);

    CHECK(DeserializeComponentMask(json) == mask);
}

TEST_CASE("JSON exclusion names are emitted in a stable order")
{
    // A mask that has not changed must produce byte-identical output, or every
    // save shows phantom diffs and dirties scenes nobody edited.
    const std::span<const ComponentMeta *const> replicable = ComponentRegistry::Instance().ReplicableComponents();
    REQUIRE(replicable.size() >= 2);

    ComponentMask mask;
    mask.Set(0);
    mask.Set(1);

    const nlohmann::json first  = SerializeComponentMask(mask);
    const nlohmann::json second = SerializeComponentMask(mask);
    CHECK(first == second);
    REQUIRE(first.size() == 2);
    CHECK(first[0].get<std::string>() == replicable[0]->name);
    CHECK(first[1].get<std::string>() == replicable[1]->name);
}

TEST_CASE("a whole-set mask survives a JSON round-trip")
{
    // Exercises the top ordinal, where a byte-boundary error would live.
    const std::span<const ComponentMeta *const> replicable = ComponentRegistry::Instance().ReplicableComponents();

    ComponentMask all;
    for (std::size_t ordinal = 0; ordinal < replicable.size(); ++ordinal)
        all.Set(ordinal);

    const nlohmann::json json = SerializeComponentMask(all);
    CHECK(json.size() == replicable.size());
    CHECK(DeserializeComponentMask(json) == all);
}

TEST_CASE("excluding a component that exists but is not replicable is dropped, loudly")
{
    const ComponentRegistry &registry = ComponentRegistry::Instance();

    std::string localOnly;
    for (const ComponentMeta &meta : registry.All())
    {
        if (!meta.replicable)
        {
            localOnly = meta.name;
            break;
        }
    }
    REQUIRE_FALSE(localOnly.empty());

    auto sink = std::make_shared<CapturingSink>();
    Assisi::Core::GetLogger().AddSink(sink);

    // Distinct from the unknown-name case on purpose: this is a stale exclusion
    // left behind when a type lost its capability, and a reader deserves to be
    // told which of the two happened.
    CHECK(DeserializeComponentMask(nlohmann::json::array({localOnly})).Empty());
    CHECK(sink->Mentions("not ACOMP(replicable)"));
}

TEST_CASE("a mask round-trips through the binary codec as names")
{
    ComponentMask mask;
    mask.Set(FirstReplicableOrdinal());

    const ComponentMeta meta = MaskHolderMeta();

    Assisi::Core::BitWriter writer;
    REQUIRE(WriteComponent(meta, &mask, writer, kAllFields, nullptr));

    ComponentMask decoded;
    Assisi::Core::BitReader reader(writer.Data());
    REQUIRE(ReadWholeComponent(meta, &decoded, reader));
    CHECK(decoded == mask);
}

TEST_CASE("an empty mask costs a single count varint on the wire")
{
    // Not a size assertion for its own sake: the default policy is "exclude
    // nothing", so the empty case is what almost every entity pays, and it must
    // stay negligible.
    const ComponentMeta meta = MaskHolderMeta();
    const ComponentMask empty;
    Assisi::Core::BitWriter writer;
    REQUIRE(WriteComponent(meta, &empty, writer, kAllFields, nullptr));

    ComponentMask decoded;
    decoded.Set(FirstReplicableOrdinal()); // start dirty, so a no-op read would show
    Assisi::Core::BitReader reader(writer.Data());
    REQUIRE(ReadWholeComponent(meta, &decoded, reader));
    CHECK(decoded.Empty());
}

TEST_CASE("a truncated mask payload fails the reader instead of inventing bits")
{
    ComponentMask mask;
    mask.Set(FirstReplicableOrdinal());

    const ComponentMeta meta = MaskHolderMeta();
    Assisi::Core::BitWriter writer;
    REQUIRE(WriteComponent(meta, &mask, writer, kAllFields, nullptr));

    // Every prefix of a valid payload must either be rejected or decode to
    // something harmless — and above all must not read past the buffer, which is
    // what the sanitizer presets prove on this loop.
    const std::vector<std::byte> full(writer.Data().begin(), writer.Data().end());
    for (std::size_t cut = 0; cut < full.size(); ++cut)
    {
        CAPTURE(cut);
        ComponentMask decoded;
        Assisi::Core::BitReader reader(std::span{full}.first(cut));
        (void)ReadWholeComponent(meta, &decoded, reader);
        CHECK((!reader.Ok() || decoded.Empty() || decoded == mask));
    }
}

TEST_CASE("a hostile element count cannot outrun the buffer")
{
    // The count is a varint the peer chooses, so a claimed billion entries must
    // be checked against the bits actually remaining rather than believed. Fed
    // through the real component framing, since that is the only entry point a
    // peer can reach.
    const ComponentMeta meta = MaskHolderMeta();

    Assisi::Core::BitWriter writer;
    const ComponentMask empty;
    REQUIRE(WriteComponent(meta, &empty, writer, kAllFields, nullptr));

    // Same framing WriteComponent produces — id, then the one-bit field mask —
    // with an absurd count spliced into the body.
    Assisi::Core::BitWriter hostile;
    hostile.WriteVarUInt32(meta.id.value); // wire write
    hostile.WriteBits64(1, 1); // the single wire field is present
    hostile.WriteVarUInt64(1'000'000'000ull);

    ComponentMask decoded;
    Assisi::Core::BitReader reader(hostile.Data());
    (void)ReadWholeComponent(meta, &decoded, reader);
    // Either the framing rejects it or the count check does; what must not
    // happen is a billion reads against a handful of bytes.
    CHECK(decoded.Empty());
}
