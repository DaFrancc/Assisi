/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file NetTransport.hpp
/// @brief Connection-oriented UDP transport — a dumb pipe over GameNetworkingSockets.
///
/// This module knows nothing about entities, snapshots, or the game: it moves
/// opaque byte payloads between a server and its clients and reports connection
/// lifecycle as events. Replication lives one layer up, in Assisi::NetSync.
///
/// Every third-party type is hidden behind a pimpl, so nothing downstream ever
/// includes a GNS header. That keeps the transport swappable (the design notes
/// name enet6+DTLS as the fallback) and keeps our strict warning set from having
/// to tolerate a vendored header.
///
/// Usage, per frame (client) or per tick (server):
/// @code
///   std::vector<NetEvent> events;
///   transport.Poll(events);
///   for (const NetEvent &e : events) { ... }
///   transport.Send(conn, payload, SendMode::Unreliable, Lane::Snapshot);
/// @endcode

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Assisi::Net
{

/// @brief Dense local handle for a connection.
///
/// Deliberately *not* GNS's HSteamNetConnection: the transport owns its own
/// handle space so the wire library never leaks into a caller's type. Handles
/// are never recycled within a NetTransport's lifetime, so a stale id is always
/// detected rather than silently aliasing a new connection.
///
/// An aggregate, matching `NetSync::ClientId`/`NetSync::NetId` — aggregate
/// initialization (`ConnectionId{7}`) is the only way in, which is what blocks
/// the implicit conversion in both directions. Deliberately no arithmetic: a
/// dense handle is never added to or subtracted from, only allocated, compared,
/// and looked up.
///
/// `NetSync::ClientId` is a wrapper type so the compiler keeps the two apart
/// (see `NetProtocol.hpp`); ConnectionId must be one too, or it converts freely
/// into any uint32_t slot, a ClientId's included.
struct ConnectionId
{
    std::uint32_t value = 0;

    [[nodiscard]] constexpr bool IsValid() const { return value != 0; }

    friend constexpr bool operator==(ConnectionId, ConnectionId)  = default;
    friend constexpr auto operator<=>(ConnectionId, ConnectionId) = default;
};

/// @brief The never-valid connection handle. Zero, so a value-initialized
/// ConnectionId is invalid by construction.
inline constexpr ConnectionId InvalidConnection{0};

/// @brief Delivery guarantee for one message.
enum class SendMode : std::uint8_t
{
    /// Fire and forget. Lost packets are never retransmitted — correct for
    /// state snapshots, where the *next* snapshot supersedes the lost one and a
    /// retransmit would only deliver stale data late.
    Unreliable,
    /// Retransmitted until acknowledged, delivered in order within its lane.
    Reliable,
};

/// @brief Independent delivery streams within one connection.
///
/// Reliable delivery is ordered *within* a lane and independent *between*
/// lanes, which is the whole point: a large reliable transfer on Bulk (a
/// late-joining client's world baseline) must never head-of-line-block the
/// per-tick snapshot stream behind it. Priorities are configured per connection
/// as it is adopted, lowest value = highest priority.
enum class Lane : std::uint8_t
{
    Control  = 0, ///< Connection handshake, spawns/despawns, anything that must arrive.
    Snapshot = 1, ///< Per-tick world state. Unreliable, latency-critical.
    Bulk     = 2, ///< Large transfers that may take many ticks (late-join baseline).
};

/// @brief Number of lanes configured on every connection. Keep in sync with Lane.
inline constexpr std::uint32_t LaneCount = 3;

/// @brief One thing that happened on the wire since the last Poll().
struct NetEvent
{
    enum class Type : std::uint8_t
    {
        Connected,    ///< A connection completed its handshake and can carry messages.
        Disconnected, ///< A connection closed (peer closed, timed out, or was rejected).
        Message,      ///< A payload arrived.
    };

    Type type       = Type::Message;
    ConnectionId connection = InvalidConnection;
    std::vector<std::byte> payload;              ///< Message only; empty otherwise.
    Lane lane = Lane::Control;                   ///< Message only.
    /// Disconnected only: GNS's end reason plus its debug string, for logging a
    /// diagnosable cause rather than "the client went away".
    std::int32_t closeReason = 0;
    std::string closeDebug;
};

/// @brief A connection's current health, for debug overlays and the adaptive
/// clock work in later networking stages.
struct ConnectionStats
{
    std::int32_t pingMs                  = 0;
    float connectionQualityLocal  = 0.f;        ///< 0..1, fraction delivered end-to-end in order.
    float connectionQualityRemote = 0.f;        ///< The same, as the peer observes it.
    float outPacketsPerSec        = 0.f;
    float outBytesPerSec          = 0.f;
    float inPacketsPerSec         = 0.f;
    float inBytesPerSec           = 0.f;
    std::int32_t sendRateBytesPerSec     = 0; ///< Estimated channel capacity, not current usage.
    std::int32_t pendingUnreliableBytes  = 0;
    std::int32_t pendingReliableBytes    = 0;
    std::int32_t sentUnackedReliable     = 0;
};

/// @brief Artificial network impairment, applied process-wide.
///
/// GNS applies these inside its own send/receive path, so a test can reproduce
/// a bad connection without a real network, a second machine, or root. Note the
/// scope: these are *global* config values in GNS, not per-connection, and they
/// do not apply to a socket pair created in its default in-process mode — see
/// NetTransport::CreateLoopbackPair.
struct SimulatedConditions
{
    float sendLossPercent = 0.f;        ///< 0..100, packets dropped on send.
    float recvLossPercent = 0.f;        ///< 0..100, packets dropped on receive.
    std::int32_t sendLagMs       = 0;   ///< Added one-way delay on send.
    std::int32_t recvLagMs       = 0;   ///< Added one-way delay on receive.
    std::int32_t sendJitterMs    = 0;   ///< Max extra random delay on send.
    std::int32_t recvJitterMs    = 0;   ///< Max extra random delay on receive.
};

/// @brief One transport endpoint: a server's listen socket, a client's outbound
/// connection, or both halves of an in-process loopback pair.
///
/// **Threading: none of its own.** GNS runs an internal service thread for the
/// wire; our contract is that every method here is called from one thread — the
/// main thread for a client, the tick thread for a server — with Poll() pumping
/// callbacks and draining received messages. Nothing in this class touches the
/// JobSystem. If Poll() ever shows up in a profile, the seam to move is that
/// single call site, not this interface.
class NetTransport
{
public:
    /// Initializes the GNS library on first construction (refcounted across
    /// instances, mirroring how PhysicsWorld owns Jolt's globals).
    NetTransport();
    ~NetTransport();

    NetTransport(const NetTransport &)            = delete;
    NetTransport &operator=(const NetTransport &) = delete;
    NetTransport(NetTransport &&)                 = delete;
    NetTransport &operator=(NetTransport &&)      = delete;

    /// @brief Bind a listen socket on @p port (all interfaces, IPv6 with IPv4
    /// mapping). Incoming connections are accepted automatically and surface as
    /// NetEvent::Type::Connected once their handshake completes.
    /// @return false if the port could not be bound; call LastError() for why.
    bool Listen(std::uint16_t port);

    /// @brief Connect to @p address (an IPv4/IPv6 literal, no DNS) on @p port.
    /// @return the new connection's handle, or InvalidConnection on immediate
    /// failure. A returned handle is *connecting*, not yet connected — wait for
    /// the Connected event before sending.
    [[nodiscard]] ConnectionId Connect(std::string_view address, std::uint16_t port);

    /// @brief Create two connections wired to each other inside this process.
    ///
    /// This is what makes a listen server one codepath instead of two: the
    /// embedded server holds one end, the local client the other, and neither
    /// knows it is not talking over UDP.
    ///
    /// @param useNetworkLoopback Selects between the two modes GNS offers, which
    ///   differ in more than performance:
    ///   - false (default) — the pair shares internal buffers. No encryption, no
    ///     packet layer, and therefore **no simulated lag/loss**: SetSimulatedConditions
    ///     has no effect on it. This is the listen-server path.
    ///   - true — the pair is routed through 127.0.0.1 as real (encrypted) UDP,
    ///     so simulated conditions *do* apply. This is the test harness path.
    /// @return both ends, or {InvalidConnection, InvalidConnection} on failure.
    [[nodiscard]] std::pair<ConnectionId, ConnectionId> CreateLoopbackPair(bool useNetworkLoopback = false);

    /// @brief Close @p connection. Emits no Disconnected event for the local
    /// side — the caller already knows. Unknown handles are ignored.
    /// @param linger If true, queued reliable data is given a chance to flush
    ///   before the connection actually goes away.
    void Close(ConnectionId connection, bool linger = true);

    /// @brief Queue @p data for delivery on @p lane with @p mode.
    /// @return false if the handle is unknown/closed or GNS rejected the send.
    /// A true return means "accepted for delivery", not "delivered".
    bool Send(ConnectionId connection, std::span<const std::byte> data, SendMode mode, Lane lane);

    /// @brief Pump the library and drain everything that arrived since the last
    /// call into @p outEvents (which is cleared first).
    ///
    /// Call exactly once per frame (client) or tick (server). Connection state
    /// changes are reported in the order GNS delivered them, interleaved with
    /// messages.
    void Poll(std::vector<NetEvent> &outEvents);

    /// @brief Fill @p outStats for @p connection.
    /// @return false if the handle is unknown or GNS has no status for it.
    bool GetConnectionStats(ConnectionId connection, ConnectionStats &outStats) const;

    /// @brief True while a listen socket is bound.
    [[nodiscard]] bool IsListening() const;

    /// @brief Number of live connections (connecting or connected).
    [[nodiscard]] std::size_t ConnectionCount() const;

    /// @brief Human-readable reason the last failed call failed. Empty if none.
    [[nodiscard]] std::string_view LastError() const;

    /// @brief Apply artificial impairment to every socket in the process.
    ///
    /// Static because GNS scopes these globally, not per-connection — writing it
    /// as an instance method would imply an isolation the library does not
    /// provide. Requires at least one live NetTransport (the library must be
    /// initialized); returns false otherwise.
    static bool SetSimulatedConditions(const SimulatedConditions &conditions);

    /// @brief Route GNS's internal diagnostics into the engine log.
    ///
    /// Off by default: at anything above Warning, GNS is extremely chatty, and
    /// the messages arrive on its service thread. Enable it while debugging a
    /// connection problem, not in a normal run.
    enum class DebugLevel : std::uint8_t
    {
        None,
        Warning,
        Message,
        Verbose,
        Everything,
    };
    static void SetDebugLevel(DebugLevel level);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace Assisi::Net

/// Prints as the bare number, so a log line reads "connection 7" without every
/// call site spelling `.value`.
template <> struct std::formatter<Assisi::Net::ConnectionId> : std::formatter<std::uint32_t>
{
    auto format(Assisi::Net::ConnectionId id, std::format_context &ctx) const
    {
        return std::formatter<std::uint32_t>::format(id.value, ctx);
    }
};

template <> struct std::hash<Assisi::Net::ConnectionId>
{
    [[nodiscard]] std::size_t operator()(Assisi::Net::ConnectionId id) const noexcept
    {
        return std::hash<std::uint32_t>{}(id.value);
    }
};
