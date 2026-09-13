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
#include <format>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace Assisi::NetSync
{

/// @brief The integer a NetId is made of, and the one place its width is
/// decided.
///
/// Everything that reads, writes, allocates or bounds a NetId is keyed to this
/// name rather than spelling `std::uint32_t` again — `BitReader::ReadVarId`
/// takes the wire width from it, `NetIdRangeFits` takes the end of the id space
/// from it, and `ReplicationServer::_nextNetId` is declared with it. Widening
/// the id space is then this line plus a `kNetProtocolVersion` bump, not a sweep
/// over forty sites where a missed one truncates in silence.
///
/// One thing does **not** follow, deliberately, and the static_assert below is
/// what says so out loud: see `CodecContext::instanceToWire`.
using NetIdValue = std::uint32_t;

/// @brief Server-assigned, session-scoped identity for a replicated entity.
///
/// Local `ECS::Entity` handles are (index, generation) pairs whose values
/// depend on each machine's own allocation history, so they are meaningless
/// across a connection. NetId is the only entity identity on the wire.
///
/// Never recycled within a session: reuse would let a stale reference silently
/// address a different entity, so the counter only climbs and the space is
/// spent rather than reclaimed. At `NetIdValue`'s current width that budget is
/// total spawns per session, not live entities — a distinction that matters for
/// a server meant to stay up indefinitely, and the reason the width is a typedef
/// rather than a literal.
///
/// An aggregate, matching `ClientId` below — aggregate initialization
/// (`NetId{7}`) is the only way in, which is what blocks the implicit conversion
/// in both directions.
///
/// Deliberately no arithmetic. Allocating from a counter, and deriving a member
/// block's ids as `base + index`, are both real operations — but they belong at
/// the two or three sites that do them, spelled out, rather than baked into the
/// type where every other use inherits them for free. `base + index` is only
/// *sometimes* a member's NetId, and that is a seam worth having to look at.
struct NetId
{
    NetIdValue value = 0;

    [[nodiscard]] constexpr bool IsValid() const { return value != 0; }

    friend constexpr bool operator==(NetId, NetId)  = default;
    friend constexpr auto operator<=>(NetId, NetId) = default;
};

/// @brief The never-valid NetId. Zero, so a value-initialized NetId is invalid.
inline constexpr NetId InvalidNetId{0};

// The one place a NetId's width is committed to outside this header, and the
// reason it cannot simply follow the typedef: `CodecContext::instanceToWire`
// hands a base NetId through a `std::uint32_t` channel, and the codec applies it
// to `AFIELD(instanceRef)` fields, which are `FieldType::UInt32` in the
// reflected schema. Widening that channel's signature alone would move the
// truncation *inside* the codec, where it would be silent — the exact shape of
// B10.
//
// So widening NetIdValue is gated on a real decision: widen the instanceRef
// field type through the schema and the generator, or keep instance references
// 32-bit and accept that a base above the ceiling cannot be named in a
// component field. This fires at the line being edited rather than letting
// either choice be made by accident.
static_assert(sizeof(NetIdValue) <= sizeof(std::uint32_t),
              "NetIdValue no longer fits the instanceRef codec channel: CodecContext::instanceToWire and "
              "AFIELD(instanceRef) are 32-bit by declaration. Widen the instanceRef field type through "
              "Reflect and reflectgen, or keep instance references 32-bit — but decide it here.");

/// @brief Does a block of @p count ids starting at @p base fit the id space?
///
/// The bound behind every `base + i` derived on this wire. Written as a
/// subtraction rather than a widened sum on purpose: a sum can only be checked
/// by computing it in something wider, which stops working the moment NetIdValue
/// is as wide as the arithmetic. This form holds at every width, which is the
/// whole point of the typedef above.
[[nodiscard]] constexpr bool NetIdRangeFits(NetId base, std::uint64_t count)
{
    if (count == 0)
        return true; // an empty range fits anywhere, including past the end
    return count - 1u <= static_cast<std::uint64_t>(std::numeric_limits<NetIdValue>::max()) - base.value;
}

/// @brief Session-scoped identity for a *participant*: the thing `ControlledBy`
/// names, directed messages address, and logs blame.
///
/// Distinct from `Net::ConnectionId` on purpose, and a wrapper type rather than
/// an alias so the compiler enforces it. A ConnectionId is a transport handle —
/// meaningful only inside the server process, meaningless on any other machine
/// — and the two id spaces are one honest mistake apart. Replicating a
/// ConnectionId would be replicating a pointer.
///
/// Conventions, matching the uniform zero-means-invalid rule the rest of the
/// module already follows (`InvalidNetId`, `Net::InvalidConnection`):
///
///  - **0 = nobody.** A default-constructed id claims nothing, which is what
///    makes a default-constructed `ControlledBy` harmless.
///  - **1 = the host itself** — the listen server's own player, which is not a
///    connection and never will be (see NetSession.hpp on why there is no
///    loopback client).
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

/// @brief The listen server's own player. Not a connection — see ClientId.
inline constexpr ClientId HostClientId{1};

/// @brief The first id handed to a remote client. Everything below it is
/// reserved, which is what keeps `0` and the host out of the assignable range.
inline constexpr std::uint32_t kFirstRemoteClientId = 2;

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
    /// Client → server: "re-anchor me from nothing". Zeroes that connection's
    /// per-entity baselines and clears its in-flight ring — the same path the
    /// periodic keyframe sweep takes, on demand.
    ///
    /// Carries no payload: the only thing it can say is *that* the client wants
    /// a full re-anchor, and a client asking for one it does not need costs one
    /// over-full snapshot. It exists because "something is visibly wrong on my
    /// screen" is a state a human can see and the protocol cannot, and waiting
    /// out an 8.5-second sweep to find out whether it heals is a bad debugging
    /// loop.
    RequestKeyframe = 7,
    /// Client → server: one `AMSG(intent, …)`, tick-stamped.
    ///
    /// The only thing a client may say beyond its input window and its acks,
    /// and it arrives at exactly one place. That is the design rather than an
    /// implementation detail: every documented exploit in the RPC survey was
    /// attacker-shaped messages meeting hand-written parsing spread across many
    /// receive sites, and one validated door is the only structural answer.
    Intent = 8,
    /// Server → client: one `AMSG(event, reliable)`, tick-stamped.
    ///
    /// Rare by design. Both Quake 3 and Unreal hit the same reliable-buffer
    /// cliff — Unreal's overflow *closes the connection* — and both mitigations
    /// amount to asking every call site to budget against a global. The
    /// unreliable form rides the snapshot instead, and the documented rule is
    /// that a type sending at snapshot cadence belongs there or is state wearing
    /// an event costume.
    Announcement = 9,

    Count
};

/// @brief Why a connection was refused.
enum class RejectReason : std::uint8_t
{
    ProtocolMismatch = 1, ///< The two builds do not agree on component layout.
    ServerFull       = 2,
    /// The two machines do not hold the same set of level and blueprint files.
    ///
    /// Deliberately strict: *any* difference refuses, including files neither
    /// machine ever loads, because a stray experimental `.abp` is indistinguishable
    /// from a car whose wheels moved. That is a development-time cost and never a
    /// shipping one, and it buys the property blueprint replication depends on —
    /// after a successful join, both machines are known to expand any blueprint
    /// identically.
    ContentMismatch = 3,
};

/// @brief Framing version for the messages in this file.
///
/// The component table is hashed by `Core::Reflect::ProtocolHash`, and covers
/// everything the codec carries — but not the shape of the messages *around* the
/// component blocks. A field added to `ServerHello`, a new section in a snapshot,
/// a different varint form: all invisible to the component table and all fatal to
/// a mismatched pair. Bump this when any Write*/Read* pair here changes.
///
/// This constant is the *only* route by which a framing change reaches
/// NetProtocolHash() (see NetProtocol.cpp) — which is why "bump it" is a rule
/// rather than a courtesy. A framing change without a bump produces two
/// hash-equal builds that pair happily and then misparse each other, silently:
/// exactly the failure the handshake exists to prevent.
///
///  - 3 → 4: `ServerHello.clientId`.
///  - 4 → 5: `MessageType::Intent` and its envelope.
///  - 5 → 6: the snapshot's message section, and `MessageType::Announcement`.
///  - 6 → 7: `ClientHello.contentSetHash` and `RejectReason::ContentMismatch`.
///  - 7 → 8: the snapshot gained an instance-record section, ahead of despawns.
///    A v7 client reads that count as its despawn count and desyncs the whole
///    stream, so this is a refuse-to-join change, not a tolerable one.
///  - 8 → 9: an instance record carries **which of its members exist** — one bit
///    for "all of them", and one bit per member when they do not. Nothing on
///    the wire said so
///    before, so a member pruned on the host was expanded and bound by every
///    later joiner and no despawn ever named it. Same section, one bit wider in
///    the common case, and a v8 client would read the presence bit as the first
///    byte of the placement.
inline constexpr std::uint32_t kNetProtocolVersion = 9;

/// @brief The hash exchanged at handshake: the reflection protocol hash with
/// this module's framing version folded in.
///
/// Use this rather than `Core::Reflect::ProtocolHash()` directly anywhere a
/// handshake is written or checked; the reflection hash alone would let two
/// builds with identical components but different message framing pair up and
/// then misparse each other silently.
[[nodiscard]] std::uint64_t NetProtocolHash();

/// @brief `Core::Reflect::ProtocolSummary()` plus the framing version — the
/// human-readable companion to NetProtocolHash.
[[nodiscard]] std::string NetProtocolSummary();

/// @brief How a `ServerHello`'s level path should be read.
enum class LevelAddressing : std::uint8_t
{
    /// The host is running no level file at all. A joining editor treats this as
    /// a load failure and aborts cleanly rather than guessing.
    None = 0,
    /// A virtual path under the asset root, e.g. "levels/Materials.alvl". Both
    /// machines resolve it through their own asset system.
    Virtual = 1,
    /// An absolute filesystem path. Play-in-editor only, where host and client
    /// are processes on one machine and the level is a temp snapshot the editor
    /// wrote — which is how PIE hosts unsaved edits without a transfer protocol.
    AbsolutePath = 2,
};

/// @brief What level the host is running, and which bytes it was built from.
///
/// Tagged, never sniffed: "does this look absolute?" is a guess that differs by
/// platform, and the failure it produces (a client loading the wrong file, or
/// none) surfaces minutes later as physics against invisible geometry.
struct LevelIdentity
{
    LevelAddressing addressing = LevelAddressing::None;
    std::string path;     ///< Interpreted per `addressing`; empty when None.
    /// `Core::ContentHash64` of the level file as saved. A client that resolves
    /// the path to different bytes refuses the join: everything downstream —
    /// static geometry, unmarked dynamics, the entities that get stripped —
    /// assumes both sides loaded the same file.
    std::uint64_t contentHash = 0;
};

/// @brief Server → client handshake.
struct ServerHello
{
    /// Hash of the component layout, codec version, and message framing. Two
    /// builds that disagree here cannot exchange component data safely, and the
    /// failure would be silent corruption rather than an error — so the
    /// connection is refused. See NetProtocolHash.
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
    /// Who this client *is* for the rest of the session: the id `ControlledBy`
    /// names, directed messages address, and the client compares against to know
    /// which entities are its own. Always ≥ kFirstRemoteClientId — the host
    /// never handshakes with itself.
    ClientId clientId;
    /// Which level the client must load before it answers. The client builds its
    /// world from this and *then* sends its ClientHello — snapshots that arrived
    /// against a world it had not built yet would map NetIds onto whatever
    /// entities happened to occupy those slots.
    LevelIdentity level;
};

/// @brief Client → server handshake.
struct ClientHello
{
    std::uint64_t protocolHash = 0;

    /// One hash over **every** `.alvl` and `.abp` this build can resolve, each
    /// content-normalised, combined in sorted virtual-path order.
    ///
    /// The level's own `contentHash` names *which* level; this names the whole
    /// content set, and it has to, because a blueprint spawned from C++ is named
    /// by no level and would never be hashed — while blueprint replication makes
    /// every blueprint's content load-bearing across the wire. Once the sets are
    /// known equal, a blueprint can be named on the wire by its index in the
    /// sorted list: two bytes, no path string, no per-file hash.
    ///
    /// **The client sends it; the server decides.** The server owns who joins,
    /// which is where a content check belongs, and it matches how the protocol
    /// hash already works.
    std::uint64_t contentSetHash = 0;
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

// A NetId on the wire is `writer.WriteVarId(id)` / `reader.ReadVarId<NetId>()`,
// written out at each site like every other scalar id here. Both take their
// width from NetIdValue's declaration, including the read-side range check — so
// a value too wide for the id is refused rather than truncated into one naming a
// different entity, which is B10's failure arrived at by another route.

} // namespace Assisi::NetSync

namespace Assisi::Core
{
/// Encodes as a varint, at whatever width NetIdValue is — which is the whole
/// reason this opt-in is worth having: `ReadVarId<NetId>` takes its range check
/// from the declaration, so the width is stated once and enforced everywhere.
template <> struct IsStrongId<NetSync::NetId> : std::true_type
{
};
static_assert(StrongId<NetSync::NetId>);

/// Reaches a peer inside a component (`ControlledBy`) and, as a bare field, in
/// `ServerHello.clientId` — which encodes it as a plain `WriteVarUInt32` of the
/// value rather than through `WriteVarId`. Declaring the opt-in here is what
/// keeps that a decision rather than an oversight.
template <> struct IsStrongId<NetSync::ClientId> : std::true_type
{
};
static_assert(StrongId<NetSync::ClientId>);
} // namespace Assisi::Core

/// Prints as the bare number, so a log line reads "netId 7" rather than making
/// every call site spell `.value`. Without this the type would be strictly worse
/// to hold than the integer it replaces, which is how a good rule gets worked
/// around.
template <> struct std::formatter<Assisi::NetSync::NetId> : std::formatter<Assisi::NetSync::NetIdValue>
{
    auto format(Assisi::NetSync::NetId id, std::format_context &ctx) const
    {
        return std::formatter<Assisi::NetSync::NetIdValue>::format(id.value, ctx);
    }
};

template <> struct std::hash<Assisi::NetSync::NetId>
{
    [[nodiscard]] std::size_t operator()(Assisi::NetSync::NetId id) const noexcept
    {
        return std::hash<Assisi::NetSync::NetIdValue>{}(id.value);
    }
};
