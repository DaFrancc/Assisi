/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Network.hpp
/// @brief What a world's systems know of the network and say to it: `ctx.net`.
///
/// Every world has one, played alone or online, so one system works either way:
///
/// @code
/// // A player asks; any machine can.
/// ctx.net.SendIntent(RingBell{.ringer = me, .bell = 3});
///
/// // The authority answers, telling everyone who can see the bell.
/// if (ctx.net.IsAuthority())
///     for (const auto &[sender, ring] : ctx.events.Read<Received<RingBell>>())
///         ctx.net.Send(BellRang{.bell = ring.bell});
/// @endcode
///
/// A message that passes its checks reaches the systems of the world it was
/// sent to as an event, Received<T>, at the start of that world's next tick.
///
/// Played alone, a world is a host nobody joined: it is the authority, its one
/// player is the host, and what it sends reaches itself the same way — an
/// intent from the host, and an event unless it is addressed to another player,
/// who is not there. Played alone, nothing is checked against who controls
/// what: the one player controls everything.

#include <Assisi/Core/ClientId.hpp>
#include <Assisi/Core/Reflect/MessageInbox.hpp>
#include <Assisi/Core/Reflect/MessageMeta.hpp>
#include <Assisi/Core/Reflect/MessageTraits.hpp>
#include <Assisi/ECS/Entity.hpp>

#include <cstdint>
#include <typeindex>
#include <unordered_set>

namespace Assisi::Core
{
class EventQueue;
}
namespace Assisi::NetSync
{
class NetSession;
}

namespace Assisi::App
{

/// @brief What a machine is to the world it plays.
enum class NetRole : std::uint8_t
{
    Offline, ///< Played alone, a host nobody joined.
    Host,    ///< The authority others joined, played or dedicated.
    Client,  ///< Joined to a host, whose word the world is.
    Count_
};

class Network
{
public:
    Network() = default;
    Network(const Network &) = delete;
    Network &operator=(const Network &) = delete;

    [[nodiscard]] NetRole Role() const;
    [[nodiscard]] bool IsHost() const { return Role() == NetRole::Host; }
    [[nodiscard]] bool IsClient() const { return Role() == NetRole::Client; }

    /// @brief Whether this machine's word is the world's: a host, or a world
    /// played alone.
    [[nodiscard]] bool IsAuthority() const { return Role() != NetRole::Client; }

    /// @brief This machine's player: the host's id on a host and played alone,
    /// the id the host gave it on a client, and InvalidClientId on a client
    /// that has not heard from the host yet.
    [[nodiscard]] Core::ClientId LocalClient() const;

    /// @brief Asks the authority for something. From a client it crosses the
    /// wire; on a host or played alone it is this machine asking itself, and
    /// passes the same checks.
    /// @return false on a client that has not joined yet.
    template <typename T>
    bool SendIntent(const T &intent)
    {
        static_assert(Core::Reflect::MessageTraits<T>::direction == Core::Reflect::MessageDirection::Intent,
                      "SendIntent takes an AMSG(intent, ...). An event is the authority's word about what "
                      "happened; send it with Send.");
        return SendIntent(&intent, typeid(T));
    }

    /// @brief Tells everyone who can see the entity the event is about, or
    /// everyone, for an `independent` event. The host is told too.
    /// @return false on a client, which is not the authority, having logged it
    /// the first time for this type.
    template <typename T>
    bool Send(const T &event)
    {
        AssertEvent<T>();
        return SendEvent(&event, typeid(T),
                         Audience{.entity = ECS::NullEntity, .client = Core::InvalidClientId, .to = To::Everyone});
    }

    /// @brief Tells whoever controls @p entity, and nobody when nobody does.
    template <typename T>
    bool SendToController(ECS::Entity entity, const T &event)
    {
        AssertEvent<T>();
        return SendEvent(&event, typeid(T),
                         Audience{.entity = entity, .client = Core::InvalidClientId, .to = To::Controller});
    }

    /// @brief Tells @p client alone.
    template <typename T>
    bool SendTo(Core::ClientId client, const T &event)
    {
        AssertEvent<T>();
        return SendEvent(&event, typeid(T), Audience{.entity = ECS::NullEntity, .client = client, .to = To::One});
    }

    /// @brief Tells everyone Send would, except @p instigator, who has shown
    /// it already.
    template <typename T>
    bool SendExcept(Core::ClientId instigator, const T &event)
    {
        AssertEvent<T>();
        return SendEvent(&event, typeid(T),
                         Audience{.entity = ECS::NullEntity, .client = instigator, .to = To::AllBut});
    }

    /// @brief Pushes every message that has arrived for this world since the
    /// last call into @p events, as Received<T>. StepWorld calls it at the start
    /// of each live tick.
    void Deliver(Core::EventQueue &events);

    /// @brief Plays the world over @p session from now on, until Detach.
    /// NetworkedPlay attaches the world it plays.
    void Attach(NetSync::NetSession &session);
    void Detach();

private:
    enum class To : std::uint8_t
    {
        Everyone,
        Controller,
        One,
        AllBut,
        Count_
    };

    /// Who an event is for.
    struct Audience
    {
        ECS::Entity entity = ECS::NullEntity; ///< For Controller.
        Core::ClientId client;                ///< For One, and the one AllBut skips.
        To to = To::Everyone;
    };

    template <typename T>
    static constexpr void AssertEvent()
    {
        static_assert(Core::Reflect::MessageTraits<T>::direction == Core::Reflect::MessageDirection::Event,
                      "Send takes an AMSG(event, ...). An intent is a request of the authority; send it with "
                      "SendIntent.");
    }

    /// The type-erased halves of the sends above.
    bool SendIntent(const void *intent, std::type_index type);
    bool SendEvent(const void *event, std::type_index type, Audience audience);

    /// Whether an event for @p audience reaches a world played alone, whose one
    /// player is the host and controls everything.
    [[nodiscard]] static bool ReachesAlone(const Audience &audience);

    /// Played alone: encodes @p message and decodes it back, as a host does its
    /// own, and holds it for the next Deliver from the host.
    bool SendToSelf(const void *message, std::type_index type);

    /// Logs, once per type, that a client tried to send an event.
    void RefuseEvent(std::type_index type);

    NetSync::NetSession *_session = nullptr;
    /// What a world played alone sent itself.
    Core::Reflect::MessageInbox _toSelf;
    std::unordered_set<std::type_index> _refused;
};

} // namespace Assisi::App
