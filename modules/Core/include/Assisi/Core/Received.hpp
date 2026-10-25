/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Received.hpp
/// @brief A network message, as the systems of the world it arrived in read it.
///
/// A message that passes the network's checks is pushed into its world's events
/// as a Received<T>, and any system reads it as it reads any event:
///
/// @code
/// for (const auto &[sender, ring] : ctx.events.Read<Received<RingBell>>())
///     RingFor(sender, ring.bell);
/// @endcode
///
/// Only the engine makes one, so what a system reads came through those checks,
/// and `sender` is who the connection says sent it, never what the payload
/// claims.

#include <Assisi/Core/ClientId.hpp>
#include <Assisi/Core/Reflect/MessageMeta.hpp>
// Generated: which types are AMSG, so a Received of anything else does not
// compile.
#include <Assisi/Core/Reflect/MessageTraits.hpp>

#include <utility>

namespace Assisi::Core
{

/// @brief Message @p T as received from @p sender.
///
/// Exactly these two members, sender first, so a structured binding reads like
/// a query: `const auto &[sender, message]`.
template <typename T>
class Received
{
    static_assert(Reflect::DeclaredMessage<T>,
                  "Received<T> holds a network message: declare T with AMSG(intent, ...) or AMSG(event, ...)");

public:
    /// Who sent it: a client for an intent, the host for an event, and offline
    /// the host for both.
    ClientId sender;
    T message;

private:
    friend class Reflect::DecodedMessage;

    Received(ClientId from, T value) : sender(from), message(std::move(value)) {}
};

} // namespace Assisi::Core

// A system names it as often as Mut, so it is as short.
using Assisi::Core::Received;
