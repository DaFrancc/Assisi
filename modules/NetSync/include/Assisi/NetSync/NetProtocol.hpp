/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file NetProtocol.hpp
/// @brief Message identity, the handshake, and the snapshot wire format.
///
/// Every message on the wire starts with a MessageType. Beyond that the layout
/// is defined by the Write*/Read* pairs here — each pair is written adjacently
/// and must be edited as a unit.
///
/// The snapshot format follows the Quake 3 unification: there is no separate
/// "full state" message. A full state is a delta against the *empty* baseline,
/// so spawn, delta, keyframe, and late-join are all one code path with one set
/// of bugs instead of four.

#include <Assisi/Core/BitStream.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Assisi::NetSync
{

/// @brief Server-assigned, session-scoped identity for a replicated entity.
///
/// Local `ECS::Entity` handles are (index, generation) pairs whose values
/// depend on each machine's own allocation history, so they are meaningless
/// across a connection. NetId is the only entity identity on the wire.
///
/// Never recycled within a session: at this scale 32 bits will not run out, and
/// reuse would let a stale reference silently address a different entity.
using NetId = std::uint32_t;

/// @brief The never-valid NetId. Zero, so a value-initialized NetId is invalid.
inline constexpr NetId InvalidNetId = 0;

/// @brief What a message is. First field of every packet.
enum class MessageType : std::uint8_t
{
    /// Server → client, on connect: protocol hash and timing. The client
    /// verifies the hash before anything else is exchanged.
    ServerHello = 1,
    /// Client → server: the client's own protocol hash.
    ClientHello = 2,
    /// Server → client: the connection is rejected, with a reason a human can
    /// act on. Sent instead of a silent disconnect so a protocol mismatch does
    /// not look like a network fault.
    Reject = 3,
    /// Server → client, every net tick: world state as a delta.
    Snapshot = 4,
    /// Client → server: the last snapshot it applied. Drives the delta baseline.
    Ack = 5,
    /// Client → server, every tick: the redundant input command window.
    Input = 6,

    Count
};

/// @brief Why a connection was refused.
enum class RejectReason : std::uint8_t
{
    ProtocolMismatch = 1, ///< The two builds do not agree on component layout.
    ServerFull       = 2,
};

/// @brief Server → client handshake.
struct ServerHello
{
    /// Hash of the component layout and codec version. Two builds that disagree
    /// here cannot exchange component data safely, and the failure would be
    /// silent corruption rather than an error — so the connection is refused.
    std::uint64_t protocolHash = 0;
    /// Human-readable companion to the hash, so a rejection says *what* differs
    /// instead of just that something does.
    std::string protocolSummary;
    /// The server's fixed-step rate. The client's clock is derived from it.
    std::uint32_t tickRateHz = 60;
    /// How often snapshots are sent. Always a divisor of tickRateHz.
    std::uint32_t snapshotHz = 20;
    /// The tick the server is on right now, so the client can start its clock.
    std::uint64_t serverTick = 0;
};

/// @brief Client → server handshake.
struct ClientHello
{
    std::uint64_t protocolHash = 0;
};

/// @brief The fixed part of a snapshot, before the entity data.
struct SnapshotHeader
{
    /// The tick this snapshot describes.
    std::uint64_t serverTick = 0;
    /// The tick this delta is against — the client's last acked snapshot. Zero
    /// means the empty baseline, i.e. full state.
    std::uint64_t baselineTick = 0;
    /// How many of this client's input commands the server had buffered.
    std::uint32_t inputBufferDepth = 0;
    /// How many recent ticks found that buffer empty. Together with the depth,
    /// this is everything NetClock needs to steer the client's lead.
    std::uint32_t starvedTicks = 0;

    /// True once the client has acknowledged every entity the server currently
    /// has — i.e. its initial download is complete and it is watching a live
    /// world rather than still receiving one.
    ///
    /// A joining client's baseline is spread across however many snapshots the
    /// byte budget needs, so "am I done joining?" is not a question it can
    /// answer locally: it cannot tell a small world from the first page of a
    /// large one. Only the server knows.
    bool worldComplete = false;
};

void        WriteMessageType(MessageType type, Core::BitWriter &writer);
MessageType ReadMessageType(Core::BitReader &reader);

void WriteServerHello(const ServerHello &hello, Core::BitWriter &writer);
bool ReadServerHello(Core::BitReader &reader, ServerHello &outHello);

void WriteClientHello(const ClientHello &hello, Core::BitWriter &writer);
bool ReadClientHello(Core::BitReader &reader, ClientHello &outHello);

void WriteSnapshotHeader(const SnapshotHeader &header, Core::BitWriter &writer);
bool ReadSnapshotHeader(Core::BitReader &reader, SnapshotHeader &outHeader);

} // namespace Assisi::NetSync
