/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestNetSession.cpp
/// @brief The front end an application actually uses: two NetSessions, real
/// UDP, and the Host/Join/Disconnect lifecycle.
///
/// This is the exact path the editor's network panel drives, so a bug here is a
/// bug a player would hit. It binds real loopback sockets on purpose — the
/// point is that Host() and Join() work, which a mocked transport would not
/// establish.

#include <doctest/doctest.h>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/NetSync/NetSession.hpp>

#include <chrono>
#include <cstdint>
#include <thread>

using namespace Assisi;
using namespace Assisi::NetSync;

namespace
{

/// Bind the first free port in a small range. The transport deliberately does
/// not expose the OS-assigned port (a server is always told which one to bind),
/// so the test picks; the range is scanned so a leftover socket fails one
/// iteration instead of the suite.
std::uint16_t HostOnFreePort(NetSession &session)
{
    for (std::uint16_t port = 27200; port < 27240; ++port)
    {
        if (session.Host(port))
        {
            // No ServerHello goes out until the host knows its content set. The
            // real thing kicks a scan when hosting starts and sets this when it
            // lands; these tests hand it the empty set's hash directly.
            session.SetContentSetHash(0);
            return port;
        }
    }
    return 0;
}

ECS::Entity SpawnReplicated(ECS::Scene &scene, float x)
{
    const ECS::Entity entity = scene.Create();
    ECS::Transform    transform;
    transform.position = {x, 0.f, 0.f};
    (void)scene.Add<ECS::Transform>(entity, transform);
    (void)scene.Add<Replicated>(entity, Replicated{});
    return entity;
}

/// Drive both sessions for a while. Real time has to pass — these are real
/// sockets and GNS does its wire work on its own thread.
void Pump(NetSession &host, NetSession &client, std::uint64_t &tick, std::int32_t steps)
{
    for (std::int32_t i = 0; i < steps; ++i)
    {
        host.Poll();
        client.Poll();
        host.Tick(tick);
        client.Tick(tick);
        client.SmoothView(1.f / 60.f);
        ++tick;
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
}

} // namespace

TEST_CASE("a session hosts, another joins, and the world arrives")
{
    ECS::Scene hostScene;
    ECS::Scene clientScene;

    NetSession host(hostScene);
    NetSession client(clientScene);

    CHECK(host.Role() == SessionRole::Offline);
    CHECK_FALSE(host.IsActive());

    const std::uint16_t port = HostOnFreePort(host);
    REQUIRE_MESSAGE(port != 0, "could not bind any port in 27200-27239: ", host.LastError());
    CHECK(host.IsHost());
    CHECK(host.StatusText() == "Hosting — 0 clients");

    SpawnReplicated(hostScene, 1.f);
    SpawnReplicated(hostScene, 2.f);
    SpawnReplicated(hostScene, 3.f);

    REQUIRE_MESSAGE(client.Join("127.0.0.1", port), client.LastError());
    client.SetContentSetHash(0);
    CHECK(client.IsClient());

    std::uint64_t tick = 0;
    Pump(host, client, tick, 120);

    CHECK(client.Client()->IsSynchronized());
    CHECK(client.Client()->IsWorldComplete());
    CHECK(host.Clients().size() == 1);

    const SessionStats clientStats = client.Stats();
    CHECK(clientStats.role == SessionRole::Client);
    CHECK(clientStats.replicatedEntities == 3);
    CHECK(clientStats.snapshotsRejected == 0);
    CHECK(clientStats.snapshotsApplied > 0);

    const SessionStats hostStats = host.Stats();
    CHECK(hostStats.role == SessionRole::Host);
    CHECK(hostStats.clientCount == 1);
    CHECK(hostStats.replicatedEntities == 3);
    CHECK(hostStats.snapshotsSent > 0);

    // The status line is what the UI shows, so it is worth asserting rather
    // than leaving to be noticed as "that looks wrong" later.
    CHECK(host.StatusText() == "Hosting — 1 client");
    CHECK(client.StatusText() == "Connected — 3 entities");
}

TEST_CASE("a client's mirrored entities leave with the session")
{
    ECS::Scene hostScene;
    ECS::Scene clientScene;
    NetSession host(hostScene);
    NetSession client(clientScene);

    const std::uint16_t port = HostOnFreePort(host);
    REQUIRE(port != 0);
    SpawnReplicated(hostScene, 1.f);
    SpawnReplicated(hostScene, 2.f);
    REQUIRE(client.Join("127.0.0.1", port));
    client.SetContentSetHash(0);

    std::uint64_t tick = 0;
    Pump(host, client, tick, 120);
    REQUIRE(client.Client()->ReplicatedEntityCount() == 2);

    client.Disconnect();
    clientScene.FlushDestroyed();

    // They belonged to the session, not to the scene: leaving them behind would
    // strand a frozen copy of someone else's world in front of the player.
    CHECK(client.Role() == SessionRole::Offline);
    CHECK_FALSE(client.IsActive());
    std::size_t remaining = 0;
    for (auto [entity, replicated] : clientScene.Query<Replicated>())
    {
        (void)entity;
        (void)replicated;
        ++remaining;
    }
    CHECK(remaining == 0);

    // The host notices, eventually — and keeps running either way.
    Pump(host, client, tick, 60);
    CHECK(host.IsHost());
}

TEST_CASE("hosting a port already taken fails with a reason, not a crash")
{
    ECS::Scene sceneA;
    ECS::Scene sceneB;
    NetSession first(sceneA);
    NetSession second(sceneB);

    const std::uint16_t port = HostOnFreePort(first);
    REQUIRE(port != 0);

    CHECK_FALSE(second.Host(port));
    CHECK(second.Role() == SessionRole::Offline);
    CHECK_FALSE(second.LastError().empty());
    // The failed attempt must leave nothing behind — a later Host() on a free
    // port has to work.
    CHECK(HostOnFreePort(second) != 0);
}

TEST_CASE("joining an unparseable address fails without leaving a session behind")
{
    ECS::Scene scene;
    NetSession session(scene);

    CHECK_FALSE(session.Join("not-an-ip-literal", 27015));
    CHECK(session.Role() == SessionRole::Offline);
    CHECK_FALSE(session.LastError().empty());
    CHECK(session.Client() == nullptr);
    CHECK(session.Server() == nullptr);
}

TEST_CASE("an offline session is inert")
{
    ECS::Scene scene;
    NetSession session(scene);

    // Every loop hook must be safe to call unconditionally — an application
    // should not have to guard each one on "am I networked".
    session.Poll();
    session.Tick(0);
    session.SmoothView(1.f / 60.f);
    CHECK(session.ConsumeInput(Net::ConnectionId{1}, 0) == nullptr);
    CHECK(session.Stats().role == SessionRole::Offline);
    CHECK(session.StatusText() == "Offline");
}

TEST_CASE("a session can be re-hosted after disconnecting")
{
    ECS::Scene scene;
    NetSession session(scene);

    const std::uint16_t first = HostOnFreePort(session);
    REQUIRE(first != 0);
    session.Disconnect();
    CHECK(session.Role() == SessionRole::Offline);

    // Disconnect has to actually release the socket, or the editor's
    // Host → Disconnect → Host gesture would fail the second time.
    CHECK(session.Host(first));
    CHECK(session.IsHost());
}
