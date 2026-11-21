/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestSessionHandshake.cpp
/// @brief Who a host lets in: secrets, room, the limiter, and how a refusal ends.
///
/// Real UDP on this machine, through two NetSessions, as a game drives them.

#include <doctest/doctest.h>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Net/NetTransport.hpp>
#include <Assisi/NetSync/NetProtocol.hpp>
#include <Assisi/NetSync/NetSession.hpp>
#include <Assisi/NetSync/NetworkConfig.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace Assisi;
using namespace Assisi::NetSync;

namespace
{

/// How long a test waits for something to happen over the wire.
constexpr std::chrono::milliseconds kWireTimeout{5000};

/// One machine: its scene and its session.
struct Machine
{
    ECS::Scene scene;
    NetSession session{scene};
    std::uint64_t tick = 0;
};

/// One poll and step of every machine, then a moment for the wire.
void PumpOnce(const std::vector<Machine *> &machines)
{
    for (Machine *machine : machines)
    {
        machine->session.Poll();
        (void)machine->session.BeginStep(++machine->tick);
        machine->session.Tick();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
}

/// Pumps every machine until @p done, or the timeout.
bool PumpUntil(const std::vector<Machine *> &machines, const std::function<bool ()> &done)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kWireTimeout;
    while (std::chrono::steady_clock::now() < deadline && !done())
    {
        PumpOnce(machines);
    }
    return done();
}

/// Whether @p client has been sent the host's hello.
bool HelloArrived(Machine &client)
{
    const ServerHello *hello = client.session.Handshake();
    return hello != nullptr && hello->clientId.IsValid();
}

/// Hosts on any free port with @p access, ready to answer joiners.
std::uint16_t Host(Machine &host, SessionAccess access)
{
    REQUIRE(host.session.Host(0, LevelIdentity{}, std::move(access)));
    host.session.SetContentSetHash(0);
    const std::uint16_t port = host.session.BoundPort();
    REQUIRE(port != 0);
    return port;
}

/// Joins @p port on this machine with @p secret.
void Join(Machine &client, std::uint16_t port, std::string_view secret)
{
    REQUIRE(client.session.Join("127.0.0.1", port, /*deferHandshake=*/ false, secret));
    client.session.SetContentSetHash(0);
}

/// Pumps until @p client is synchronized or has gone Offline; true when synchronized.
bool Joined(Machine &host, Machine &client)
{
    (void)PumpUntil({&host, &client},
                    [&] { return client.session.Stats().synchronized || !client.session.IsActive(); });
    INFO("client: ", client.session.StatusText(), " end ", static_cast<std::int32_t>(client.session.LastEnd()));
    return client.session.IsActive() && client.session.Stats().synchronized;
}

/// Pumps until @p client has gone Offline, and returns why it was refused.
std::optional<RejectReason> Refusal(Machine &host, Machine &client)
{
    // Watching for a hello while it waits: a refused joiner must never get one.
    bool helloSeen = false;
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kWireTimeout;
    while (client.session.IsActive() && std::chrono::steady_clock::now() < deadline)
    {
        PumpOnce({&host, &client});
        helloSeen = helloSeen || HelloArrived(client);
    }
    CHECK_FALSE(client.session.IsActive());
    CHECK_FALSE(helloSeen);
    CHECK(client.session.LastEnd() == SessionEnd::Refused);
    return client.session.LastRefusal();
}

/// What a raw connection standing in for another build heard.
struct RawOutcome
{
    std::optional<std::uint32_t> reason; ///< The Reject's, once one arrives.
    bool closed = false;
};

/// Reads what arrived on @p transport into @p outcome.
void ReadRaw(Net::NetTransport &transport, RawOutcome &outcome)
{
    std::vector<Net::NetEvent> events;
    transport.Poll(events);
    for (const Net::NetEvent &event : events)
    {
        if (event.type == Net::NetEvent::Type::Message)
        {
            Core::BitReader reader(event.payload);
            if (ReadMessageType(reader) == MessageType::Reject)
            {
                outcome.reason = reader.ReadBits(8);
            }
        }
        outcome.closed = outcome.closed || event.type == Net::NetEvent::Type::Disconnected;
    }
}

SessionAccess Password(std::string password, std::uint32_t maxClients = SessionAccess{}.maxClients)
{
    return SessionAccess{.secret = std::move(password), .maxClients = maxClients, .rule = AccessRule::Password};
}

} // namespace

TEST_CASE("an open session lets anyone in, whatever they send")
{
    Machine host;
    Machine client;
    const std::uint16_t port = Host(host, SessionAccess{});
    Join(client, port, "not asked for");
    CHECK(Joined(host, client));
}

TEST_CASE("a password session refuses the wrong password, closes the connection, and says nothing more")
{
    Machine host;
    Machine client;
    const std::uint16_t port = Host(host, Password("hunter2"));
    Join(client, port, "hunter3");
    CHECK(Refusal(host, client) == RejectReason::WrongSecret);
    // The host closed it: nothing of the joiner is left on either side.
    CHECK(PumpUntil({&host}, [&] { return host.session.Clients().empty(); }));
    CHECK(host.session.ReadyClients().empty());
}

TEST_CASE("a password session refuses no password at all")
{
    Machine host;
    Machine client;
    const std::uint16_t port = Host(host, Password("hunter2"));
    Join(client, port, "");
    CHECK(Refusal(host, client) == RejectReason::WrongSecret);
}

TEST_CASE("a password session lets in the right password")
{
    Machine host;
    Machine client;
    const std::uint16_t port = Host(host, Password("hunter2"));
    Join(client, port, "hunter2");
    CHECK(Joined(host, client));
}

TEST_CASE("a code session takes the code however it is typed")
{
    Machine host;
    Machine client;
    const std::uint16_t port =
        Host(host, SessionAccess{.secret = "K7MQ4X", .rule = AccessRule::Code});
    Join(client, port, "k7m-q4x ");
    CHECK(Joined(host, client));
}

TEST_CASE("a full session refuses the next joiner, and only one with the right secret learns it is full")
{
    Machine host;
    Machine first;
    Machine second;
    Machine guesser;
    const std::uint16_t port = Host(host, Password("hunter2", /*maxClients=*/ 1));

    Join(first, port, "hunter2");
    REQUIRE(Joined(host, first));

    Join(second, port, "hunter2");
    CHECK(Refusal(host, second) == RejectReason::ServerFull);

    Join(guesser, port, "wrong");
    CHECK(Refusal(host, guesser) == RejectReason::WrongSecret);

    // The one already in stays in.
    CHECK(first.session.IsActive());
}

TEST_CASE("a session with room for nobody refuses everyone")
{
    Machine host;
    Machine client;
    const std::uint16_t port = Host(host, SessionAccess{.maxClients = 0});
    Join(client, port, "");
    CHECK(Refusal(host, client) == RejectReason::ServerFull);
}

TEST_CASE("an address that keeps guessing waits, and even the right secret is refused meanwhile")
{
    Machine host;
    const std::uint16_t port = Host(host, Password("hunter2"));
    for (std::uint32_t i = 0; i < kAttemptsBeforeCooldown; ++i)
    {
        Machine guesser;
        Join(guesser, port, "wrong");
        CHECK(Refusal(host, guesser) == RejectReason::WrongSecret);
    }
    Machine late;
    Join(late, port, "hunter2");
    CHECK(Refusal(host, late) == RejectReason::RetryLater);
}

TEST_CASE("a build that frames the handshake differently is refused as a mismatch, before its secret is read")
{
    Machine host;
    const std::uint16_t port = Host(host, Password("hunter2"));

    // A raw connection standing in for another build: a ClientAuth with a
    // protocol hash this build does not have, and the right password after it.
    std::unique_ptr<Net::NetTransport> other = Net::NetTransport::Create().value();
    const Net::ConnectionId connection = other->Connect("127.0.0.1", port);
    REQUIRE(connection != Net::InvalidConnection);
    Core::BitWriter writer;
    WriteMessageType(MessageType::ClientAuth, writer);
    WriteClientAuth(ClientAuth{.protocolHash = NetProtocolHash() ^ 1u, .secret = "hunter2"}, writer);
    REQUIRE(other->Send(connection, writer.Data(), Net::SendMode::Reliable, Net::Lane::Control));

    RawOutcome outcome;
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kWireTimeout;
    while (!outcome.closed && std::chrono::steady_clock::now() < deadline)
    {
        PumpOnce({&host});
        ReadRaw(*other, outcome);
    }
    CHECK(outcome.closed);
    REQUIRE(outcome.reason.has_value());
    CHECK(*outcome.reason == static_cast<std::uint32_t>(RejectReason::ProtocolMismatch));
}

TEST_CASE("a client is told when the host leaves, rather than that the connection dropped")
{
    Machine host;
    Machine client;
    const std::uint16_t port = Host(host, SessionAccess{});
    Join(client, port, "");
    REQUIRE(Joined(host, client));

    host.session.Disconnect();
    CHECK(PumpUntil({&client}, [&] { return !client.session.IsActive(); }));
    CHECK(client.session.LastEnd() == SessionEnd::HostLeft);
}

TEST_CASE("with direct connections turned off, nothing hosts or joins by address")
{
    NetworkConfig off;
    off.allowDirectConnections = false;
    ApplyNetworkConfig(off);
    Machine machine;
    CHECK_FALSE(machine.session.Host(0));
    CHECK_FALSE(machine.session.IsActive());
    CHECK(machine.session.BoundPort() == 0);
    CHECK_FALSE(machine.session.Join("127.0.0.1", Net::kAnyPortFirst));
    CHECK(machine.session.LastError().find("allowDirectConnections") != std::string::npos);
    ApplyNetworkConfig(NetworkConfig{});
    CHECK(AllowDirectConnections());
}
