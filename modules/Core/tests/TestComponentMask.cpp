/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestComponentMask.cpp
/// @brief ComponentMask: the bitset itself, and the codec paths that need no
///        registered replicable component to exercise.
///
/// The property under test throughout is the memory/disk split. In memory a mask
/// is bits indexed by replicable ordinal — compact, trivially copyable, and
/// meaningless outside the build that produced it. In every codec it is an array
/// of component *names*, because ordinals reshuffle whenever any component is
/// added, renamed, or has its capability flipped, and a persisted bit would then
/// point at the wrong component while still looking perfectly valid.
///
/// **Scope note.** Core links no ACOMP(replicable) components and cannot: the
/// code reflectgen emits for any component includes ECS::Scene, which sits above
/// Core. So the cases that need a real replicable name to resolve — round-trips,
/// ordering, the not-replicable rejection — live in the NetSync suite
/// (TestComponentMaskCodec.cpp), which links the whole engine. Splitting them
/// that way is deliberate: written here they would find an empty registry, skip
/// their own bodies, and report success without testing anything.

#include <doctest/doctest.h>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ComponentMask.hpp>
#include <Assisi/Core/Reflect/ComponentMaskJson.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace Assisi::Core::Reflect;

namespace
{

/// Collects log output so a test can assert that a *diagnostic* fired, not just
/// that the value came out right. Every dropped-name path here is a case where
/// silence would be the actual bug.
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

std::shared_ptr<CapturingSink> InstallCapturingSink()
{
    auto sink = std::make_shared<CapturingSink>();
    Assisi::Core::GetLogger().AddSink(sink);
    return sink;
}

} // namespace

TEST_CASE("ComponentMask sets, clears, and reports empty")
{
    ComponentMask mask;
    CHECK(mask.Empty());
    CHECK_FALSE(mask.Test(0));

    // The generated width is at least one byte even in a build with nothing
    // replicable, so there is always a bit 0 to address.
    REQUIRE(kReplicableMaskBytes >= 1);

    if (kReplicableComponentCount == 0)
        return;

    mask.Set(0);
    CHECK(mask.Test(0));
    CHECK_FALSE(mask.Empty());

    mask.Set(0, false);
    CHECK_FALSE(mask.Test(0));
    CHECK(mask.Empty());
}

TEST_CASE("every ordinal the width promises is addressable")
{
    // The build-time count and the mask width must agree, or exclusions on the
    // last components would silently evaporate — the tail is exactly where an
    // off-by-one in the byte arithmetic would hide.
    for (std::size_t ordinal = 0; ordinal < kReplicableComponentCount; ++ordinal)
    {
        ComponentMask mask;
        mask.Set(ordinal);
        CAPTURE(ordinal);
        CHECK(mask.Test(ordinal));
        CHECK_FALSE(mask.Empty());
    }
}

TEST_CASE("an out-of-range ordinal is inert rather than corrupting")
{
    // ReplicableOrdinalOf returns kInvalidOrdinal for a component that is not
    // replicable, and callers pass it straight through. Reading false and
    // ignoring the write is the truthful answer — such a component is not being
    // sent for an entirely different reason — and it is what keeps a bad ordinal
    // from scribbling on whatever follows the mask inside its component.
    ComponentMask mask;
    mask.Set(ComponentRegistry::kInvalidOrdinal);
    CHECK(mask.Empty());
    CHECK_FALSE(mask.Test(ComponentRegistry::kInvalidOrdinal));

    mask.Set(kReplicableComponentCount + 99);
    CHECK(mask.Empty());
}

TEST_CASE("the registry's replicable ordinals are dense and fit the generated width")
{
    // The invariant the build-time count exists to protect: ordinals are exactly
    // positions in ReplicableComponents(), so every one of them fits the mask
    // reflectgen sized. A mismatch is what the finalize-time fence shouts about,
    // and asserting the property directly is cheaper and sharper than
    // manufacturing an over-full registry.
    const ComponentRegistry &registry   = ComponentRegistry::Instance();
    const std::span<const ComponentMeta *const> replicable = registry.ReplicableComponents();

    CHECK(replicable.size() <= kReplicableComponentCount);

    for (std::size_t ordinal = 0; ordinal < replicable.size(); ++ordinal)
    {
        CAPTURE(replicable[ordinal]->name);
        CHECK(replicable[ordinal]->replicable);
        CHECK(registry.ReplicableOrdinalOf(replicable[ordinal]->id) == ordinal);
    }

    // ...and a component that is not replicable has no ordinal at all, which is
    // what makes "excluding a local component" unrepresentable rather than merely
    // discouraged.
    for (const ComponentMeta &meta : registry.All())
    {
        if (!meta.replicable)
            CHECK(registry.ReplicableOrdinalOf(meta.id) == ComponentRegistry::kInvalidOrdinal);
    }
}

TEST_CASE("an empty mask serializes to an empty array and back")
{
    const ComponentMask empty;
    const nlohmann::json json = SerializeComponentMask(empty);
    CHECK(json.is_array());
    CHECK(json.empty());
    CHECK(DeserializeComponentMask(json) == empty);

    // An absent field reads as "nothing excluded", which is the default policy:
    // send every capable component present.
    CHECK(DeserializeComponentMask(nlohmann::json{}) == empty);
}

TEST_CASE("an unresolvable exclusion name is dropped, loudly")
{
    // It cannot round-trip: there is no bit that represents it. That trade is
    // acceptable only because the warning fires at the earliest moment anyone
    // could act — the editor cannot produce such a name, so one means a
    // hand-edited file or a renamed type.
    auto sink = InstallCapturingSink();

    const ComponentMask empty;
    CHECK(DeserializeComponentMask(nlohmann::json::array({"NoSuchComponentAnywhere"})) == empty);
    CHECK(sink->Mentions("NoSuchComponentAnywhere"));
}

TEST_CASE("a malformed exclusion list loses one field rather than the whole level")
{
    auto sink = InstallCapturingSink();

    // Not an array at all: warn and read as no exclusions. A hand-mangled level
    // file should degrade, not throw.
    CHECK(DeserializeComponentMask(nlohmann::json{{"nonsense", 1}}).Empty());
    CHECK(sink->Mentions("expected an array"));

    // An array whose elements are not strings: skip them, keep going.
    nlohmann::json mixed = nlohmann::json::array();
    mixed.push_back(42);
    mixed.push_back(nlohmann::json::object());
    CHECK(DeserializeComponentMask(mixed).Empty());
    CHECK(sink->Mentions("must be component names"));
}
