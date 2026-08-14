/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestContentSet.cpp
/// @brief The join gate that makes blueprint replication safe: both machines
/// hold the same level and blueprint files, or nobody connects.
///
/// The check is deliberately strict — *any* difference refuses, including files
/// neither machine ever loads — and what it buys is the property the rest of the
/// design leans on: after a successful join, both sides are known to expand any
/// blueprint identically, so a spawn can travel as "expand file #7" rather than
/// as every component of every member (docs/blueprint-system-concept.md §9).
///
/// The subtle half is the *timing*. Both hellos are sent exactly once and never
/// resent, so a hello sent before its side knows its content set is a join with
/// no correct outcome — no retry can fix it. Each side therefore withholds until
/// it knows, which is what the last two cases here pin down.

#include <doctest/doctest.h>

#include <ostream>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Net/NetTransport.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>

using namespace Assisi;
using namespace Assisi::NetSync;

namespace
{

/// A loopback pair with nothing pre-arranged: each case decides for itself which
/// side learns its content set, and when.
struct Harness
{
    Net::NetTransport transport;
    ECS::Scene serverScene;
    ECS::Scene clientScene;

    std::pair<Net::ConnectionId, Net::ConnectionId> pair;

    ReplicationServer server;
    ReplicationClient client;

    std::uint64_t tick = 0;

    Harness()
        : pair(transport.CreateLoopbackPair()), server(transport, serverScene, /*physics=*/ nullptr),
        client(transport, clientScene, pair.second)
    {
    }

    void Step(int32_t steps = 1)
    {
        for (int32_t i = 0; i < steps; ++i)
        {
            std::vector<Net::NetEvent> events;
            transport.Poll(events);
            for (const Net::NetEvent &event : events)
            {
                if (event.type != Net::NetEvent::Type::Message)
                    continue;
                if (event.connection == pair.first)
                    server.HandleMessage(pair.first, event.payload);
                else if (event.connection == pair.second)
                    client.HandleMessage(event.payload);
            }
            server.Tick(tick++);
        }
    }
};

} // namespace

TEST_CASE("ContentSet: matching hashes join")
{
    Harness harness;
    harness.server.SetContentSetHash(0xdeadbeefcafef00dULL);
    harness.client.SetContentSetHash(0xdeadbeefcafef00dULL);
    harness.server.AddConnection(harness.pair.first);

    harness.Step(4);
    CHECK(harness.client.IsSynchronized());
    CHECK(harness.client.RejectMessage().empty());
}

TEST_CASE("ContentSet: a different set is refused, and the client is told why")
{
    Harness harness;
    harness.server.SetContentSetHash(0xdeadbeefcafef00dULL);
    // One stray .abp is enough, and indistinguishable from a car whose wheels
    // moved — which is the whole point of hashing the set rather than the level.
    harness.client.SetContentSetHash(0xdeadbeefcafef00eULL);
    harness.server.AddConnection(harness.pair.first);

    harness.Step(4);
    CHECK_FALSE(harness.client.IsSynchronized());
    CHECK(harness.client.RejectMessage().find("content") != std::string::npos);
}

TEST_CASE("ContentSet: a server with no hash yet sends no hello at all")
{
    Harness harness;
    harness.client.SetContentSetHash(7);
    harness.server.AddConnection(harness.pair.first);

    // Registered, but silent: a hello answered before the server can check the
    // answer is a join it could neither accept nor refuse, because the answer
    // comes exactly once.
    harness.Step(4);
    CHECK_FALSE(harness.client.IsSynchronized());
    CHECK(harness.client.Handshake().protocolHash == 0); // no ServerHello arrived
    CHECK_FALSE(harness.server.HasContentSetHash());

    // The scan lands; the waiting connection is served and the join completes.
    harness.server.SetContentSetHash(7);
    harness.Step(4);
    CHECK(harness.client.IsSynchronized());
}

TEST_CASE("ContentSet: a client with no hash yet holds its answer, then joins")
{
    Harness harness;
    harness.server.SetContentSetHash(7);
    harness.server.AddConnection(harness.pair.first);

    // The ServerHello arrives and is accepted — the client simply cannot answer
    // it yet, and answering with a placeholder would be a refusal no retry fixes.
    harness.Step(4);
    CHECK(harness.client.Handshake().protocolHash != 0);
    CHECK_FALSE(harness.client.IsSynchronized());

    harness.client.SetContentSetHash(7);
    harness.Step(4);
    CHECK(harness.client.IsSynchronized());
}
