/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Reflect/MessageMeta.hpp
/// @brief Runtime descriptor for a reflected *message* type — what other engines
/// spell as an RPC.
///
/// A message is a plain struct annotated `AMSG(direction, reliability)`, and
/// that is the whole design. It is not a function, and the difference is not
/// cosmetic: a struct goes through reflectgen's existing field path, so it gets
/// binary and JSON codecs, the inspector, and — the part that matters most —
/// inclusion in the protocol hash, all for free. A function-shaped RPC would
/// need a signature parser, a parameter model, and dispatch codegen, and would
/// lose the hash inclusion, which is the thing that makes a mismatched pair
/// refuse to connect instead of misparsing each other.
///
/// Addressing is data, not a receiver. There is no "call this on that object"
/// because there is no object: a message about an entity carries a `NetId`
/// field like any other field.

#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <typeindex>
#include <vector>

#include <nlohmann/json.hpp>

#include <Assisi/Core/Reflect/FieldMeta.hpp>
#include <Assisi/Core/StrongId.hpp>

namespace Assisi::Core::Reflect
{

/// @brief Dense wire identity of a message type, assigned alphabetically at
/// registry finalize — the same scheme, and the same safety argument, as
/// `ComponentId`: the handshake refuses on a protocol-hash mismatch, and
/// hash-equal builds have identical message sets and therefore identical ids.
/// An aggregate, matching ComponentId and the NetSync ids: aggregate
/// initialization (`MessageId{7}`) is the only way in, which is what blocks the
/// implicit conversion in both directions. No arithmetic — the one place a
/// count becomes an id is the registry's finalize loop, and it says so.
///
/// Note the sentinel differs from ComponentId's despite the shared scheme: zero
/// is invalid *here*, because message ids start at one.
struct MessageId
{
    std::uint32_t value = 0;

    [[nodiscard]] constexpr bool IsValid() const { return value != 0; }

    friend constexpr bool operator==(MessageId, MessageId)  = default;
    friend constexpr auto operator<=>(MessageId, MessageId) = default;
};

/// @brief The never-valid MessageId. Zero, so a value-initialized id is invalid
/// and dense ids start at one.
inline constexpr MessageId kInvalidMessageId{0};

/// @brief Which way a message travels, and therefore who is allowed to send it.
///
/// Mandatory on every `AMSG`, spelled first. It is not a default anyone should
/// have to remember, and it gives the receive side a check that costs nothing:
/// an `Event` arriving *from* a client is rejected before the payload is even
/// looked at, because the vocabulary itself says clients do not speak it.
enum class MessageDirection : std::uint8_t
{
    /// Client → server: "I would like this to happen." Never trusted, always
    /// validated, and the server is free to decide nothing happens.
    Intent = 1,

    /// Server → client: "this happened." Authoritative by construction — the
    /// only machine that can send one is the machine that decides.
    Event = 2,
};

/// @brief Whether a message must arrive, spelled second and also mandatory.
///
/// Per *type*, never per send. A message that is sometimes reliable has an
/// unclear meaning, and per-type declaration is what lets a diagnostic panel
/// show reliable traffic broken down by type.
enum class MessageReliability : std::uint8_t
{
    /// Fire and forget. The right answer for the spammy, freshest-wins cases —
    /// map pings, hit sparks — where a resent stale message is worse than a
    /// lost one.
    Unreliable = 0,

    /// Must arrive. Rare by design: both Quake 3 and Unreal hit the same
    /// reliable-buffer cliff, and Unreal's overflow *closes the connection*.
    Reliable = 1,
};

/// @brief Runtime descriptor for one `AMSG` type.
struct MessageMeta
{
    std::string            name;
    std::type_index        typeIndex;
    std::vector<FieldMeta> fields;

    MessageDirection   direction   = MessageDirection::Intent;
    MessageReliability reliability = MessageReliability::Unreliable;

    /// @brief `AMSG(..., independent)`: this message names no entity, so
    /// nothing about it can be scoped or deferred by relevancy.
    ///
    /// Chat lines, round banners, anything whose meaning does not depend on the
    /// recipient already knowing about some entity. An independent message
    /// bypasses the hold-until-the-target-arrives queue entirely, because there
    /// is no target to wait for.
    bool independent = false;

    /// @brief Dense wire id, finalized by the registry. `kInvalidMessageId`
    /// until then.
    MessageId id = kInvalidMessageId;

    /// @brief Serialize an instance to JSON. For tests, tooling, and logging —
    /// the wire form is the binary codec, never this.
    std::function<nlohmann::json(const void *message_ptr)> serialize;

    /// @brief Deserialize JSON into a caller-owned instance. Absent keys leave
    /// the instance's current value untouched.
    ///
    /// @return false when a field is present but unreadable, having logged which
    ///         one. Absent keys are not failures — see ComponentMeta::addToScene.
    std::function<bool(const nlohmann::json &j, void *out_ptr)> deserialize;
};

/// @brief The same three facts as MessageMeta's grammar fields, available at
/// compile time. Specialized by generated code, once per `AMSG` type.
///
/// Undefined for anything else, on purpose: it is what turns "you sent an event
/// from a client" into a compile error at the send site instead of a dropped
/// packet at the receive site, and it makes passing a struct that was never
/// declared a message fail with an incomplete type rather than silently
/// encoding nothing.
template <typename T>
struct MessageTraits;

} // namespace Assisi::Core::Reflect

namespace Assisi::Core
{
/// Encodes as a varint — the id prefix on every intent and event packet.
template <> struct IsStrongId<Reflect::MessageId> : std::true_type
{
};
static_assert(StrongId<Reflect::MessageId>);
} // namespace Assisi::Core

/// Prints as the bare number, so a log line reads "message 7" without every
/// call site spelling `.value`. Without it the type would be strictly worse to
/// hold than the integer it replaces, which is how a good rule gets worked
/// around.
template <> struct std::formatter<Assisi::Core::Reflect::MessageId> : std::formatter<std::uint32_t>
{
    auto format(Assisi::Core::Reflect::MessageId id, std::format_context &ctx) const
    {
        return std::formatter<std::uint32_t>::format(id.value, ctx);
    }
};

template <> struct std::hash<Assisi::Core::Reflect::MessageId>
{
    [[nodiscard]] std::size_t operator()(Assisi::Core::Reflect::MessageId id) const noexcept
    {
        return std::hash<std::uint32_t>{}(id.value);
    }
};
