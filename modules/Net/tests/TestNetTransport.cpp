/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestNetTransport.cpp
/// @brief Stage 0/1 definition of done: GNS links, and the transport moves
/// reliable and unreliable bytes over both a loopback pair and a real socket.
///
/// These tests bind real UDP sockets on 127.0.0.1. That is deliberate — the
/// point of the stage is to prove the dependency and the wrapper actually work
/// end to end, which a mock would not. Everything is time-boxed so a firewall
/// or a busy port fails the assertion instead of hanging the suite.

#include <doctest/doctest.h>

#include <Assisi/Net/NetTransport.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace Assisi::Net;

namespace
{

using Clock = std::chrono::steady_clock;

std::vector<std::byte> Bytes(std::string_view text)
{
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (const char c : text)
        out.push_back(static_cast<std::byte>(c));
    return out;
}

std::string Text(const std::vector<std::byte> &payload)
{
    std::string out;
    out.reserve(payload.size());
    for (const std::byte b : payload)
        out.push_back(static_cast<char>(b));
    return out;
}

/// Pump one or two transports until @p done says so or @p timeout elapses,
/// accumulating every event seen into @p events.
///
/// The sleep matters: GNS does its wire work on an internal service thread, so
/// a tight spin on Poll() would burn a core and still not deliver anything
/// sooner. A millisecond is well under any timing this suite asserts on.
template <typename Predicate>
bool PumpUntil(std::vector<NetTransport *> transports, std::vector<std::vector<NetEvent>> &events,
               Predicate done, std::chrono::milliseconds timeout = std::chrono::milliseconds{5000})
{
    events.resize(transports.size());
    const Clock::time_point deadline = Clock::now() + timeout;

    std::vector<NetEvent> batch;
    while (Clock::now() < deadline)
    {
        for (std::size_t i = 0; i < transports.size(); ++i)
        {
            transports[i]->Poll(batch);
            for (NetEvent &event : batch)
                events[i].push_back(std::move(event));
        }
        if (done(events))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    // One last drain, so a predicate that became true during the final sleep is
    // not reported as a timeout.
    for (std::size_t i = 0; i < transports.size(); ++i)
    {
        transports[i]->Poll(batch);
        for (NetEvent &event : batch)
            events[i].push_back(std::move(event));
    }
    return done(events);
}

std::size_t CountOfType(const std::vector<NetEvent> &events, NetEvent::Type type)
{
    std::size_t count = 0;
    for (const NetEvent &event : events)
        if (event.type == type)
            ++count;
    return count;
}

const NetEvent *FindMessage(const std::vector<NetEvent> &events, std::string_view payload)
{
    for (const NetEvent &event : events)
        if (event.type == NetEvent::Type::Message && Text(event.payload) == payload)
            return &event;
    return nullptr;
}

/// Bind a listen socket on the first free port in a small range.
///
/// Port 0 would be tidier, but the transport deliberately does not expose the
/// OS-assigned port (a server is always told which port to bind), so the test
/// picks one. The range is scanned rather than hardcoded so a leftover socket
/// from a previous run — or anything else on the machine — fails one iteration
/// instead of the suite.
std::uint16_t ListenOnFreePort(NetTransport &server)
{
    for (std::uint16_t port = 27100; port < 27140; ++port)
        if (server.Listen(port))
            return port;
    return 0;
}

} // namespace

TEST_CASE("loopback pair delivers reliable and unreliable messages on both lanes")
{
    NetTransport transport;

    const auto [host, client] = transport.CreateLoopbackPair();
    REQUIRE(host != InvalidConnection);
    REQUIRE(client != InvalidConnection);
    CHECK(transport.ConnectionCount() == 2);

    // A socket pair is born connected; the transport synthesizes the events so
    // this path looks like a remote one to the layer above.
    std::vector<std::vector<NetEvent>> events;
    REQUIRE(PumpUntil({&transport}, events,
                      [](const auto &e) { return CountOfType(e[0], NetEvent::Type::Connected) == 2; }));

    CHECK(transport.Send(host, Bytes("control-reliable"), SendMode::Reliable, Lane::Control));
    CHECK(transport.Send(host, Bytes("snapshot-unreliable"), SendMode::Unreliable, Lane::Snapshot));
    CHECK(transport.Send(client, Bytes("input-unreliable"), SendMode::Unreliable, Lane::Snapshot));

    std::vector<std::vector<NetEvent>> received;
    REQUIRE(PumpUntil({&transport}, received,
                      [](const auto &e) { return CountOfType(e[0], NetEvent::Type::Message) == 3; }));

    const NetEvent *control  = FindMessage(received[0], "control-reliable");
    const NetEvent *snapshot = FindMessage(received[0], "snapshot-unreliable");
    const NetEvent *input    = FindMessage(received[0], "input-unreliable");
    REQUIRE(control != nullptr);
    REQUIRE(snapshot != nullptr);
    REQUIRE(input != nullptr);

    // Lane identity has to survive the round trip: the whole reason lanes exist
    // is that the replication layer routes on them.
    CHECK(control->lane == Lane::Control);
    CHECK(snapshot->lane == Lane::Snapshot);
    CHECK(control->connection == client); // sent from host, arrives at client
    CHECK(input->connection == host);
}

TEST_CASE("loopback pair reports a close to the other end")
{
    NetTransport transport;

    const auto [host, client] = transport.CreateLoopbackPair();
    REQUIRE(host != InvalidConnection);

    std::vector<std::vector<NetEvent>> connected;
    REQUIRE(PumpUntil({&transport}, connected,
                      [](const auto &e) { return CountOfType(e[0], NetEvent::Type::Connected) == 2; }));

    transport.Close(client, false);

    std::vector<std::vector<NetEvent>> events;
    REQUIRE(PumpUntil({&transport}, events,
                      [](const auto &e) { return CountOfType(e[0], NetEvent::Type::Disconnected) == 1; }));
    CHECK(events[0].back().connection == host);

    // Closing is what unregisters a handle, so sending to either end now fails
    // rather than quietly going nowhere.
    CHECK_FALSE(transport.Send(client, Bytes("gone"), SendMode::Reliable, Lane::Control));
}

TEST_CASE("client connects to a listen socket over real UDP and echoes both send modes")
{
    NetTransport server;
    NetTransport client;

    const std::uint16_t port = ListenOnFreePort(server);
    REQUIRE_MESSAGE(port != 0, "could not bind any port in 27100-27139: ", server.LastError());
    CHECK(server.IsListening());

    const ConnectionId toServer = client.Connect("127.0.0.1", port);
    REQUIRE_MESSAGE(toServer != InvalidConnection, client.LastError());

    // Both sides must reach Connected: the client learns its handshake finished,
    // the server learns a client showed up (and which handle it got).
    std::vector<std::vector<NetEvent>> events;
    REQUIRE(PumpUntil({&server, &client}, events, [](const auto &e) {
        return CountOfType(e[0], NetEvent::Type::Connected) == 1 &&
               CountOfType(e[1], NetEvent::Type::Connected) == 1;
    }));

    const ConnectionId toClient = events[0].front().connection;
    REQUIRE(toClient != InvalidConnection);
    CHECK(server.ConnectionCount() == 1);

    CHECK(client.Send(toServer, Bytes("hello-reliable"), SendMode::Reliable, Lane::Control));
    CHECK(client.Send(toServer, Bytes("hello-unreliable"), SendMode::Unreliable, Lane::Snapshot));

    std::vector<std::vector<NetEvent>> serverEvents;
    REQUIRE(PumpUntil({&server}, serverEvents,
                      [](const auto &e) { return CountOfType(e[0], NetEvent::Type::Message) == 2; }));
    REQUIRE(FindMessage(serverEvents[0], "hello-reliable") != nullptr);
    REQUIRE(FindMessage(serverEvents[0], "hello-unreliable") != nullptr);

    // ...and back the other way, which is the direction snapshots travel.
    CHECK(server.Send(toClient, Bytes("snapshot"), SendMode::Unreliable, Lane::Snapshot));
    std::vector<std::vector<NetEvent>> clientEvents;
    REQUIRE(PumpUntil({&client}, clientEvents,
                      [](const auto &e) { return CountOfType(e[0], NetEvent::Type::Message) == 1; }));
    CHECK(Text(clientEvents[0].back().payload) == "snapshot");
    CHECK(clientEvents[0].back().lane == Lane::Snapshot);

    ConnectionStats stats;
    CHECK(client.GetConnectionStats(toServer, stats));
    CHECK(stats.pingMs >= 0);
    // Quality is a rolling measurement GNS reports as -1 until it has seen
    // enough packets, which a connection this young has not. Assert the range
    // it can legitimately be in rather than a number that only holds later.
    CHECK(stats.connectionQualityLocal <= 1.f);
    CHECK_FALSE(client.GetConnectionStats(InvalidConnection, stats));

    // A server-side close must reach the client as a Disconnected event — the
    // signal the replication layer tears a session down on.
    server.Close(toClient, false);
    std::vector<std::vector<NetEvent>> closeEvents;
    REQUIRE(PumpUntil({&client}, closeEvents,
                      [](const auto &e) { return CountOfType(e[0], NetEvent::Type::Disconnected) == 1; }));
    CHECK(closeEvents[0].back().connection == toServer);
}

TEST_CASE("network-loopback pair under simulated lag and loss still converges")
{
    // The soak fixture every later stage reuses. Note the mode: the default
    // in-process socket pair bypasses the packet layer entirely and would ignore
    // these settings, so this must be the bUseNetworkLoopback=true variant.
    NetTransport transport;

    SimulatedConditions conditions;
    conditions.sendLossPercent = 5.f;
    conditions.sendLagMs       = 25;
    conditions.recvLagMs       = 25;
    conditions.sendJitterMs    = 5;
    REQUIRE(NetTransport::SetSimulatedConditions(conditions));

    const auto [host, client] = transport.CreateLoopbackPair(true);
    REQUIRE(host != InvalidConnection);
    REQUIRE(client != InvalidConnection);

    // Reliable delivery is the guarantee under test: with 5% send loss, every
    // one of these must still arrive, in order, on its lane.
    constexpr std::size_t kMessages = 40;
    for (std::size_t i = 0; i < kMessages; ++i)
        CHECK(transport.Send(host, Bytes("m" + std::to_string(i)), SendMode::Reliable, Lane::Control));

    std::vector<std::vector<NetEvent>> events;
    const bool arrived = PumpUntil({&transport}, events,
                                   [](const auto &e) {
        return CountOfType(e[0], NetEvent::Type::Message) == kMessages;
    },
                                   std::chrono::milliseconds{15000});

    // Reset before asserting: these are process-global in GNS, so leaving them
    // set would impair every test that runs after this one.
    SimulatedConditions clear;
    NetTransport::SetSimulatedConditions(clear);

    REQUIRE(arrived);
    std::size_t index = 0;
    for (const NetEvent &event : events[0])
    {
        if (event.type != NetEvent::Type::Message)
            continue;
        CHECK(Text(event.payload) == "m" + std::to_string(index));
        CHECK(event.lane == Lane::Control);
        ++index;
    }
    CHECK(index == kMessages);
}
