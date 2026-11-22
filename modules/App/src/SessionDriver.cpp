/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/SessionDriver.hpp>

#include <Assisi/App/StepWorld.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/NetSync/NetSession.hpp>
#include <Assisi/NetSync/NetworkConfig.hpp>
#include <Assisi/NetSync/SessionSecret.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <random>
#include <utility>

namespace Assisi::App
{
namespace
{

/// When, from its start, a search sends its queries (s). Three, because a
/// broadcast is a datagram nobody acknowledges, and a host that missed the first
/// still answers in time.
constexpr std::array<double, 3> kQueryTimes{0.0, 0.5, 1.0};

constexpr double kMillisecondsPerSecond = 1000.0;

double SteadySeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

NetSync::AccessRule RuleOf(Access access)
{
    switch (access)
    {
    case Access::Password: return NetSync::AccessRule::Password;
    case Access::Code:     return NetSync::AccessRule::Code;
    case Access::Open:
    case Access::Count_:   break;
    }
    return NetSync::AccessRule::Open;
}

JoinRefusal RefusalOf(NetSync::RejectReason reason)
{
    switch (reason)
    {
    case NetSync::RejectReason::ProtocolMismatch: return JoinRefusal::Version;
    case NetSync::RejectReason::ContentMismatch:  return JoinRefusal::Content;
    case NetSync::RejectReason::WrongSecret:      return JoinRefusal::WrongSecret;
    case NetSync::RejectReason::RetryLater:       return JoinRefusal::TooManyTries;
    case NetSync::RejectReason::ServerFull:       break;
    }
    return JoinRefusal::Full;
}

/// Whether two entries for one session differ in anything a list shows. The
/// ping is left out, or every repeat search would announce every session, and
/// so is the address, which differs by which of a host's addresses answered first.
bool SameDetails(const SessionEntry &a, const SessionEntry &b)
{
    return a.name == b.name && a.level == b.level && a.gameData == b.gameData && a.players == b.players &&
           a.maxPlayers == b.maxPlayers && a.port == b.port && a.needsSecret == b.needsSecret &&
           a.compatible == b.compatible;
}

} // namespace

std::optional<SessionAddress> ParseSessionAddress(std::string_view text)
{
    SessionAddress address;
    std::string_view port;
    if (text.starts_with('['))
    {
        // [ipv6]:port
        const std::size_t close = text.find(']');
        if (close == std::string_view::npos)
        {
            return std::nullopt;
        }
        address.host = std::string(text.substr(1, close - 1));
        const std::string_view rest = text.substr(close + 1);
        if (!rest.empty())
        {
            if (!rest.starts_with(':'))
            {
                return std::nullopt;
            }
            port = rest.substr(1);
        }
    }
    else if (std::count(text.begin(), text.end(), ':') == 1)
    {
        // ipv4:port. A bare IPv6 address has more than one colon and no port.
        const std::size_t colon = text.find(':');
        address.host = std::string(text.substr(0, colon));
        port = text.substr(colon + 1);
        if (port.empty())
        {
            return std::nullopt; // "host:" with nothing after the colon
        }
    }
    else
    {
        address.host = std::string(text);
    }

    if (address.host.empty())
    {
        return std::nullopt;
    }
    if (!port.empty())
    {
        std::uint32_t value = 0;
        const std::from_chars_result parsed = std::from_chars(port.data(), port.data() + port.size(), value);
        constexpr std::uint32_t kMaxPort = 65535;
        if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() || value == 0 || value > kMaxPort)
        {
            return std::nullopt;
        }
        address.port = static_cast<std::uint16_t>(value);
    }
    return address;
}

SessionDriver::SessionDriver(Core::EventQueue &universalEvents, SessionHooks hooks, NetSync::ReplicationConfig config,
                             std::uint16_t discoveryPort)
    : _events(universalEvents), _hooks(std::move(hooks)), _config(std::move(config)), _discoveryPort(discoveryPort)
{
}

SessionDriver::~SessionDriver() { Shutdown(); }

void SessionDriver::Update(World &world, float dt)
{
    if (world.net.TakeSearchRequest())
    {
        FindSessions();
    }
    if (std::optional<SessionRequest> request = world.net.TakeSessionRequest())
    {
        ApplyRequest(world, std::move(*request));
    }
    Advance(dt);
    AnswerQueries();
    AdvanceSearch();
    world.net.Publish(_info, _entries);
}

void SessionDriver::ApplyRequest(World &world, SessionRequest request)
{
    switch (request.kind)
    {
    case SessionRequest::Kind::Host:
        Host(world, request.host, _hooks.currentLevel ? _hooks.currentLevel() : NetSync::LevelIdentity{},
             HostRole::Player);
        break;
    case SessionRequest::Kind::JoinEntry:
        JoinEntry(world, request.entry, request.secret);
        break;
    case SessionRequest::Kind::JoinAddress:
        JoinAddress(world, request.address, request.secret);
        break;
    case SessionRequest::Kind::Leave:
    case SessionRequest::Kind::Count_:
        Leave();
        break;
    }
}

void SessionDriver::Host(World &world, const HostOptions &options, NetSync::LevelIdentity level, HostRole role)
{
    if (_phase != Phase::Idle)
    {
        Fail(HostFailure::AlreadyInSession);
        return;
    }
    if (!NetSync::AllowDirectConnections())
    {
        Fail(HostFailure::DirectConnectionsForbidden);
        return;
    }
    if (level.addressing == NetSync::LevelAddressing::None)
    {
        Fail(HostFailure::NoLevel);
        return;
    }

    NetSync::SessionAccess access;
    access.rule = RuleOf(options.access);
    if (options.access == Access::Password)
    {
        if (options.password.empty() || options.password.size() > NetSync::kMaxSecretBytes)
        {
            Fail(HostFailure::BadPassword);
            return;
        }
        access.secret = options.password;
    }
    else if (options.access == Access::Code)
    {
        access.secret = NetSync::GenerateSessionCode();
    }

    // Checked whether or not the session is listed, so making it hidden is never
    // what makes a too-long name work. A path too long for a reply is left out
    // of it: a list can show a session without its level.
    NetSync::DiscoveryReply probe;
    probe.name = options.name;
    probe.gameData = options.gameData;
    if (!NetSync::WriteDiscoveryReply(probe).has_value())
    {
        Fail(HostFailure::TooMuchData);
        return;
    }
    _role = role;
    _gameData = options.gameData;
    _level = level.path.size() <= NetSync::kMaxDiscoveryLevelBytes ? level.path : std::string{};

    // A listen server's own player takes one of the places and is not a connection.
    const std::uint32_t ownPlace = role == HostRole::Player ? 1u : 0u;
    access.maxClients = options.maxPlayers == 0 ? NetSync::SessionAccess{}.maxClients
                                                : options.maxPlayers - std::min(options.maxPlayers, ownPlace);

    _play = std::make_unique<NetworkedPlay>(world, _config);
    const std::string code = options.access == Access::Code ? access.secret : std::string{};
    if (!_play->Host(options.port, std::move(level), role, std::move(access)))
    {
        Teardown();
        Fail(HostFailure::PortUnavailable);
        return;
    }

    _info = SessionInfo{.id = NetSync::GenerateSessionId(),
                        .name = options.name,
                        .code = code,
                        .players = ownPlace,
                        .maxPlayers = options.maxPlayers,
                        .port = _play->Session().BoundPort(),
                        .visibility = options.visibility,
                        .access = options.access,
                        .hosting = true};
    _phase = Phase::Hosting;

    if (options.visibility == Visibility::Listed)
    {
        std::expected<Net::UdpSocket, std::string> responder =
            Net::UdpSocket::Open(_discoveryPort, Net::UdpPortUse::Shared);
        if (responder)
        {
            _responder.emplace(std::move(*responder));
        }
        else
        {
            // Still hosting: joinable by address, only not found by a search.
            Core::Log::Warn("Session: hosting, but not answering searches - {}.", responder.error());
        }
    }
    Core::Log::Info("Session: hosting '{}' on port {} ({}, {}).", options.name, _info->port,
                    options.visibility == Visibility::Listed ? "listed" : "hidden",
                    options.access == Access::Open ? "open" : options.access == Access::Code ? "code" : "password");
    _events.Push(SessionStarted{});
}

void SessionDriver::Join(World &world, std::string_view host, std::uint16_t port, std::string_view secret)
{
    if (_phase != Phase::Idle)
    {
        // The session already running is left as it is.
        Announce(JoinRefusal::AlreadyInSession);
        return;
    }
    if (!NetSync::AllowDirectConnections())
    {
        Announce(JoinRefusal::DirectConnectionsForbidden);
        return;
    }
    _play = std::make_unique<NetworkedPlay>(world, _config);
    if (!_play->Join(host, port, secret))
    {
        _play.reset();
        Refuse(JoinRefusal::Unreachable, /*restore=*/ false);
        return;
    }
    _info = SessionInfo{}; // a client: not hosting, and told nothing of the session but its world
    _joinSeconds = 0.f;
    _phase = Phase::Connecting;
}

void SessionDriver::JoinEntry(World &world, const SessionEntry &entry, std::string_view secret)
{
    if (!entry.id.IsValid() || entry.address.empty() || entry.port == 0)
    {
        Announce(JoinRefusal::NotFound);
        return;
    }
    const bool wasIdle = _phase == Phase::Idle;
    Join(world, entry.address, entry.port, secret);
    if (wasIdle && _phase == Phase::Connecting)
    {
        _info->id = entry.id;
        _info->name = entry.name;
    }
}

void SessionDriver::JoinAddress(World &world, std::string_view address, std::string_view secret)
{
    const std::optional<SessionAddress> parsed = ParseSessionAddress(address);
    if (!parsed)
    {
        Core::Log::Warn("Session: '{}' is not an address to join (an IP, with an optional :port).", address);
        Announce(JoinRefusal::Unreachable);
        return;
    }
    Join(world, parsed->host, parsed->port, secret);
}

void SessionDriver::Leave()
{
    switch (_phase)
    {
    case Phase::Idle:
    case Phase::Count_:
        return;
    case Phase::Hosting:
    case Phase::Connecting:
        End(EndReason::Left, /*restore=*/ false);
        return;
    case Phase::Admitting:
    case Phase::Live:
        End(EndReason::Left, /*restore=*/ true);
        return;
    }
}

void SessionDriver::Shutdown()
{
    Teardown();
    _searcher.reset();
    _searching = false;
}

void SessionDriver::Advance(float dt)
{
    switch (_phase)
    {
    case Phase::Hosting:
        _info->players = PlayerCount();
        return;
    case Phase::Connecting:
        AdvanceConnecting(dt);
        return;
    case Phase::Admitting:
        AdvanceAdmitting();
        return;
    case Phase::Live:
        if (!_play->Session().IsActive())
        {
            End(_play->Session().LastEnd() == NetSync::SessionEnd::HostLeft ? EndReason::HostLeft
                                                                           : EndReason::ConnectionLost,
                /*restore=*/ true);
        }
        return;
    case Phase::Idle:
    case Phase::Count_:
        return;
    }
}

void SessionDriver::AdvanceConnecting(float dt)
{
    NetSync::NetSession &session = _play->Session();
    if (!session.IsActive())
    {
        RefuseFromSession(/*restore=*/ false);
        return;
    }
    if (!session.IsAwaitingLevel())
    {
        _joinSeconds += dt;
        if (_joinSeconds >= kJoinTimeoutSeconds)
        {
            Core::Log::Warn("Session: no answer from the host after {} s.", kJoinTimeoutSeconds);
            Refuse(JoinRefusal::Unreachable, /*restore=*/ false);
        }
        return;
    }

    // Here, at the safe point: building a level frees and loads assets.
    const NetSync::LevelIdentity level = session.Handshake()->level;
    const std::expected<void, std::string> built =
        _hooks.buildJoinedLevel ? _hooks.buildJoinedLevel(level) : std::unexpected(std::string("nothing builds it"));
    if (!built)
    {
        Core::Log::Error("Session: cannot join - the host's level '{}': {}", level.path, built.error());
        session.AbortJoin(built.error());
        Refuse(JoinRefusal::Level, /*restore=*/ true);
        _lastProblem = "join refused: the host's level '" + level.path + "': " + built.error();
        return;
    }
    _play->ConfirmLevelReady();
    _phase = Phase::Admitting;
}

void SessionDriver::AdvanceAdmitting()
{
    NetSync::NetSession &session = _play->Session();
    if (!session.IsActive())
    {
        RefuseFromSession(/*restore=*/ true);
        return;
    }
    // The host's first snapshot is the only word that it let this machine in:
    // the content check comes after the level is built, and passes silently.
    if (session.Client() != nullptr && session.Client()->SnapshotsApplied() > 0)
    {
        _phase = Phase::Live;
        Core::Log::Info("Session: joined.");
        _events.Push(SessionJoined{});
    }
}

void SessionDriver::RefuseFromSession(bool restore)
{
    const NetSync::NetSession &session = _play->Session();
    const std::optional<NetSync::RejectReason> refusal = session.LastRefusal();
    if (session.LastEnd() == NetSync::SessionEnd::Refused && refusal.has_value())
    {
        Refuse(RefusalOf(*refusal), restore);
        return;
    }
    Refuse(JoinRefusal::Unreachable, restore);
}

void SessionDriver::Refuse(JoinRefusal reason, bool restore)
{
    Teardown();
    if (restore && _hooks.restoreAlone)
    {
        _hooks.restoreAlone();
    }
    Announce(reason);
}

void SessionDriver::Announce(JoinRefusal reason)
{
    _lastProblem = "join refused: " + std::string(Name(reason));
    Core::Log::Warn("Session: {}.", _lastProblem);
    _events.Push(SessionRefused{.reason = reason});
}

void SessionDriver::Fail(HostFailure reason)
{
    _lastProblem = "not hosting: " + std::string(Name(reason));
    Core::Log::Warn("Session: {}.", _lastProblem);
    _events.Push(SessionFailed{.reason = reason});
}

void SessionDriver::End(EndReason reason, bool restore)
{
    if (reason != EndReason::Left)
    {
        _lastProblem = "session ended: " + std::string(Name(reason));
    }
    Core::Log::Info("Session: ended - {}.", Name(reason));
    Teardown();
    if (restore && _hooks.restoreAlone)
    {
        _hooks.restoreAlone();
    }
    _events.Push(SessionEnded{.reason = reason});
}

void SessionDriver::Teardown()
{
    // A client's mirrors and a host's connections go with the session; the
    // world's own player is played alone from the next step.
    _play.reset();
    _responder.reset();
    _info.reset();
    _gameData.clear();
    _level.clear();
    _phase = Phase::Idle;
}

std::uint32_t SessionDriver::PlayerCount() const
{
    if (!_play)
    {
        return 0;
    }
    const std::uint32_t own = _role == HostRole::Player ? 1u : 0u;
    return own + static_cast<std::uint32_t>(_play->Session().ReadyClients().size());
}

NetSync::DiscoveryReply SessionDriver::Reply(std::uint32_t nonce) const
{
    constexpr std::uint32_t kMaxReported = 0xFFFF;
    NetSync::DiscoveryReply reply;
    reply.id = _info ? _info->id : Core::SessionId{};
    reply.protocolHash = NetSync::NetProtocolHash();
    reply.name = _info ? _info->name : std::string{};
    reply.levelPath = _level;
    reply.gameData = _gameData;
    reply.nonce = nonce;
    reply.gamePort = _info ? _info->port : std::uint16_t{0};
    reply.players = static_cast<std::uint16_t>(std::min(PlayerCount(), kMaxReported));
    reply.maxPlayers = static_cast<std::uint16_t>(std::min(_info ? _info->maxPlayers : 0u, kMaxReported));
    reply.needsSecret = _info && _info->access != Access::Open;
    return reply;
}

void SessionDriver::AnswerQueries()
{
    if (!_responder || _phase != Phase::Hosting)
    {
        return;
    }
    while (std::optional<Net::UdpDatagram> datagram = _responder->Receive())
    {
        // Only the local network, so this host is never a reflector aimed at
        // someone else; and only a query this build reads.
        if (!NetSync::IsLocalNetworkAddress(datagram->from.address))
        {
            continue;
        }
        const std::optional<NetSync::DiscoveryQuery> query = NetSync::ReadDiscoveryQuery(datagram->bytes);
        if (!query)
        {
            continue;
        }
        const std::expected<std::vector<std::byte>, NetSync::DiscoveryError> reply =
            NetSync::WriteDiscoveryReply(Reply(query->nonce));
        if (reply)
        {
            (void)_responder->SendTo(datagram->from, *reply);
        }
    }
}

void SessionDriver::FindSessions()
{
    if (!NetSync::AllowDirectConnections())
    {
        if (!_warnedNoDirect)
        {
            Core::Log::Warn("Session: no LAN search - allowDirectConnections is off in config/network.json.");
            _warnedNoDirect = true;
        }
        _events.Push(SessionSearchDone{});
        return;
    }
    if (!_searcher)
    {
        std::expected<Net::UdpSocket, std::string> searcher = Net::UdpSocket::Open(0, Net::UdpPortUse::Own);
        if (!searcher)
        {
            Core::Log::Warn("Session: cannot search - {}.", searcher.error());
            _events.Push(SessionSearchDone{});
            return;
        }
        _searcher.emplace(std::move(*searcher));
    }
    std::random_device device;
    Search search;
    search.broadcasts = Net::UdpSocket::BroadcastAddresses();
    search.startedSeconds = SteadySeconds();
    search.nonce = device();
    _search = std::move(search);
    _searching = true;
}

void SessionDriver::AdvanceSearch()
{
    if (!_searcher)
    {
        return;
    }
    ReadReplies();
    if (!_searching)
    {
        return;
    }
    const double elapsed = SteadySeconds() - _search.startedSeconds;
    while (_search.queriesSent < kQueryTimes.size() && elapsed >= kQueryTimes[_search.queriesSent])
    {
        SendQuery();
    }
    if (elapsed >= kSearchSeconds)
    {
        EndSearch();
    }
}

void SessionDriver::SendQuery()
{
    const std::vector<std::byte> query = NetSync::WriteDiscoveryQuery(NetSync::DiscoveryQuery{.nonce = _search.nonce});
    for (const std::string &broadcast : _search.broadcasts)
    {
        (void)_searcher->SendTo(Net::UdpEndpoint{.address = broadcast, .port = _discoveryPort}, query);
    }
    _search.lastQuerySeconds = SteadySeconds();
    ++_search.queriesSent;
}

void SessionDriver::ReadReplies()
{
    while (std::optional<Net::UdpDatagram> datagram = _searcher->Receive())
    {
        // Late replies to a finished search, and replies to someone else's, are
        // not this search's: its list is what answered it.
        const std::optional<NetSync::DiscoveryReply> reply = NetSync::ReadDiscoveryReply(datagram->bytes);
        if (reply && _searching && reply->nonce == _search.nonce)
        {
            TakeReply(*reply, datagram->from.address);
        }
    }
}

void SessionDriver::TakeReply(const NetSync::DiscoveryReply &reply, const std::string &from)
{
    SessionEntry entry{.id = reply.id,
                       .name = reply.name,
                       .address = from,
                       .level = reply.levelPath,
                       .gameData = reply.gameData,
                       .pingMs = static_cast<std::int32_t>((SteadySeconds() - _search.lastQuerySeconds) *
                                                           kMillisecondsPerSecond),
                       .players = reply.players,
                       .maxPlayers = reply.maxPlayers,
                       .port = reply.gamePort,
                       .needsSecret = reply.needsSecret,
                       .compatible = reply.protocolHash == NetSync::NetProtocolHash()};
    _search.answered.insert(entry.id);

    const std::vector<SessionEntry>::iterator known =
        std::find_if(_entries.begin(), _entries.end(), [&entry](const SessionEntry &e) { return e.id == entry.id; });
    if (known == _entries.end())
    {
        _entries.push_back(entry);
        _events.Push(SessionFound{.entry = std::move(entry)});
        return;
    }
    const bool changed = !SameDetails(*known, entry);
    entry.address = known->address;
    *known = entry;
    if (changed)
    {
        _events.Push(SessionFound{.entry = std::move(entry)});
    }
}

void SessionDriver::EndSearch()
{
    const std::unordered_set<Core::SessionId> &answered = _search.answered;
    std::vector<Core::SessionId> lost;
    for (const SessionEntry &entry : _entries)
    {
        if (!answered.contains(entry.id))
        {
            lost.push_back(entry.id);
        }
    }
    std::erase_if(_entries, [&answered](const SessionEntry &entry) { return !answered.contains(entry.id); });
    for (const Core::SessionId id : lost)
    {
        _events.Push(SessionLost{.id = id});
    }
    _searching = false;
    _events.Push(SessionSearchDone{});
}

std::uint64_t SessionDriver::BeginStep(std::uint64_t localTick, bool stepping)
{
    return _play ? _play->BeginStep(localTick, stepping) : localTick;
}

void SessionDriver::StepWorld(const SystemContext &ctx)
{
    if (_play)
    {
        _play->StepWorld(ctx);
        return;
    }
    App::StepWorld(ctx);
}

void SessionDriver::EndStep()
{
    if (_play)
    {
        _play->EndStep();
    }
}

void SessionDriver::SmoothView(float dt)
{
    if (_play)
    {
        _play->Session().SmoothView(dt);
    }
}

double SessionDriver::TimeScale() const { return _play ? _play->TimeScale() : 1.0; }

} // namespace Assisi::App
