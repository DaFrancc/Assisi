/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ClientId.hpp
/// @brief Who a player is, for the length of a networked session.

#include <Assisi/Core/StrongId.hpp>

#include <cstdint>
#include <type_traits>

namespace Assisi::Core
{

/// @brief Session-scoped identity for a *participant*: the thing `ControlledBy`
/// names, directed messages address, received messages are from, and logs
/// blame.
///
/// Distinct from `Net::ConnectionId` on purpose, and a wrapper type rather than
/// an alias so the compiler enforces it. A ConnectionId is a transport handle —
/// meaningful only inside the server process, meaningless on any other machine
/// — and the two id spaces are one honest mistake apart. Replicating a
/// ConnectionId would be replicating a pointer.
///
/// In Core rather than NetSync because a game names it in every build: a
/// message read offline is from the host, and a build without networking still
/// reads messages.
///
/// Conventions, matching the uniform zero-means-invalid rule the networking
/// ids follow (`InvalidNetId`, `Net::InvalidConnection`):
///
///  - **0 = nobody.** A default-constructed id claims nothing, which is what
///    makes a default-constructed `ControlledBy` harmless.
///  - **1 = the host itself** — the listen server's own player, which is not a
///    connection and never will be (see NetSession.hpp on why there is no
///    loopback client). Offline, the one player is the host.
///  - **2… = remote clients**, assigned monotonically as connections arrive and
///    **never reused within a session**. Reuse would make "who did this"
///    ambiguous in a log line or a late-arriving message, and saves nothing.
struct ClientId
{
    std::uint32_t value = 0;

    [[nodiscard]] constexpr bool IsValid() const { return value != 0; }

    friend constexpr bool operator==(ClientId, ClientId)  = default;
    friend constexpr auto operator<=>(ClientId, ClientId) = default;
};

/// @brief The never-valid ClientId. Claims nothing, controls nothing.
inline constexpr ClientId InvalidClientId{0};

/// @brief The listen server's own player, and offline the only one. Not a
/// connection — see ClientId.
inline constexpr ClientId HostClientId{1};

/// Reaches a peer inside a component (`ControlledBy`) and, as a bare field, in
/// `ServerHello.clientId` — which encodes it as a plain `WriteVarUInt32` of the
/// value rather than through `WriteVarId`. Declaring the opt-in here is what
/// keeps that a decision rather than an oversight.
template <> struct IsStrongId<ClientId> : std::true_type
{
};
static_assert(StrongId<ClientId>);

} // namespace Assisi::Core
