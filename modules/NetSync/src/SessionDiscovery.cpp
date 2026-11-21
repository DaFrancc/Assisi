/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/SessionDiscovery.hpp>

#include <Assisi/Core/BitStream.hpp>

#include <array>
#include <charconv>
#include <cstring>

namespace Assisi::NetSync
{
namespace
{

/// The first bytes of every datagram, so anything else that lands on the port,
/// another program's broadcast included, is ignored before a field is read.
constexpr std::size_t kMagicBytes = 8;
constexpr std::array<char, kMagicBytes> kQueryMagic{'A', 'S', 'S', 'I', 'S', 'I', '-', 'Q'};
constexpr std::array<char, kMagicBytes> kReplyMagic{'A', 'S', 'S', 'I', 'S', 'I', '-', 'R'};

void WriteHeader(const std::array<char, kMagicBytes> &magic, Core::BitWriter &writer)
{
    writer.WriteBytes(std::as_bytes(std::span(magic)));
    writer.WriteUInt8(kDiscoveryVersion);
}

/// Reads the magic and version; false, with the reader invalidated, when either
/// is not this build's.
bool ReadHeader(const std::array<char, kMagicBytes> &magic, Core::BitReader &reader)
{
    std::array<std::byte, kMagicBytes> read{};
    reader.ReadBytes(read);
    const std::uint8_t version = reader.ReadUInt8();
    if (!reader.Ok() || std::memcmp(read.data(), magic.data(), kMagicBytes) != 0 || version != kDiscoveryVersion)
    {
        reader.Invalidate();
        return false;
    }
    return true;
}

bool FieldsFit(const DiscoveryReply &reply)
{
    if (reply.name.size() > kMaxSessionNameBytes || reply.levelPath.size() > kMaxDiscoveryLevelBytes ||
        reply.gameData.size() > kMaxGameDataPairs)
    {
        return false;
    }
    for (const auto &[key, value] : reply.gameData)
    {
        if (key.size() > kMaxGameDataKeyBytes || value.size() > kMaxGameDataValueBytes)
        {
            return false;
        }
    }
    return true;
}

/// One dotted IPv4 literal as its four parts, or nothing.
std::optional<std::array<std::uint8_t, 4>> ParseIpv4(std::string_view text)
{
    constexpr std::uint32_t kMaxOctet = 255;
    std::array<std::uint8_t, 4> parts{};
    std::size_t part = 0;
    const char *at = text.data();
    const char *end = text.data() + text.size();
    while (part < parts.size())
    {
        std::uint32_t value = 0;
        const std::from_chars_result parsed = std::from_chars(at, end, value);
        if (parsed.ec != std::errc{} || parsed.ptr == at || value > kMaxOctet)
        {
            return std::nullopt;
        }
        parts[part++] = static_cast<std::uint8_t>(value);
        at = parsed.ptr;
        if (part < parts.size())
        {
            if (at == end || *at != '.')
            {
                return std::nullopt;
            }
            ++at;
        }
    }
    if (at != end)
    {
        return std::nullopt;
    }
    return parts;
}

bool IsLocalIpv4(const std::array<std::uint8_t, 4> &ip)
{
    constexpr std::uint8_t kTen = 10;
    constexpr std::uint8_t kLoopback = 127;
    constexpr std::uint8_t kPrivate172 = 172;
    constexpr std::uint8_t kPrivate172Mask = 0xF0; // 172.16/12: the second part's top four bits
    constexpr std::uint8_t kPrivate172Second = 16;
    constexpr std::uint8_t kPrivate192 = 192;
    constexpr std::uint8_t kPrivate168 = 168;
    constexpr std::uint8_t kLinkLocal169 = 169;
    constexpr std::uint8_t kLinkLocal254 = 254;
    return ip[0] == kTen || ip[0] == kLoopback ||
           (ip[0] == kPrivate172 && (ip[1] & kPrivate172Mask) == kPrivate172Second) ||
           (ip[0] == kPrivate192 && ip[1] == kPrivate168) || (ip[0] == kLinkLocal169 && ip[1] == kLinkLocal254);
}

} // namespace

std::vector<std::byte> WriteDiscoveryQuery(const DiscoveryQuery &query)
{
    Core::BitWriter writer;
    WriteHeader(kQueryMagic, writer);
    writer.WriteUInt32(query.nonce);
    return {writer.Data().begin(), writer.Data().end()};
}

std::optional<DiscoveryQuery> ReadDiscoveryQuery(std::span<const std::byte> datagram)
{
    Core::BitReader reader(datagram);
    if (!ReadHeader(kQueryMagic, reader))
    {
        return std::nullopt;
    }
    DiscoveryQuery query;
    query.nonce = reader.ReadUInt32();
    if (!reader.Ok())
    {
        return std::nullopt;
    }
    return query;
}

std::expected<std::vector<std::byte>, DiscoveryError> WriteDiscoveryReply(const DiscoveryReply &reply)
{
    if (!FieldsFit(reply))
    {
        return std::unexpected(DiscoveryError::TooLong);
    }
    Core::BitWriter writer;
    WriteHeader(kReplyMagic, writer);
    writer.WriteUInt32(reply.nonce);
    writer.WriteUInt64(reply.id.high);
    writer.WriteUInt64(reply.id.low);
    writer.WriteUInt64(reply.protocolHash);
    writer.WriteString(reply.name);
    writer.WriteUInt16(reply.gamePort);
    writer.WriteUInt16(reply.players);
    writer.WriteUInt16(reply.maxPlayers);
    writer.WriteBool(reply.needsSecret);
    writer.WriteString(reply.levelPath);
    writer.WriteVarUInt32(static_cast<std::uint32_t>(reply.gameData.size()));
    for (const auto &[key, value] : reply.gameData)
    {
        writer.WriteString(key);
        writer.WriteString(value);
    }
    if (writer.BytesWritten() > kMaxDiscoveryReplyBytes)
    {
        return std::unexpected(DiscoveryError::TooBig);
    }
    return std::vector<std::byte>{writer.Data().begin(), writer.Data().end()};
}

std::optional<DiscoveryReply> ReadDiscoveryReply(std::span<const std::byte> datagram)
{
    if (datagram.size() > kMaxDiscoveryReplyBytes)
    {
        return std::nullopt;
    }
    Core::BitReader reader(datagram);
    if (!ReadHeader(kReplyMagic, reader))
    {
        return std::nullopt;
    }
    DiscoveryReply reply;
    reply.nonce = reader.ReadUInt32();
    reply.id.high = reader.ReadUInt64();
    reply.id.low = reader.ReadUInt64();
    reply.protocolHash = reader.ReadUInt64();
    reply.name = reader.ReadString(kMaxSessionNameBytes);
    reply.gamePort = reader.ReadUInt16();
    reply.players = reader.ReadUInt16();
    reply.maxPlayers = reader.ReadUInt16();
    reply.needsSecret = reader.ReadBool();
    reply.levelPath = reader.ReadString(kMaxDiscoveryLevelBytes);
    const std::uint32_t pairs = reader.ReadVarUInt32();
    if (!reader.Ok() || pairs > kMaxGameDataPairs)
    {
        return std::nullopt;
    }
    reply.gameData.reserve(pairs);
    for (std::uint32_t i = 0; i < pairs; ++i)
    {
        std::string key = reader.ReadString(kMaxGameDataKeyBytes);
        std::string value = reader.ReadString(kMaxGameDataValueBytes);
        reply.gameData.emplace_back(std::move(key), std::move(value));
    }
    // A session with no id, or no port to join on, is not one a list can offer.
    if (!reader.Ok() || !reply.id.IsValid() || reply.gamePort == 0)
    {
        return std::nullopt;
    }
    return reply;
}

bool IsLocalNetworkAddress(std::string_view address)
{
    // A zone ("fe80::1%eth0") names the interface, not the address.
    address = address.substr(0, address.find('%'));
    if (const std::optional<std::array<std::uint8_t, 4>> ipv4 = ParseIpv4(address))
    {
        return IsLocalIpv4(*ipv4);
    }
    if (address.find(':') == std::string_view::npos)
    {
        return false;
    }

    std::string lower(address);
    for (char &symbol : lower)
    {
        if (symbol >= 'A' && symbol <= 'F')
        {
            symbol = static_cast<char>(symbol - 'A' + 'a');
        }
    }
    // An IPv4 address carried in IPv6, as a dual-stack socket reports one.
    constexpr std::string_view kMapped = "::ffff:";
    if (lower.starts_with(kMapped))
    {
        const std::optional<std::array<std::uint8_t, 4>> mapped =
            ParseIpv4(std::string_view(lower).substr(kMapped.size()));
        return mapped.has_value() && IsLocalIpv4(*mapped);
    }
    if (lower == "::1")
    {
        return true;
    }
    const std::string_view first = std::string_view(lower).substr(0, lower.find(':'));
    std::uint32_t hextet = 0;
    const std::from_chars_result parsed =
        std::from_chars(first.data(), first.data() + first.size(), hextet, /*base=*/ 16);
    if (first.empty() || parsed.ec != std::errc{} || parsed.ptr != first.data() + first.size())
    {
        return false;
    }
    constexpr std::uint32_t kUniqueLocalMask = 0xFE00; // fc00::/7
    constexpr std::uint32_t kUniqueLocal = 0xFC00;
    constexpr std::uint32_t kLinkLocalMask = 0xFFC0;   // fe80::/10
    constexpr std::uint32_t kLinkLocal = 0xFE80;
    return (hextet & kUniqueLocalMask) == kUniqueLocal || (hextet & kLinkLocalMask) == kLinkLocal;
}

} // namespace Assisi::NetSync
