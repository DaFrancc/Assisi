/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestSessionDiscovery.cpp
/// @brief The LAN discovery datagrams, and which addresses a host answers.

#include <doctest/doctest.h>

#include <Assisi/NetSync/SessionDiscovery.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

using namespace Assisi;
using namespace Assisi::NetSync;

namespace
{

DiscoveryReply SampleReply()
{
    DiscoveryReply reply;
    reply.id = Core::SessionId{.high = 0x0123456789ABCDEFull, .low = 0xFEDCBA9876543210ull};
    reply.protocolHash = 0xC0FFEEull;
    reply.name = "DaFrancc's hunt";
    reply.levelPath = "levels/NetPile.alvl";
    reply.gameData = {{"mode", "coop"}, {"map", "valley"}};
    reply.nonce = 0xDEADBEEFu;
    reply.gamePort = 27015;
    reply.players = 2;
    reply.maxPlayers = 4;
    reply.needsSecret = true;
    return reply;
}

} // namespace

TEST_CASE("a query reads back with its nonce")
{
    const std::vector<std::byte> bytes = WriteDiscoveryQuery(DiscoveryQuery{.nonce = 42});
    const std::optional<DiscoveryQuery> read = ReadDiscoveryQuery(bytes);
    REQUIRE(read.has_value());
    CHECK(read->nonce == 42);
}

TEST_CASE("a reply reads back with every field")
{
    const DiscoveryReply sent = SampleReply();
    const std::expected<std::vector<std::byte>, DiscoveryError> bytes = WriteDiscoveryReply(sent);
    REQUIRE(bytes.has_value());
    CHECK(bytes->size() <= kMaxDiscoveryReplyBytes);

    const std::optional<DiscoveryReply> read = ReadDiscoveryReply(*bytes);
    REQUIRE(read.has_value());
    CHECK(read->id == sent.id);
    CHECK(read->protocolHash == sent.protocolHash);
    CHECK(read->name == sent.name);
    CHECK(read->levelPath == sent.levelPath);
    CHECK(read->gameData == sent.gameData);
    CHECK(read->nonce == sent.nonce);
    CHECK(read->gamePort == sent.gamePort);
    CHECK(read->players == sent.players);
    CHECK(read->maxPlayers == sent.maxPlayers);
    CHECK(read->needsSecret);
}

TEST_CASE("a query is not a reply, and a reply is not a query")
{
    const std::vector<std::byte> query = WriteDiscoveryQuery(DiscoveryQuery{.nonce = 1});
    const std::vector<std::byte> reply = WriteDiscoveryReply(SampleReply()).value();
    CHECK_FALSE(ReadDiscoveryReply(query).has_value());
    CHECK_FALSE(ReadDiscoveryQuery(reply).has_value());
}

TEST_CASE("a truncated, garbled or other-version datagram is ignored")
{
    const std::vector<std::byte> reply = WriteDiscoveryReply(SampleReply()).value();
    for (std::size_t length = 0; length < reply.size(); ++length)
    {
        CHECK_FALSE(ReadDiscoveryReply(std::span(reply).first(length)).has_value());
    }

    std::vector<std::byte> wrongMagic = reply;
    wrongMagic[0] = std::byte{'X'};
    CHECK_FALSE(ReadDiscoveryReply(wrongMagic).has_value());

    // The version is the byte after the eight of the magic.
    constexpr std::size_t kVersionByte = 8;
    std::vector<std::byte> wrongVersion = reply;
    wrongVersion[kVersionByte] = std::byte{kDiscoveryVersion + 1};
    CHECK_FALSE(ReadDiscoveryReply(wrongVersion).has_value());

    const std::vector<std::byte> garbage(64, std::byte{0x5A});
    CHECK_FALSE(ReadDiscoveryReply(garbage).has_value());
    CHECK_FALSE(ReadDiscoveryQuery(garbage).has_value());
}

TEST_CASE("a reply over a field's limit is not written")
{
    DiscoveryReply longName = SampleReply();
    longName.name.assign(kMaxSessionNameBytes + 1, 'n');
    CHECK(WriteDiscoveryReply(longName).error() == DiscoveryError::TooLong);

    DiscoveryReply tooManyPairs = SampleReply();
    tooManyPairs.gameData.assign(kMaxGameDataPairs + 1, {"k", "v"});
    CHECK(WriteDiscoveryReply(tooManyPairs).error() == DiscoveryError::TooLong);

    DiscoveryReply longValue = SampleReply();
    longValue.gameData = {{"k", std::string(kMaxGameDataValueBytes + 1, 'v')}};
    CHECK(WriteDiscoveryReply(longValue).error() == DiscoveryError::TooLong);
}

TEST_CASE("a reply whose fields each fit but together are too big is not written")
{
    DiscoveryReply big = SampleReply();
    big.gameData.clear();
    for (std::size_t i = 0; i < kMaxGameDataPairs; ++i)
    {
        big.gameData.emplace_back(std::string(kMaxGameDataKeyBytes, 'k'), std::string(kMaxGameDataValueBytes, 'v'));
    }
    CHECK(WriteDiscoveryReply(big).error() == DiscoveryError::TooBig);
}

TEST_CASE("only local network addresses are answered")
{
    for (const char *local : {"10.1.2.3", "172.16.0.1", "172.31.255.255", "192.168.1.20", "169.254.3.4",
                              "127.0.0.1", "::1", "fe80::1%eth0", "FD12:3456::1", "fc00::5", "::ffff:192.168.0.9"})
    {
        CAPTURE(local);
        CHECK(IsLocalNetworkAddress(local));
    }
    for (const char *remote : {"8.8.8.8", "172.32.0.1", "172.15.255.255", "192.169.0.1", "2001:4860:4860::8888",
                               "::ffff:8.8.8.8", "not an address", "", "1.2.3", "256.1.1.1"})
    {
        CAPTURE(remote);
        CHECK_FALSE(IsLocalNetworkAddress(remote));
    }
}
