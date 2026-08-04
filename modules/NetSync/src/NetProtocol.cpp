/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/NetProtocol.hpp>

#include <Assisi/Core/ContentHash.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/NetSync/BodyState.hpp>

#include <format>

namespace Assisi::NetSync
{
namespace
{
/// Bits spent on the message type. Five is room for 32 message kinds — far more
/// than this protocol will grow to, and still cheaper than a whole byte on
/// every packet.
constexpr std::uint32_t kMessageTypeBits = 5;

/// A protocol summary is a diagnostic string, not a payload. Bounding it keeps
/// a hostile handshake from asking us to allocate megabytes before we have
/// decided to trust the peer at all.
constexpr std::size_t kMaxSummaryBytes = 512;

/// Same reasoning for the handshake's level path: it is read before the peer has
/// been trusted, and no legitimate path is anywhere near this long.
constexpr std::size_t kMaxLevelPathBytes = 512;
} // namespace

namespace
{

/// The quantization parameters, as the text the handshake hash is taken over.
///
/// Readable on purpose, and the same reason `ProtocolLayoutDescription` is: when
/// two builds refuse to pair, diffing two descriptions names the field, where a
/// 64-bit mismatch never could.
std::string QuantizationDescription()
{
    const BodyQuantization &q = Quantization();
    return std::format("body positionExtent={:g} positionBits={} linearMax={:g} linearBits={} "
                       "angularMax={:g} angularBits={}",
                       static_cast<double>(q.positionExtent), q.positionBits,
                       static_cast<double>(q.linearVelocityMax), q.linearVelocityBits,
                       static_cast<double>(q.angularVelocityMax), q.angularVelocityBits);
}

} // namespace

std::uint64_t NetProtocolHash()
{
    // FNV-1a continued over the framing version and the quantization, so a change
    // the component table cannot see still refuses to pair. Continuing the same
    // hash rather than mixing with XOR keeps one avalanche, and reuses the
    // primitive Core already ships instead of introducing a second one.
    //
    // The quantization has to be in here: two builds packing the same position
    // over different ranges corrupt each other *silently*, decoding perfectly
    // well into the wrong numbers, which is the one failure mode a handshake
    // exists to prevent.
    std::uint64_t hash = Core::Reflect::ProtocolHash();
    for (std::uint32_t shift = 0; shift < 32; shift += 8)
    {
        hash ^= (kNetProtocolVersion >> shift) & 0xFFu;
        hash *= Core::kFnvPrime;
    }

    const std::string quantization = QuantizationDescription();
    for (const char byte : quantization)
    {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(byte));
        hash *= Core::kFnvPrime;
    }
    return hash;
}

std::string NetProtocolSummary()
{
    // The quantization goes in the summary, not just the hash: a refused
    // connection should say which field the two builds disagree about, and this
    // string is what a rejection carries.
    return std::format("{} net={} {} hash={}", Core::Reflect::ProtocolSummary(), kNetProtocolVersion,
                       QuantizationDescription(), Core::ToHex64(NetProtocolHash()));
}

void WriteMessageType(MessageType type, Core::BitWriter &writer)
{
    writer.WriteBits(static_cast<std::uint32_t>(type), kMessageTypeBits);
}

MessageType ReadMessageType(Core::BitReader &reader)
{
    const std::uint32_t raw = reader.ReadBits(kMessageTypeBits);
    if (!reader.Ok() || raw == 0 || raw >= static_cast<std::uint32_t>(MessageType::Count))
    {
        // An unknown type is not something to skip past: the rest of the packet
        // is unparseable by definition, so latch the failure.
        reader.Invalidate();
        return MessageType::Count;
    }
    return static_cast<MessageType>(raw);
}

void WriteServerHello(const ServerHello &hello, Core::BitWriter &writer)
{
    writer.WriteUInt64(hello.protocolHash);
    writer.WriteString(hello.protocolSummary);
    writer.WriteVarUInt32(hello.tickRateHz);
    writer.WriteVarUInt32(hello.snapshotHz);
    writer.WriteVarUInt64(hello.serverTick);
    writer.WriteVarUInt32(hello.clientId.value);
    writer.WriteBits(static_cast<std::uint32_t>(hello.level.addressing), 8);
    writer.WriteString(hello.level.path);
    writer.WriteUInt64(hello.level.contentHash);
}

bool ReadServerHello(Core::BitReader &reader, ServerHello &outHello)
{
    ServerHello hello;
    hello.protocolHash    = reader.ReadUInt64();
    hello.protocolSummary = reader.ReadString(kMaxSummaryBytes);
    hello.tickRateHz      = reader.ReadVarUInt32();
    hello.snapshotHz      = reader.ReadVarUInt32();
    hello.serverTick      = reader.ReadVarUInt64();
    hello.clientId        = ClientId{reader.ReadVarUInt32()};

    const std::uint32_t addressing = reader.ReadBits(8);
    hello.level.path               = reader.ReadString(kMaxLevelPathBytes);
    hello.level.contentHash        = reader.ReadUInt64();

    // A zero rate would make the client's clock arithmetic divide by zero, and
    // a snapshot rate above the tick rate is nonsense. Reject rather than
    // sanitize: this is the message that establishes whether we trust the peer.
    // An addressing mode we do not know is the same call: guessing how to read
    // the path is exactly the sniffing the tag exists to avoid.
    //
    // A clientId below kFirstRemoteClientId is the same class of answer: 0 is
    // "nobody" and 1 is the host, so a joiner told it is either one has been
    // handed an identity it cannot act on — it would compare its own id against
    // every ControlledBy and match the wrong entities, or none.
    if (!reader.Ok() || hello.tickRateHz == 0 || hello.snapshotHz == 0 || hello.snapshotHz > hello.tickRateHz ||
        hello.clientId.value < kFirstRemoteClientId ||
        addressing > static_cast<std::uint32_t>(LevelAddressing::AbsolutePath))
    {
        reader.Invalidate();
        return false;
    }
    hello.level.addressing = static_cast<LevelAddressing>(addressing);

    outHello = std::move(hello);
    return true;
}

void WriteClientHello(const ClientHello &hello, Core::BitWriter &writer)
{
    writer.WriteUInt64(hello.protocolHash);
}

bool ReadClientHello(Core::BitReader &reader, ClientHello &outHello)
{
    ClientHello hello;
    hello.protocolHash = reader.ReadUInt64();
    if (!reader.Ok())
        return false;

    outHello = hello;
    return true;
}

void WriteSnapshotHeader(const SnapshotHeader &header, Core::BitWriter &writer)
{
    writer.WriteVarUInt64(header.serverTick);
    writer.WriteVarUInt64(header.baselineTick);
    writer.WriteVarUInt32(header.inputBufferDepth);
    writer.WriteVarUInt32(header.starvedTicks);
    writer.WriteBool(header.worldComplete);
}

bool ReadSnapshotHeader(Core::BitReader &reader, SnapshotHeader &outHeader)
{
    SnapshotHeader header;
    header.serverTick       = reader.ReadVarUInt64();
    header.baselineTick     = reader.ReadVarUInt64();
    header.inputBufferDepth = reader.ReadVarUInt32();
    header.starvedTicks     = reader.ReadVarUInt32();
    header.worldComplete    = reader.ReadBool();

    // A baseline in the future describes a snapshot we cannot have sent.
    if (!reader.Ok() || header.baselineTick > header.serverTick)
    {
        reader.Invalidate();
        return false;
    }

    outHeader = header;
    return true;
}

} // namespace Assisi::NetSync
