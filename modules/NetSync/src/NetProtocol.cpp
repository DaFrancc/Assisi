/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/NetProtocol.hpp>

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
} // namespace

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
}

bool ReadServerHello(Core::BitReader &reader, ServerHello &outHello)
{
    ServerHello hello;
    hello.protocolHash    = reader.ReadUInt64();
    hello.protocolSummary = reader.ReadString(kMaxSummaryBytes);
    hello.tickRateHz      = reader.ReadVarUInt32();
    hello.snapshotHz      = reader.ReadVarUInt32();
    hello.serverTick      = reader.ReadVarUInt64();

    // A zero rate would make the client's clock arithmetic divide by zero, and
    // a snapshot rate above the tick rate is nonsense. Reject rather than
    // sanitize: this is the message that establishes whether we trust the peer.
    if (!reader.Ok() || hello.tickRateHz == 0 || hello.snapshotHz == 0 || hello.snapshotHz > hello.tickRateHz)
    {
        reader.Invalidate();
        return false;
    }

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
