/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ComponentMask.hpp
/// @brief A fixed-width set of replicable component types.
///
/// The storage behind per-entity replication policy: which of the components an
/// entity carries it declines to send. Two representations, because the two jobs
/// have incompatible requirements, and the split is the same one `EntityRef`
/// already makes (a raw handle in memory, a stable serial index on disk):
///
///  - **In memory, a bitset** — one byte per eight replicable types, trivially
///    copyable, no heap. It lives on `NetSync::Replicated`, a core struct that
///    ECS pools store and that scene snapshots, PIE world builds, and undo
///    capture all copy; a heap-owning container there would be the wrong shape.
///    Bit index is the component's *replicable ordinal*
///    (ComponentRegistry::ReplicableOrdinalOf), so a lookup is one dense-array
///    hop and a test is one AND.
///
///  - **On disk and on the wire, an array of component names.** Neither ids nor
///    ordinals are build-stable — ids are assigned alphabetically and densely, so
///    adding *any* component shifts them, and ordinals additionally shift when
///    any type's capability flips. Persisting raw bits would silently re-aim an
///    exclusion at the wrong component after an unrelated edit, which is the
///    worst kind of rot: correct-looking data, wrong meaning. Names rot exactly
///    as fast as the level format already does, since component blocks are
///    name-keyed.
///
/// A consequence of that split worth stating: an unresolvable name cannot
/// survive a load→save round-trip, because there is no bit to represent it. It
/// warns and is dropped at load. Acceptable because the editor cannot *produce*
/// one — checkboxes only ever toggle registered components — so an unknown name
/// means a hand-edited file or a renamed type, and the load warning is the
/// earliest moment anyone could act on either.
///
/// The width comes from ReplicableLimits.hpp, which reflectgen counts from the
/// tree at build time. There is no ceiling to configure and none to hit.

#include <Assisi/Core/Reflect/ReplicableLimits.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Assisi::Core::Reflect
{

/// @brief One bit per replicable component type, indexed by replicable ordinal.
struct ComponentMask
{
    std::array<std::uint8_t, kReplicableMaskBytes> bits{};

    /// @brief Whether @p ordinal is in the set. Out-of-range reads false rather
    /// than trapping: an ordinal is only invalid when a component is not
    /// replicable, and "a non-replicable component is not excluded" is the
    /// truthful answer — such a component is not being sent for a different
    /// reason entirely.
    [[nodiscard]] constexpr bool Test(std::size_t ordinal) const
    {
        return ordinal < kReplicableComponentCount && (bits[ordinal / 8u] & (1u << (ordinal % 8u))) != 0u;
    }

    /// @brief Add or remove @p ordinal. Out-of-range is ignored, for the same
    /// reason Test() reads false: there is no bit that could mean it.
    constexpr void Set(std::size_t ordinal, bool value = true)
    {
        if (ordinal >= kReplicableComponentCount)
            return;
        const auto bit = static_cast<std::uint8_t>(1u << (ordinal % 8u));
        if (value)
            bits[ordinal / 8u] |= bit;
        else
            bits[ordinal / 8u] &= static_cast<std::uint8_t>(~bit);
    }

    /// @brief True when nothing is excluded — the default, and the common case.
    [[nodiscard]] constexpr bool Empty() const
    {
        for (const std::uint8_t byte : bits)
        {
            if (byte != 0u)
                return false;
        }
        return true;
    }

    bool operator==(const ComponentMask &) const = default;
};

static_assert(std::is_trivially_copyable_v<ComponentMask>,
              "ComponentMask lives inside a pooled ECS component that snapshots, PIE world builds, and "
              "undo capture all copy; keeping it trivially copyable is the point of the bitset form.");

} // namespace Assisi::Core::Reflect
