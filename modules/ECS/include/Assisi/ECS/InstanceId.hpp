/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file InstanceId.hpp
/// @brief Which blueprint instance something belongs to — a number that means
/// nothing outside the world that issued it.

#include <Assisi/Core/StrongId.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>

namespace Assisi::ECS
{

/// @brief A live blueprint instance, by the id its world handed out.
///
/// A `std::uint32_t` underneath and deliberately not interchangeable with one.
/// Blueprint replication passes four unsigned integers through the same
/// functions, and they are not the same kind of thing:
///
///   - an **instance id** is a per-world counter from 1 (Runtime::InstanceTable),
///     restarted on every level load, meaningful only on the machine that issued
///     it;
///   - a **member index** is a position in a blueprint's flattened member list,
///     identical on every machine that reads the same file;
///   - a **NetId** is a replication identity, agreed across the session;
///   - a **baseNetId** is the NetId an instance's member block starts at, and
///     `baseNetId + memberIndex` is *usually* — not always — a member's NetId.
///
/// As bare integers, passing one where another was meant compiles and produces a
/// plausible wrong answer. This type makes the narrowest of them — a purely local
/// identity — the one the compiler checks.
///
/// **No implicit conversion, in either direction.** `InstanceId{7}` and `id.value`
/// belong at the boundary where a raw number genuinely arrives (a file, a wire, an
/// ImGui id), never to quiet a compiler that is telling you two id spaces met.
/// An aggregate, matching NetSync::ClientId; aggregate initialization is the only
/// way in, which is what blocks the implicit conversion. No arithmetic, because
/// adding to an instance id is never a meaningful operation.
struct InstanceId
{
    /// 0 is never a live instance — the table hands out from 1 — so a
    /// value-initialized InstanceId is "none" and needs no separate sentinel.
    std::uint32_t value = 0;

    [[nodiscard]] constexpr bool IsValid() const { return value != 0; }

    friend constexpr bool operator==(InstanceId, InstanceId)  = default;
    friend constexpr auto operator<=>(InstanceId, InstanceId) = default;
};

/// @brief No live instance. Spelled out where the intent is "none" rather than
/// "the number zero".
inline constexpr InstanceId NullInstance{};

} // namespace Assisi::ECS

namespace Assisi::Core
{
/// Encodes as a varint. Note what crosses is never this number itself — the
/// codec's instanceToWire hook substitutes the instance's base NetId — but the
/// field it occupies is an id slot all the same.
template <> struct IsStrongId<ECS::InstanceId> : std::true_type
{
};
static_assert(StrongId<ECS::InstanceId>);
} // namespace Assisi::Core

/// Prints as the bare number, so a log line reads "instance 7" rather than making
/// every call site spell `.value`.
template <> struct std::formatter<Assisi::ECS::InstanceId> : std::formatter<std::uint32_t>
{
    auto format(Assisi::ECS::InstanceId id, std::format_context &ctx) const
    {
        return std::formatter<std::uint32_t>::format(id.value, ctx);
    }
};

template <> struct std::hash<Assisi::ECS::InstanceId>
{
    [[nodiscard]] std::size_t operator()(Assisi::ECS::InstanceId id) const noexcept
    {
        return std::hash<std::uint32_t>{}(id.value);
    }
};
