/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestUdpSocket.cpp
/// @brief The plain UDP socket LAN discovery broadcasts on.
///
/// Real sockets on this machine. The broadcast case uses the loopback network's
/// broadcast address, which reaches every socket here sharing the port without
/// needing a LAN.

#include <doctest/doctest.h>

#include <Assisi/Net/UdpSocket.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace Assisi::Net;

namespace
{

std::vector<std::byte> Bytes(std::string_view text)
{
    std::vector<std::byte> out;
    for (const char c : text)
    {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

/// The first datagram @p socket receives within a second, or nothing.
std::optional<UdpDatagram> Await(UdpSocket &socket)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (std::optional<UdpDatagram> datagram = socket.Receive())
        {
            return datagram;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("a socket on port 0 gets a free port, and receives nothing it was not sent")
{
    std::expected<UdpSocket, std::string> socket = UdpSocket::Open(0, UdpPortUse::Own);
    REQUIRE_MESSAGE(socket.has_value(), socket.error());
    CHECK(socket->Port() != 0);
    CHECK_FALSE(socket->Receive().has_value());
}

TEST_CASE("a datagram goes to one address and an answer comes back to its sender")
{
    std::expected<UdpSocket, std::string> host = UdpSocket::Open(0, UdpPortUse::Shared);
    std::expected<UdpSocket, std::string> joiner = UdpSocket::Open(0, UdpPortUse::Own);
    REQUIRE(host.has_value());
    REQUIRE(joiner.has_value());

    REQUIRE(joiner->SendTo(UdpEndpoint{.address = "127.0.0.1", .port = host->Port()}, Bytes("who is there")));
    const std::optional<UdpDatagram> query = Await(*host);
    REQUIRE(query.has_value());
    CHECK(query->bytes == Bytes("who is there"));
    CHECK(query->from.port == joiner->Port());

    REQUIRE(host->SendTo(query->from, Bytes("me")));
    const std::optional<UdpDatagram> reply = Await(*joiner);
    REQUIRE(reply.has_value());
    CHECK(reply->bytes == Bytes("me"));
}

TEST_CASE("a broadcast reaches every socket sharing the port")
{
    std::expected<UdpSocket, std::string> first = UdpSocket::Open(0, UdpPortUse::Shared);
    REQUIRE(first.has_value());
    std::expected<UdpSocket, std::string> second = UdpSocket::Open(first->Port(), UdpPortUse::Shared);
    REQUIRE_MESSAGE(second.has_value(), second.error());
    std::expected<UdpSocket, std::string> joiner = UdpSocket::Open(0, UdpPortUse::Own);
    REQUIRE(joiner.has_value());

    REQUIRE(joiner->SendTo(UdpEndpoint{.address = "127.255.255.255", .port = first->Port()}, Bytes("anyone?")));
    CHECK(Await(*first).has_value());
    CHECK(Await(*second).has_value());
}

TEST_CASE("an owned port cannot be bound twice")
{
    std::expected<UdpSocket, std::string> first = UdpSocket::Open(0, UdpPortUse::Own);
    REQUIRE(first.has_value());
    CHECK_FALSE(UdpSocket::Open(first->Port(), UdpPortUse::Own).has_value());
}

TEST_CASE("the broadcast addresses include the limited and loopback ones")
{
    const std::vector<std::string> addresses = UdpSocket::BroadcastAddresses();
    CHECK(std::find(addresses.begin(), addresses.end(), "255.255.255.255") != addresses.end());
    CHECK(std::find(addresses.begin(), addresses.end(), "127.255.255.255") != addresses.end());
}

TEST_CASE("an address that does not parse is not sent to")
{
    std::expected<UdpSocket, std::string> socket = UdpSocket::Open(0, UdpPortUse::Own);
    REQUIRE(socket.has_value());
    CHECK_FALSE(socket->SendTo(UdpEndpoint{.address = "not an address", .port = 1}, Bytes("x")));
}
