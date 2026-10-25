/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/Network.hpp>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/DecodedMessage.hpp>
#include <Assisi/Core/Reflect/MessageRegistry.hpp>

#include <Assisi/NetSync/NetSession.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>

#include <utility>

namespace Assisi::App
{

namespace
{

const Core::Reflect::MessageMeta *MetaOf(std::type_index type)
{
    const Core::Reflect::MessageRegistry &registry = Core::Reflect::MessageRegistry::Instance();
    return registry.ById(registry.IdOf(type));
}

} // namespace

NetRole Network::Role() const
{
    if (_session != nullptr && _session->IsHost())
    {
        return NetRole::Host;
    }
    if (_session != nullptr && _session->IsClient())
    {
        return NetRole::Client;
    }
    return NetRole::Offline;
}

Core::ClientId Network::LocalClient() const
{
    if (_session != nullptr && _session->IsActive())
    {
        return _session->LocalClientId();
    }
    return Core::HostClientId;
}

void Network::Deliver(Core::EventQueue &events)
{
    _toSelf.Deliver(events);
    if (_session != nullptr)
    {
        _session->Deliver(events);
    }
}

void Network::Attach(NetSync::NetSession &session) { _session = &session; }

void Network::Detach() { _session = nullptr; }

bool Network::SendIntent(const void *intent, std::type_index type)
{
    if (_session != nullptr && _session->IsActive())
    {
        return _session->SendIntent(intent, type);
    }
    return SendToSelf(intent, type);
}

bool Network::SendEvent(const void *event, std::type_index type, Audience audience)
{
    if (_session != nullptr && _session->IsClient())
    {
        RefuseEvent(type);
        return false;
    }
    if (_session != nullptr && _session->IsHost())
    {
        using Recipients = NetSync::ReplicationServer::Recipients;
        NetSync::ReplicationServer &server = *_session->Server();
        switch (audience.to)
        {
        case To::Everyone:
            server.SendEvent(event, type, Recipients::AllRelevant, Core::InvalidClientId);
            break;
        case To::Controller:
            server.SendEvent(event, type, Recipients::Directed, _session->ControllerOf(audience.entity));
            break;
        case To::One:
            server.SendEvent(event, type, Recipients::Directed, audience.client);
            break;
        case To::AllBut:
        case To::Count_:
            server.SendEvent(event, type, Recipients::ExceptInstigator, audience.client);
            break;
        }
        return true;
    }
    // Addressed to a player who is not here: sent, and nobody hears it, as on a
    // host nobody joined.
    return !ReachesAlone(audience) || SendToSelf(event, type);
}

bool Network::ReachesAlone(const Audience &audience)
{
    switch (audience.to)
    {
    case To::Everyone:
    case To::Controller:
        return true;
    case To::One:
        return audience.client == Core::HostClientId;
    case To::AllBut:
    case To::Count_:
        return audience.client != Core::HostClientId;
    }
    return false;
}

bool Network::SendToSelf(const void *message, std::type_index type)
{
    const Core::Reflect::MessageMeta *meta = MetaOf(type);
    if (meta == nullptr)
    {
        return false;
    }
    // Through the wire's codec rather than copied, as a host reads its own: a
    // member that is not an AFIELD arrives as its default, alone as online, so
    // a game cannot come to depend on what only a world played alone keeps.
    Core::BitWriter writer;
    if (!Core::Reflect::WriteMessage(*meta, message, writer))
    {
        return false;
    }
    Core::BitReader reader(writer.Data());
    (void)Core::Reflect::ReadMessageId(reader);
    Core::Reflect::DecodedMessage decoded = meta->decode(*meta, reader, nullptr);
    if (!decoded)
    {
        return false;
    }
    _toSelf.Hold(Core::HostClientId, std::move(decoded));
    return true;
}

void Network::RefuseEvent(std::type_index type)
{
    if (!_refused.insert(type).second)
    {
        return;
    }
    const Core::Reflect::MessageMeta *meta = MetaOf(type);
    Core::Log::Warn("Network: a client sent the event '{}', which only the host sends. Ask the host with an "
                    "intent, and let it send the event.",
                    meta != nullptr ? meta->name : type.name());
}

} // namespace Assisi::App
