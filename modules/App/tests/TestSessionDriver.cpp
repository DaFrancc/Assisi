/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestSessionDriver.cpp
/// @brief Hosting, joining, leaving and browsing through `ctx.net`, as every
/// program drives them: requests applied at the frame's safe point, events
/// raised for what happened.
///
/// Real sockets on this machine. Each case searches on a port of its own, so a
/// game running here, or a case running beside it, does not answer.

#include "PlayerSessionTesting.hpp"

#include <doctest/doctest.h>

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/SessionDriver.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Net/NetTransport.hpp>
#include <Assisi/Net/UdpSocket.hpp>
#include <Assisi/NetSync/NetSession.hpp>
#include <Assisi/NetSync/NetworkConfig.hpp>
#include <Assisi/NetSync/SessionSecret.hpp>
#include <Assisi/Physics/CollisionSource.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace Assisi;
using namespace Assisi::App;

namespace
{

/// How long a case waits for something over the wire. Past a search's length,
/// so a search that should end has ended.
constexpr std::chrono::milliseconds kWireTimeout{6000};

/// A UDP port nothing on this machine holds right now.
std::uint16_t FreeUdpPort()
{
    std::expected<Net::UdpSocket, std::string> probe = Net::UdpSocket::Open(0, Net::UdpPortUse::Own);
    REQUIRE(probe.has_value());
    return probe->Port();
}

NetSync::LevelIdentity TestLevel()
{
    NetSync::LevelIdentity level;
    level.addressing = NetSync::LevelAddressing::Virtual;
    level.path = "levels/Session.alvl";
    return level;
}

/// One machine: a world, its driver, and every session event it has heard.
struct Peer
{
    explicit Peer(std::uint16_t discoveryPort)
        : driver(universal,
                 SessionHooks{.currentLevel = [] { return TestLevel(); },
                              .buildJoinedLevel = [this](const NetSync::LevelIdentity &) { return BuildLevel(); },
                              .restoreAlone = [this] { ++restored; }},
                 {}, discoveryPort)
    {
    }

    std::expected<void, std::string> BuildLevel()
    {
        ++built;
        if (failBuild)
        {
            return std::unexpected(std::string("the test refused it"));
        }
        return {};
    }

    App::World world{Physics::NoCollisionAssets()};
    Core::EventQueue universal{Core::EventScope::Universal};
    SessionDriver driver;
    std::vector<SessionStarted> started;
    std::vector<SessionFailed> failed;
    std::vector<SessionJoined> joined;
    std::vector<SessionRefused> refused;
    std::vector<SessionEnded> ended;
    std::vector<SessionFound> found;
    std::vector<SessionLost> lost;
    std::vector<SessionSearchDone> searchesDone;
    std::uint64_t tick = 0;
    std::int32_t restored = 0;
    std::int32_t built = 0;
    bool failBuild = false;
};

template <typename E> void Collect(Core::EventQueue &queue, std::vector<E> &into)
{
    for (const E &event : queue.Read<E>())
    {
        into.push_back(event);
    }
}

/// One frame on @p peer: a fixed step inside its session, then the safe point.
void Frame(Peer &peer)
{
    (void)peer.driver.BeginStep(++peer.tick, /*stepping=*/ true);
    peer.driver.EndStep();
    peer.driver.Update(peer.world, Testing::kStep);
    Collect(peer.universal, peer.started);
    Collect(peer.universal, peer.failed);
    Collect(peer.universal, peer.joined);
    Collect(peer.universal, peer.refused);
    Collect(peer.universal, peer.ended);
    Collect(peer.universal, peer.found);
    Collect(peer.universal, peer.lost);
    Collect(peer.universal, peer.searchesDone);
    peer.universal.Flush();
}

/// Frames every peer until @p done, or the timeout.
bool RunUntil(const std::vector<Peer *> &peers, const std::function<bool ()> &done)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kWireTimeout;
    while (!done() && std::chrono::steady_clock::now() < deadline)
    {
        for (Peer *peer : peers)
        {
            Frame(*peer);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    return done();
}

/// Hosts from @p peer's world with @p options, and waits for the answer.
void HostThrough(Peer &peer, HostOptions options)
{
    peer.world.net.Host(std::move(options));
    REQUIRE(RunUntil({&peer}, [&] { return !peer.started.empty() || !peer.failed.empty(); }));
    REQUIRE(peer.started.size() == 1);
}

/// "127.0.0.1:port", the address @p host's session is joined at here.
std::string AddressOf(const Peer &host)
{
    return "127.0.0.1:" + std::to_string(host.world.net.Session()->port);
}

/// Searches from @p peer, and waits for the search to end.
void Search(Peer &peer, const std::vector<Peer *> &others)
{
    const std::size_t before = peer.searchesDone.size();
    peer.world.net.FindSessions();
    std::vector<Peer *> everyone = others;
    everyone.push_back(&peer);
    REQUIRE(RunUntil(everyone, [&] { return peer.searchesDone.size() > before; }));
}

bool NetworkedLocally(Peer &peer) { return peer.world.resources.Get<LocalClient>().networked; }

/// Turns direct connections off for as long as it lives, and back on after,
/// however the case ends.
struct DirectConnectionsOff
{
    DirectConnectionsOff()
    {
        NetSync::NetworkConfig off;
        off.allowDirectConnections = false;
        NetSync::ApplyNetworkConfig(off);
    }
    ~DirectConnectionsOff() { NetSync::ApplyNetworkConfig(NetSync::NetworkConfig{}); }
    DirectConnectionsOff(const DirectConnectionsOff &) = delete;
    DirectConnectionsOff &operator=(const DirectConnectionsOff &) = delete;
};

struct Fixture
{
    Fixture() { (void)Testing::FreshRoot("assisi-session-driver"); }

    std::uint16_t discovery = FreeUdpPort();
};

} // namespace

TEST_CASE("an address is read with or without its port")
{
    const std::optional<SessionAddress> plain = ParseSessionAddress("192.168.1.20");
    REQUIRE(plain.has_value());
    CHECK(plain->host == "192.168.1.20");
    CHECK(plain->port == kDefaultSessionPort);

    const std::optional<SessionAddress> withPort = ParseSessionAddress("192.168.1.20:27020");
    REQUIRE(withPort.has_value());
    CHECK(withPort->port == 27020);

    const std::optional<SessionAddress> ipv6 = ParseSessionAddress("[fe80::1]:27021");
    REQUIRE(ipv6.has_value());
    CHECK(ipv6->host == "fe80::1");
    CHECK(ipv6->port == 27021);
    CHECK(ParseSessionAddress("::1")->host == "::1");

    for (const char *bad : {"", ":27015", "1.2.3.4:", "1.2.3.4:0", "1.2.3.4:70000", "1.2.3.4:abc", "[::1"})
    {
        CAPTURE(bad);
        CHECK_FALSE(ParseSessionAddress(bad).has_value());
    }
}

TEST_CASE("a host request is applied at the safe point, and the session says its id, port and code")
{
    Fixture fixture;
    Peer host(fixture.discovery);
    host.world.net.Host(HostOptions{.name = "Lodge", .visibility = Visibility::Hidden, .access = Access::Code});
    CHECK_FALSE(host.world.net.Session().has_value()); // only queued so far

    REQUIRE(RunUntil({&host}, [&] { return !host.started.empty(); }));
    const std::optional<SessionInfo> &info = host.world.net.Session();
    REQUIRE(info.has_value());
    CHECK(info->hosting);
    CHECK(info->id.IsValid());
    CHECK(info->name == "Lodge");
    CHECK(info->port != 0);
    CHECK(info->code.size() == NetSync::kSessionCodeLength);
    CHECK(info->players == 1);
    // From the first step inside the session.
    CHECK(RunUntil({&host}, [&] { return NetworkedLocally(host); }));
}

TEST_CASE("hosting on a port already taken fails, saying so")
{
    Fixture fixture;
    std::unique_ptr<Net::NetTransport> squatter = Net::NetTransport::Create().value();
    REQUIRE(squatter->Listen(0));

    Peer host(fixture.discovery);
    host.world.net.Host(HostOptions{.name = "Lodge", .port = squatter->ListenPort()});
    REQUIRE(RunUntil({&host}, [&] { return !host.failed.empty(); }));
    CHECK(host.failed.front().reason == HostFailure::PortUnavailable);
    CHECK_FALSE(host.world.net.Session().has_value());
}

TEST_CASE("hosting with a name too long, or a password missing, fails without opening anything")
{
    Fixture fixture;
    Peer host(fixture.discovery);
    host.world.net.Host(HostOptions{.name = std::string(NetSync::kMaxSessionNameBytes + 1, 'n')});
    REQUIRE(RunUntil({&host}, [&] { return !host.failed.empty(); }));
    CHECK(host.failed.back().reason == HostFailure::TooMuchData);

    host.world.net.Host(HostOptions{.name = "Lodge", .access = Access::Password});
    REQUIRE(RunUntil({&host}, [&] { return host.failed.size() == 2; }));
    CHECK(host.failed.back().reason == HostFailure::BadPassword);
    CHECK_FALSE(host.driver.InSession());
}

TEST_CASE("a listed host is found with its name, players and game data; a hidden one is not")
{
    Fixture fixture;
    Peer listed(fixture.discovery);
    Peer hidden(fixture.discovery);
    Peer browser(fixture.discovery);
    HostThrough(listed, HostOptions{.name = "Listed lodge",
                                    .access = Access::Password,
                                    .password = "hunter2",
                                    .maxPlayers = 4,
                                    .gameData = {{"mode", "coop"}}});
    HostThrough(hidden, HostOptions{.name = "Hidden lodge", .visibility = Visibility::Hidden});

    Search(browser, {&listed, &hidden});
    REQUIRE(browser.found.size() == 1);
    const SessionEntry &entry = browser.found.front().entry;
    CHECK(entry.name == "Listed lodge");
    CHECK(entry.id == listed.world.net.Session()->id);
    CHECK(entry.players == 1);
    CHECK(entry.maxPlayers == 4);
    CHECK(entry.needsSecret);
    CHECK(entry.compatible);
    CHECK(entry.level == TestLevel().path);
    CHECK(entry.gameData == GameData{{"mode", "coop"}});
    CHECK(entry.port == listed.world.net.Session()->port);
    CHECK(entry.pingMs >= 0);
    CHECK(browser.world.net.Sessions().size() == 1);
}

TEST_CASE("two hosts on one machine are both found")
{
    Fixture fixture;
    Peer first(fixture.discovery);
    Peer second(fixture.discovery);
    Peer browser(fixture.discovery);
    HostThrough(first, HostOptions{.name = "First"});
    HostThrough(second, HostOptions{.name = "Second"});
    Search(browser, {&first, &second});
    CHECK(browser.found.size() == 2);
    CHECK(browser.world.net.Sessions().size() == 2);
}

TEST_CASE("a repeat search keeps what still answers, and loses what stopped")
{
    Fixture fixture;
    Peer staying(fixture.discovery);
    Peer going(fixture.discovery);
    Peer browser(fixture.discovery);
    HostThrough(staying, HostOptions{.name = "Staying"});
    HostThrough(going, HostOptions{.name = "Going"});
    Search(browser, {&staying, &going});
    REQUIRE(browser.found.size() == 2);

    // Nothing changed, so nothing is announced again and nothing is lost.
    Search(browser, {&staying, &going});
    CHECK(browser.found.size() == 2);
    CHECK(browser.lost.empty());

    going.world.net.Leave();
    REQUIRE(RunUntil({&going}, [&] { return !going.ended.empty(); }));
    Search(browser, {&staying});
    REQUIRE(browser.lost.size() == 1);
    CHECK(browser.lost.front().id != staying.world.net.Session()->id);
    REQUIRE(browser.world.net.Sessions().size() == 1);
    CHECK(browser.world.net.Sessions().front().name == "Staying");
}

TEST_CASE("a client joins a found session, builds the host's level, and leaving plays alone again")
{
    Fixture fixture;
    Peer host(fixture.discovery);
    Peer client(fixture.discovery);
    HostThrough(host, HostOptions{.name = "Lodge"});
    Search(client, {&host});
    REQUIRE(client.found.size() == 1);

    client.world.net.Join(client.found.front().entry);
    REQUIRE(RunUntil({&host, &client}, [&] { return !client.joined.empty() || !client.refused.empty(); }));
    REQUIRE(client.joined.size() == 1);
    CHECK(client.built == 1);
    CHECK(NetworkedLocally(client));
    CHECK_FALSE(client.world.net.Session()->hosting);
    CHECK(RunUntil({&host, &client}, [&] { return host.world.net.Session()->players == 2; }));

    client.world.net.Leave();
    REQUIRE(RunUntil({&host, &client}, [&] { return !client.ended.empty(); }));
    CHECK(client.ended.front().reason == EndReason::Left);
    CHECK(client.restored == 1);
    CHECK_FALSE(client.world.net.Session().has_value());
    CHECK_FALSE(NetworkedLocally(client));
}

TEST_CASE("a client is told the host left, and plays alone again")
{
    Fixture fixture;
    Peer host(fixture.discovery);
    Peer client(fixture.discovery);
    HostThrough(host, HostOptions{.name = "Lodge"});
    client.world.net.Join(AddressOf(host));
    REQUIRE(RunUntil({&host, &client}, [&] { return !client.joined.empty(); }));

    host.world.net.Leave();
    REQUIRE(RunUntil({&host, &client}, [&] { return !client.ended.empty(); }));
    CHECK(host.ended.front().reason == EndReason::Left);
    CHECK(client.ended.front().reason == EndReason::HostLeft);
    CHECK(client.restored == 1);
    CHECK_FALSE(NetworkedLocally(client));
    CHECK(host.restored == 0); // a host keeps its world
}

TEST_CASE("a hidden session with a code is joined by its address and the code, however it is typed")
{
    Fixture fixture;
    Peer host(fixture.discovery);
    Peer client(fixture.discovery);
    HostThrough(host, HostOptions{.name = "Lodge", .visibility = Visibility::Hidden, .access = Access::Code});
    std::string typed = host.world.net.Session()->code;
    for (char &symbol : typed)
    {
        symbol = static_cast<char>(symbol >= 'A' && symbol <= 'Z' ? symbol - 'A' + 'a' : symbol);
    }
    typed.insert(3, " - ");

    client.world.net.Join(AddressOf(host), typed);
    REQUIRE(RunUntil({&host, &client}, [&] { return !client.joined.empty() || !client.refused.empty(); }));
    CHECK(client.joined.size() == 1);
}

TEST_CASE("a wrong password is refused before the host's level is built, and the world is left alone")
{
    Fixture fixture;
    Peer host(fixture.discovery);
    Peer client(fixture.discovery);
    HostThrough(host, HostOptions{.name = "Lodge", .access = Access::Password, .password = "hunter2"});
    client.world.net.Join(AddressOf(host), "hunter3");
    REQUIRE(RunUntil({&host, &client}, [&] { return !client.refused.empty(); }));
    CHECK(client.refused.front().reason == JoinRefusal::WrongSecret);
    CHECK(client.built == 0);
    CHECK(client.restored == 0);
    CHECK_FALSE(client.driver.InSession());

    client.world.net.Join(AddressOf(host), "hunter2");
    REQUIRE(RunUntil({&host, &client}, [&] { return !client.joined.empty(); }));
}

TEST_CASE("a full session refuses with Full")
{
    Fixture fixture;
    Peer host(fixture.discovery);
    Peer client(fixture.discovery);
    // The host plays, so a session of one has no room for anyone else.
    HostThrough(host, HostOptions{.name = "Lodge", .maxPlayers = 1});
    client.world.net.Join(AddressOf(host));
    REQUIRE(RunUntil({&host, &client}, [&] { return !client.refused.empty(); }));
    CHECK(client.refused.front().reason == JoinRefusal::Full);
}

TEST_CASE("a host level this machine cannot build refuses the join and plays alone again")
{
    Fixture fixture;
    Peer host(fixture.discovery);
    Peer client(fixture.discovery);
    client.failBuild = true;
    HostThrough(host, HostOptions{.name = "Lodge"});
    client.world.net.Join(AddressOf(host));
    REQUIRE(RunUntil({&host, &client}, [&] { return !client.refused.empty(); }));
    CHECK(client.refused.front().reason == JoinRefusal::Level);
    CHECK(client.restored == 1);
}

TEST_CASE("nobody at the address refuses as unreachable")
{
    Fixture fixture;
    Peer client(fixture.discovery);
    client.world.net.Join("not an address");
    REQUIRE(RunUntil({&client}, [&] { return !client.refused.empty(); }));
    CHECK(client.refused.front().reason == JoinRefusal::Unreachable);

    client.world.net.Join(SessionEntry{});
    REQUIRE(RunUntil({&client}, [&] { return client.refused.size() == 2; }));
    CHECK(client.refused.back().reason == JoinRefusal::NotFound);
}

TEST_CASE("hosting or joining while in a session is refused, and the session goes on")
{
    Fixture fixture;
    Peer host(fixture.discovery);
    HostThrough(host, HostOptions{.name = "Lodge"});
    host.world.net.Host(HostOptions{.name = "Again"});
    REQUIRE(RunUntil({&host}, [&] { return !host.failed.empty(); }));
    CHECK(host.failed.front().reason == HostFailure::AlreadyInSession);

    host.world.net.Join("127.0.0.1:27015");
    REQUIRE(RunUntil({&host}, [&] { return !host.refused.empty(); }));
    CHECK(host.refused.front().reason == JoinRefusal::AlreadyInSession);
    CHECK(host.driver.InSession());
    CHECK(host.world.net.Session()->name == "Lodge");
}

namespace
{

/// One step of each of @p plays, answering a client's handshake once it asks.
void StepPlays(const std::vector<NetworkedPlay *> &plays, std::uint64_t tick)
{
    for (NetworkedPlay *play : plays)
    {
        (void)play->BeginStep(tick, /*stepping=*/ true);
        if (play->Session().IsAwaitingLevel())
        {
            play->ConfirmLevelReady();
        }
        play->EndStep();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
}

} // namespace

TEST_CASE("a client whose host left plays alone from its next step, before anything tears its session down")
{
    Fixture fixture;
    App::World hostWorld{Physics::NoCollisionAssets()};
    App::World clientWorld{Physics::NoCollisionAssets()};
    NetworkedPlay host{hostWorld};
    NetworkedPlay client{clientWorld};
    REQUIRE(host.Host(0, TestLevel(), HostRole::Player));
    REQUIRE(client.Join("127.0.0.1", host.Session().BoundPort()));

    std::uint64_t tick = 0;
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kWireTimeout;
    while (host.Session().ReadyClients().empty() && std::chrono::steady_clock::now() < deadline)
    {
        StepPlays({&host, &client}, ++tick);
    }
    REQUIRE_FALSE(host.Session().ReadyClients().empty());
    REQUIRE(clientWorld.resources.Get<LocalClient>().networked);

    host.Session().Disconnect();
    const std::chrono::steady_clock::time_point closeDeadline = std::chrono::steady_clock::now() + kWireTimeout;
    while (client.Session().IsActive() && std::chrono::steady_clock::now() < closeDeadline)
    {
        StepPlays({&client}, ++tick);
    }
    REQUIRE_FALSE(client.Session().IsActive());
    StepPlays({&client}, ++tick);
    CHECK_FALSE(clientWorld.resources.Get<LocalClient>().networked);
}

TEST_CASE("with direct connections off, nothing hosts, joins or searches")
{
    Fixture fixture;
    const DirectConnectionsOff off;

    Peer peer(fixture.discovery);
    peer.world.net.Host(HostOptions{.name = "Lodge"});
    REQUIRE(RunUntil({&peer}, [&] { return !peer.failed.empty(); }));
    CHECK(peer.failed.front().reason == HostFailure::DirectConnectionsForbidden);

    peer.world.net.Join("127.0.0.1:27015");
    REQUIRE(RunUntil({&peer}, [&] { return !peer.refused.empty(); }));
    CHECK(peer.refused.front().reason == JoinRefusal::DirectConnectionsForbidden);

    peer.world.net.FindSessions();
    Frame(peer);
    CHECK(peer.searchesDone.size() == 1); // at once, having found nothing
    CHECK(peer.found.empty());
}
