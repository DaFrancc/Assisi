/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Net/NetTransport.hpp>

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>
#include <steam/steamnetworkingtypes.h>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Logger.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

// Only for the address-family probe behind HostSupportsIPv6(); see Listen().
#ifdef _WIN32
#    include <winsock2.h>
#else
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
#endif

namespace Assisi::Net
{
namespace
{

/// Lane priorities handed to ConfigureConnectionLanes, indexed by Lane. Lower
/// value = higher priority, so Snapshot outranks Control outranks Bulk: a late
/// snapshot is worthless (the next one replaces it), while control traffic is
/// reliable and can afford to wait a packet, and Bulk exists precisely to be
/// the thing that yields. Weights are only consulted between lanes of *equal*
/// priority, so with three distinct priorities they are irrelevant — we pass
/// nullptr rather than invent numbers that never get read.
constexpr std::array<int, LaneCount> kLanePriorities{
    1, // Control
    0, // Snapshot
    2, // Bulk
};

/// GNS's global init is refcounted here rather than left to the caller: the
/// listen server constructs two NetTransports in one process, and neither can
/// be the one that owns library lifetime.
std::mutex   g_libraryMutex;
std::int32_t g_libraryRefs = 0;

/// Does this host have IPv6 at all?
///
/// Listen() needs this to tell two very different situations apart, both of
/// which surface identically as "GNS bound IPv4 only". Asked once and cached:
/// the answer cannot change while the process runs, and the probe is a syscall
/// we would otherwise repeat on every Host().
bool HostSupportsIPv6()
{
    static const bool supported = []
    {
#ifdef _WIN32
        // Safe without our own WSAStartup: this is only ever reached after GNS
        // has opened a listen socket, so Winsock is already initialised.
        const SOCKET probe = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
        if (probe == INVALID_SOCKET)
            return false;
        ::closesocket(probe);
#else
        const int probe = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
        if (probe < 0)
            return false;
        ::close(probe);
#endif
        return true;
    }();
    return supported;
}

} // namespace

// ---------------------------------------------------------------------------

struct NetTransport::Impl
{
    ISteamNetworkingSockets *sockets = nullptr;

    HSteamListenSocket listenSocket = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup pollGroup    = k_HSteamNetPollGroup_Invalid;

    // Two-way handle mapping. The forward map is what Send/Close/stats consult;
    // the reverse map is what the connection-status callback needs, since GNS
    // hands us its own handle and nothing else.
    std::unordered_map<ConnectionId, HSteamNetConnection> byId;
    std::unordered_map<HSteamNetConnection, ConnectionId> byHandle;

    // Ids start at 1 and only ever increase: InvalidConnection is 0, and never
    // recycling means a stale handle is always rejected instead of silently
    // addressing whoever took its slot.
    ConnectionId nextId = 1;

    /// Events produced by the status callback between Poll() calls. Poll()
    /// appends received messages to the caller's vector after draining this, so
    /// a Connected event always precedes that connection's first message.
    std::vector<NetEvent> pendingEvents;

    std::string lastError;

    // ---- instance registry, for routing GNS's global callback -------------
    //
    // GNS's connection-status callback is a plain function pointer with no user
    // data, so the callback has to work out which NetTransport a connection
    // belongs to. With at most a handful of instances per process (a listen
    // server runs two), a linear scan over live instances is simpler and more
    // obviously correct than smuggling `this` through GNS's per-connection
    // user-data field — which would also have to survive the inherit-from-listen-
    // socket path for accepted connections.
    static std::vector<Impl *> &Instances()
    {
        static std::vector<Impl *> instances;
        return instances;
    }

    /// True if this instance is the one that should handle @p info.
    bool Owns(const SteamNetConnectionStatusChangedCallback_t &info) const
    {
        if (byHandle.contains(info.m_hConn))
            return true;
        // A brand-new incoming connection is not in the map yet; its listen
        // socket is the only link back to us.
        return info.m_info.m_hListenSocket != k_HSteamListenSocket_Invalid &&
               info.m_info.m_hListenSocket == listenSocket;
    }

    ConnectionId Register(HSteamNetConnection handle)
    {
        const ConnectionId id = nextId++;
        byId.emplace(id, handle);
        byHandle.emplace(handle, id);
        return id;
    }

    void Unregister(HSteamNetConnection handle)
    {
        const auto it = byHandle.find(handle);
        if (it == byHandle.end())
            return;
        byId.erase(it->second);
        byHandle.erase(it);
    }

    /// Give a connection its poll group and lane configuration. Both are
    /// per-connection and neither is inherited from the listen socket, so every
    /// path that produces a usable connection (accept, connect, socket pair)
    /// has to come through here.
    void Adopt(HSteamNetConnection handle)
    {
        sockets->SetConnectionPollGroup(handle, pollGroup);
        const EResult result = sockets->ConfigureConnectionLanes(handle, static_cast<int>(LaneCount),
                                                                 kLanePriorities.data(), nullptr);
        if (result != k_EResultOK)
            Core::Log::Warn("Net: ConfigureConnectionLanes failed ({}); lanes fall back to a single stream",
                            static_cast<std::int32_t>(result));
    }

    void OnStatusChanged(const SteamNetConnectionStatusChangedCallback_t &info);

    /// The one global callback GNS calls; fans out to whichever instance owns
    /// the connection. Registered once, on the first NetTransport construction.
    /// A static member rather than a free function only because Impl is private
    /// to NetTransport — GNS takes it as a plain function pointer either way.
    static void StatusChangedCallback(SteamNetConnectionStatusChangedCallback_t *info)
    {
        if (info == nullptr)
            return;
        for (Impl *impl : Instances())
        {
            if (impl->Owns(*info))
            {
                impl->OnStatusChanged(*info);
                return;
            }
        }
    }
};

namespace
{

void DebugOutputCallback(ESteamNetworkingSocketsDebugOutputType type, const char *message)
{
    // Arrives on GNS's service thread, so keep it to a log call — the logger is
    // the only shared state touched here.
    if (type <= k_ESteamNetworkingSocketsDebugOutputType_Warning)
        Core::Log::Warn("GNS: {}", message != nullptr ? message : "");
    else
        Core::Log::Trace("GNS: {}", message != nullptr ? message : "");
}

} // namespace

void NetTransport::Impl::OnStatusChanged(const SteamNetConnectionStatusChangedCallback_t &info)
{
    switch (info.m_info.m_eState)
    {
    case k_ESteamNetworkingConnectionState_Connecting:
    {
        // Only incoming connections (those with a listen socket) need action
        // here; an outbound connection in this state is simply still in flight.
        if (info.m_info.m_hListenSocket == k_HSteamListenSocket_Invalid)
            break;

        if (sockets->AcceptConnection(info.m_hConn) != k_EResultOK)
        {
            sockets->CloseConnection(info.m_hConn, 0, "accept failed", false);
            Core::Log::Warn("Net: rejected incoming connection (accept failed)");
            break;
        }
        Adopt(info.m_hConn);
        Register(info.m_hConn);
        break;
    }

    case k_ESteamNetworkingConnectionState_Connected:
    {
        // Outbound connections reach this state without ever passing through
        // the accept path above, so this is where they get adopted.
        const auto it = byHandle.find(info.m_hConn);
        if (it == byHandle.end())
            break; // Not ours after all, or already torn down.
        if (info.m_eOldState == k_ESteamNetworkingConnectionState_Connecting &&
            info.m_info.m_hListenSocket == k_HSteamListenSocket_Invalid)
            Adopt(info.m_hConn);

        NetEvent event;
        event.type       = NetEvent::Type::Connected;
        event.connection = it->second;
        pendingEvents.push_back(std::move(event));
        break;
    }

    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
    {
        const auto it = byHandle.find(info.m_hConn);
        if (it != byHandle.end())
        {
            // Report a disconnect only for a connection the caller was told
            // about. A connection that died during its handshake was never
            // announced, so announcing its death would be the first the caller
            // ever heard of it.
            if (info.m_eOldState == k_ESteamNetworkingConnectionState_Connected)
            {
                NetEvent event;
                event.type        = NetEvent::Type::Disconnected;
                event.connection  = it->second;
                event.closeReason = info.m_info.m_eEndReason;
                event.closeDebug  = info.m_info.m_szEndDebug;
                pendingEvents.push_back(std::move(event));
            }
            Unregister(info.m_hConn);
        }
        // Required even for a connection the peer closed: GNS holds the handle's
        // resources until the local side closes it too.
        sockets->CloseConnection(info.m_hConn, 0, nullptr, false);
        break;
    }

    default:
        // None / FindingRoute / FinWait / Linger: nothing for us to do.
        break;
    }
}

// ---------------------------------------------------------------------------

NetTransport::NetTransport() : _impl(std::make_unique<Impl>())
{
    {
        const std::lock_guard<std::mutex> lock(g_libraryMutex);
        if (g_libraryRefs == 0)
        {
            SteamNetworkingErrMsg errMsg{};
            if (!GameNetworkingSockets_Init(nullptr, errMsg))
            {
                // Nothing can work after this; fail loudly rather than hand back
                // an object whose every method silently returns false.
                ASSISI_ASSERT(false, "GameNetworkingSockets_Init failed");
                Core::Log::Error("Net: GameNetworkingSockets_Init failed: {}", static_cast<const char *>(errMsg));
            }
            SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(
                &Impl::StatusChangedCallback);
        }
        ++g_libraryRefs;
    }

    _impl->sockets   = SteamNetworkingSockets();
    _impl->pollGroup = _impl->sockets->CreatePollGroup();
    Impl::Instances().push_back(_impl.get());
}

NetTransport::~NetTransport()
{
    for (const auto &[id, handle] : _impl->byId)
        _impl->sockets->CloseConnection(handle, 0, "shutting down", false);
    _impl->byId.clear();
    _impl->byHandle.clear();

    if (_impl->pollGroup != k_HSteamNetPollGroup_Invalid)
        _impl->sockets->DestroyPollGroup(_impl->pollGroup);
    if (_impl->listenSocket != k_HSteamListenSocket_Invalid)
        _impl->sockets->CloseListenSocket(_impl->listenSocket);

    std::vector<Impl *> &instances = Impl::Instances();
    instances.erase(std::remove(instances.begin(), instances.end(), _impl.get()), instances.end());

    const std::lock_guard<std::mutex> lock(g_libraryMutex);
    if (--g_libraryRefs == 0)
        GameNetworkingSockets_Kill();
}

bool NetTransport::Listen(std::uint16_t port)
{
    if (_impl->listenSocket != k_HSteamListenSocket_Invalid)
    {
        _impl->lastError = "already listening";
        return false;
    }

    // Cleared (all-zero) means "any address"; on a dual-stack host that accepts
    // IPv4 connections too, via IPv4-mapped addresses.
    SteamNetworkingIPAddr address{};
    address.Clear();
    address.m_port = port;

    _impl->listenSocket = _impl->sockets->CreateListenSocketIP(address, 0, nullptr);
    if (_impl->listenSocket == k_HSteamListenSocket_Invalid)
    {
        _impl->lastError = "failed to bind listen socket";
        return false;
    }

    // A valid handle is not proof the port was free.
    //
    // We asked for the all-zero address, which GNS reads as "auto": it binds
    // IPv6 dual stack, and *silently falls back to IPv4* if that bind fails —
    // without ever looking at why it failed. The fallback exists for hosts
    // without IPv6, but EADDRINUSE takes the same path, so a port already held
    // by a dual-stack socket still yields a live listen socket bound to
    // 0.0.0.0. On Windows that second bind is permitted, because IPv4 and IPv6
    // keep separate wildcard port spaces and an existing dual-stack bind does
    // not reserve the plain-IPv4 slot. It is not a harmless duplicate either:
    // the IPv4-only latecomer takes delivery of the IPv4 traffic, so a second
    // Host() on a busy port would quietly steal the first server's clients.
    //
    // GNS documents the tell (isteamnetworkingsockets.h): ::0 back means "any
    // IPv4 or IPv6", ::ffff:0:0 means "any IPv4". So getting IPv4 when we asked
    // for both means the dual-stack bind lost — to a conflict if this host has
    // IPv6, which is the case we must reject, or legitimately if it does not,
    // which we must keep working. Hence the capability probe.
    //
    // Linux is unaffected in practice: there a dual-stack [::] bind does own
    // the IPv4 port too, so a duplicate fails both attempts and we returned
    // above. This is the same postcondition on both platforms; only Windows
    // ever reaches it. If the readback itself fails we accept the socket rather
    // than refuse to host on an unproven suspicion.
    SteamNetworkingIPAddr bound{};
    bound.Clear();
    if (_impl->sockets->GetListenSocketAddress(_impl->listenSocket, &bound) && bound.IsIPv4() &&
        HostSupportsIPv6())
    {
        _impl->sockets->CloseListenSocket(_impl->listenSocket);
        _impl->listenSocket = k_HSteamListenSocket_Invalid;
        _impl->lastError    = "port already in use";
        return false;
    }

    _impl->lastError.clear();
    return true;
}

ConnectionId NetTransport::Connect(std::string_view address, std::uint16_t port)
{
    // ParseString needs a null-terminated string and a string_view is not
    // guaranteed to be one.
    const std::string addressText(address);

    SteamNetworkingIPAddr parsed{};
    parsed.Clear();
    if (!parsed.ParseString(addressText.c_str()))
    {
        _impl->lastError = "could not parse address (an IP literal is required; DNS is not resolved here)";
        return InvalidConnection;
    }
    parsed.m_port = port;

    const HSteamNetConnection handle = _impl->sockets->ConnectByIPAddress(parsed, 0, nullptr);
    if (handle == k_HSteamNetConnection_Invalid)
    {
        _impl->lastError = "ConnectByIPAddress failed";
        return InvalidConnection;
    }

    _impl->lastError.clear();
    return _impl->Register(handle);
}

std::pair<ConnectionId, ConnectionId> NetTransport::CreateLoopbackPair(bool useNetworkLoopback)
{
    HSteamNetConnection first  = k_HSteamNetConnection_Invalid;
    HSteamNetConnection second = k_HSteamNetConnection_Invalid;

    if (!_impl->sockets->CreateSocketPair(&first, &second, useNetworkLoopback, nullptr, nullptr))
    {
        _impl->lastError = "CreateSocketPair failed";
        return {InvalidConnection, InvalidConnection};
    }

    _impl->Adopt(first);
    _impl->Adopt(second);
    const ConnectionId firstId  = _impl->Register(first);
    const ConnectionId secondId = _impl->Register(second);

    // A socket pair is born connected and GNS posts no callbacks for it. Emit
    // the Connected events ourselves so a caller driving both a loopback and a
    // real client sees one event stream, not two shapes of one.
    for (const ConnectionId id : {firstId, secondId})
    {
        NetEvent event;
        event.type       = NetEvent::Type::Connected;
        event.connection = id;
        _impl->pendingEvents.push_back(std::move(event));
    }

    _impl->lastError.clear();
    return {firstId, secondId};
}

void NetTransport::Close(ConnectionId connection, bool linger)
{
    const auto it = _impl->byId.find(connection);
    if (it == _impl->byId.end())
        return;

    const HSteamNetConnection handle = it->second;
    _impl->sockets->CloseConnection(handle, 0, nullptr, linger);
    _impl->Unregister(handle);
}

bool NetTransport::Send(ConnectionId connection, std::span<const std::byte> data, SendMode mode, Lane lane)
{
    const auto it = _impl->byId.find(connection);
    if (it == _impl->byId.end())
    {
        _impl->lastError = "unknown connection";
        return false;
    }

    // SendMessageToConnection always writes lane 0, so anything that wants a
    // lane has to go through an allocated message. Use that path for every send
    // rather than branching: one code path, and the allocation comes out of
    // GNS's own pool.
    SteamNetworkingMessage_t *message = SteamNetworkingUtils()->AllocateMessage(static_cast<int>(data.size()));
    if (message == nullptr)
    {
        _impl->lastError = "AllocateMessage failed";
        return false;
    }

    if (!data.empty())
        std::memcpy(message->m_pData, data.data(), data.size());
    message->m_conn    = it->second;
    message->m_idxLane = static_cast<std::uint16_t>(lane);
    // NoNagle on both modes: this transport carries per-tick game state, where
    // coalescing a snapshot into the next packet trades latency for a bandwidth
    // win we do not need at this scale.
    message->m_nFlags = mode == SendMode::Reliable ? k_nSteamNetworkingSend_ReliableNoNagle
                                                   : k_nSteamNetworkingSend_UnreliableNoNagle;

    // Declared as GNS's own int64 rather than std::int64_t: on LP64 the latter
    // is `long`, GNS's is `long long`, and while both are 64 bits a pointer to
    // one is not a pointer to the other. Take the API at its word instead of
    // casting the out-parameter.
    int64 result = 0;
    _impl->sockets->SendMessages(1, &message, &result, true);
    if (result < 0)
    {
        _impl->lastError = "SendMessages rejected the message";
        return false;
    }

    _impl->lastError.clear();
    return true;
}

void NetTransport::Poll(std::vector<NetEvent> &outEvents)
{
    outEvents.clear();

    // RunCallbacks dispatches connection-status changes synchronously on this
    // thread, appending to pendingEvents, so it must run before we move them out.
    _impl->sockets->RunCallbacks();

    outEvents.swap(_impl->pendingEvents);
    _impl->pendingEvents.clear();

    // Drain in bounded batches so one very busy tick cannot spin here forever.
    constexpr int             kBatchSize = 64;
    SteamNetworkingMessage_t *batch[kBatchSize];
    for (;;)
    {
        const int received = _impl->sockets->ReceiveMessagesOnPollGroup(_impl->pollGroup, batch, kBatchSize);
        if (received <= 0)
            break;

        for (int i = 0; i < received; ++i)
        {
            SteamNetworkingMessage_t *message = batch[i];

            const auto it = _impl->byHandle.find(message->m_conn);
            if (it != _impl->byHandle.end())
            {
                NetEvent event;
                event.type       = NetEvent::Type::Message;
                event.connection = it->second;
                event.lane       = message->m_idxLane < LaneCount ? static_cast<Lane>(message->m_idxLane)
                                                                  : Lane::Control;

                const auto *bytes = static_cast<const std::byte *>(message->m_pData);
                event.payload.assign(bytes, bytes + message->m_cbSize);
                outEvents.push_back(std::move(event));
            }

            message->Release();
        }

        if (received < kBatchSize)
            break;
    }
}

bool NetTransport::GetConnectionStats(ConnectionId connection, ConnectionStats &outStats) const
{
    const auto it = _impl->byId.find(connection);
    if (it == _impl->byId.end())
        return false;

    SteamNetConnectionRealTimeStatus_t status{};
    if (_impl->sockets->GetConnectionRealTimeStatus(it->second, &status, 0, nullptr) != k_EResultOK)
        return false;

    outStats.pingMs                  = status.m_nPing;
    outStats.connectionQualityLocal  = status.m_flConnectionQualityLocal;
    outStats.connectionQualityRemote = status.m_flConnectionQualityRemote;
    outStats.outPacketsPerSec        = status.m_flOutPacketsPerSec;
    outStats.outBytesPerSec          = status.m_flOutBytesPerSec;
    outStats.inPacketsPerSec         = status.m_flInPacketsPerSec;
    outStats.inBytesPerSec           = status.m_flInBytesPerSec;
    outStats.sendRateBytesPerSec     = status.m_nSendRateBytesPerSecond;
    outStats.pendingUnreliableBytes  = status.m_cbPendingUnreliable;
    outStats.pendingReliableBytes    = status.m_cbPendingReliable;
    outStats.sentUnackedReliable     = status.m_cbSentUnackedReliable;
    return true;
}

bool NetTransport::IsListening() const { return _impl->listenSocket != k_HSteamListenSocket_Invalid; }

std::size_t NetTransport::ConnectionCount() const { return _impl->byId.size(); }

std::string_view NetTransport::LastError() const { return _impl->lastError; }

bool NetTransport::SetSimulatedConditions(const SimulatedConditions &conditions)
{
    const std::lock_guard<std::mutex> lock(g_libraryMutex);
    if (g_libraryRefs == 0)
        return false;

    ISteamNetworkingUtils *utils = SteamNetworkingUtils();
    bool                   ok    = true;
    ok &= utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, conditions.sendLossPercent);
    ok &= utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Recv, conditions.recvLossPercent);
    ok &= utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Send, conditions.sendLagMs);
    ok &= utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Recv, conditions.recvLagMs);
    ok &= utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketJitter_Send_Max,
                                           conditions.sendJitterMs);
    ok &= utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketJitter_Recv_Max,
                                           conditions.recvJitterMs);
    return ok;
}

void NetTransport::SetDebugLevel(DebugLevel level)
{
    const std::lock_guard<std::mutex> lock(g_libraryMutex);
    if (g_libraryRefs == 0)
        return;

    ESteamNetworkingSocketsDebugOutputType type = k_ESteamNetworkingSocketsDebugOutputType_None;
    switch (level)
    {
    case DebugLevel::None:       type = k_ESteamNetworkingSocketsDebugOutputType_None; break;
    case DebugLevel::Warning:    type = k_ESteamNetworkingSocketsDebugOutputType_Warning; break;
    case DebugLevel::Message:    type = k_ESteamNetworkingSocketsDebugOutputType_Msg; break;
    case DebugLevel::Verbose:    type = k_ESteamNetworkingSocketsDebugOutputType_Verbose; break;
    case DebugLevel::Everything: type = k_ESteamNetworkingSocketsDebugOutputType_Everything; break;
    }
    SteamNetworkingUtils()->SetDebugOutputFunction(type, type == k_ESteamNetworkingSocketsDebugOutputType_None
                                                             ? nullptr
                                                             : &DebugOutputCallback);
}

} // namespace Assisi::Net
