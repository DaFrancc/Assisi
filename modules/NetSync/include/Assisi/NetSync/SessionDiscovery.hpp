/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SessionDiscovery.hpp
/// @brief The two datagrams of LAN discovery: a joiner's query and a host's reply.
///
/// A joiner broadcasts a query; every listed host on the network answers it,
/// by unicast to the joiner, with what a browse list shows. A hidden host stays
/// silent. Neither datagram carries a secret.
///
/// A host answers only an address on the local network, and its reply is small
/// and capped, so it cannot be used to bounce traffic at someone else. The reply
/// is untrusted input on the joiner's side: every length is bounded on read.

#include <Assisi/Core/SessionId.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Assisi::NetSync
{

/// @brief The UDP port listed hosts listen for queries on.
inline constexpr std::uint16_t kLanDiscoveryPort = 27016;

/// @brief The datagram format's version. A datagram of another version is
/// ignored, as if it had never arrived.
inline constexpr std::uint8_t kDiscoveryVersion = 1;

/// @brief The largest reply a host sends: under any network's packet size, so
/// it is never split, and small next to the query that asks for it.
inline constexpr std::size_t kMaxDiscoveryReplyBytes = 1200;

/// @brief The longest session name.
inline constexpr std::size_t kMaxSessionNameBytes = 64;
/// @brief The most game data pairs a session carries.
inline constexpr std::size_t kMaxGameDataPairs = 16;
/// @brief The longest game data key.
inline constexpr std::size_t kMaxGameDataKeyBytes = 32;
/// @brief The longest game data value.
inline constexpr std::size_t kMaxGameDataValueBytes = 128;
/// @brief The longest level path a reply names.
inline constexpr std::size_t kMaxDiscoveryLevelBytes = 256;

/// @brief Key/value strings the host sets, such as `mode=coop`. The engine
/// carries them and never reads them.
using GameData = std::vector<std::pair<std::string, std::string>>;

/// @brief A joiner asking who is hosting.
struct DiscoveryQuery
{
    /// Random per search and echoed by every reply, so a reply to an older
    /// search, or to someone else's, is told apart.
    std::uint32_t nonce = 0;
};

/// @brief A listed host's answer.
struct DiscoveryReply
{
    Core::SessionId id;
    /// The host's NetProtocolHash, so a list can grey out a session this build
    /// cannot join.
    std::uint64_t protocolHash = 0;
    std::string name;
    std::string levelPath;
    GameData gameData;
    std::uint32_t nonce = 0;     ///< The query's, echoed.
    std::uint16_t gamePort = 0;  ///< Where the host's session listens.
    std::uint16_t players = 0;   ///< Including the host, when it plays.
    std::uint16_t maxPlayers = 0;
    bool needsSecret = false;    ///< A password or a code; which, the joiner never needs to know.
};

/// @brief Why a reply could not be written.
enum class DiscoveryError : std::uint8_t
{
    TooLong, ///< A name, key, value or path is over its limit, or there are too many pairs.
    TooBig,  ///< Everything fits its own limit, and the whole is over kMaxDiscoveryReplyBytes.
    Count_
};

[[nodiscard]] std::vector<std::byte> WriteDiscoveryQuery(const DiscoveryQuery &query);
/// @brief The query in @p datagram, or nothing when it is not one this build reads.
[[nodiscard]] std::optional<DiscoveryQuery> ReadDiscoveryQuery(std::span<const std::byte> datagram);

[[nodiscard]] std::expected<std::vector<std::byte>, DiscoveryError> WriteDiscoveryReply(const DiscoveryReply &reply);
/// @brief The reply in @p datagram, or nothing when it is not one this build
/// reads or breaks any limit above.
[[nodiscard]] std::optional<DiscoveryReply> ReadDiscoveryReply(std::span<const std::byte> datagram);

/// @brief Whether @p address, an IPv4 or IPv6 literal, is on a local network:
/// private (10/8, 172.16/12, 192.168/16, fc00::/7), link-local (169.254/16,
/// fe80::/10) or loopback (127/8, ::1). Only these are answered.
[[nodiscard]] bool IsLocalNetworkAddress(std::string_view address);

} // namespace Assisi::NetSync
