/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SessionDriver.hpp
/// @brief Hosting, joining, leaving and browsing sessions, written once for every program.
///
/// The game, the editor and the headless server each own one. It applies what a
/// world's systems asked of `ctx.net`, runs the join (connect, build the host's
/// level, wait to be let in), answers and makes LAN searches, raises the
/// session events, and wraps every fixed step in its session. What differs
/// between programs is three things, handed in as hooks: which level this
/// machine would host, how it builds the host's level when it joins, and how
/// it goes back to playing alone when a joined session ends.
///
/// @code
/// SessionDriver sessions{universalEvents, hooks};
/// // the frame's safe point:
/// sessions.Update(world, dt);
/// // each fixed step:
/// const std::uint64_t tick = sessions.BeginStep(GetSimTick(), /*stepping=*/ true);
/// sessions.StepWorld(contextAt(tick));
/// sessions.EndStep();
/// @endcode

#include <Assisi/App/NetworkedPlay.hpp>
#include <Assisi/App/SessionTypes.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Net/UdpSocket.hpp>
#include <Assisi/NetSync/NetProtocol.hpp>
#include <Assisi/NetSync/ReplicationConfig.hpp>
#include <Assisi/NetSync/SessionDiscovery.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Assisi::Core
{
class EventQueue;
}

namespace Assisi::App
{

/// @brief How long a join waits for the host's hello before calling it
/// unreachable (s). Long enough for a slow machine to answer, short enough that
/// a player is not left wondering.
inline constexpr float kJoinTimeoutSeconds = 10.f;

/// @brief How long a LAN search listens for answers (s).
inline constexpr double kSearchSeconds = 2.0;

/// @brief The three things a program does its own way. See the file comment.
struct SessionHooks
{
    /// The level this machine would host now. LevelAddressing::None refuses
    /// hosting with HostFailure::NoLevel.
    std::function<NetSync::LevelIdentity ()> currentLevel;
    /// Builds @p level, the host's, into the world: load it, then strip the
    /// entities the host owns, which arrive as mirrors. An error is the reason,
    /// for the log; the join is refused with JoinRefusal::Level.
    std::function<std::expected<void, std::string>(const NetSync::LevelIdentity &level)> buildJoinedLevel;
    /// A joined session ended after the world became the host's: put back a
    /// world this machine plays alone. Not called for a host, which keeps its world.
    std::function<void ()> restoreAlone;
};

/// @brief An address as typed: the host, and the port when one was written.
struct SessionAddress
{
    std::string host;
    std::uint16_t port = kDefaultSessionPort;
};

/// @brief Reads `192.168.1.20`, `192.168.1.20:27015`, `::1` or `[::1]:27015`.
/// Nothing when it has no host or a port that is not a number from 1 to 65535.
[[nodiscard]] std::optional<SessionAddress> ParseSessionAddress(std::string_view text);

class SessionDriver
{
public:
    /// @param universalEvents Where the session events are raised.
    /// @param discoveryPort The port listed hosts answer searches on; a test
    ///   picks its own, so a game running on the machine does not answer it.
    SessionDriver(Core::EventQueue &universalEvents, SessionHooks hooks, NetSync::ReplicationConfig config = {},
                  std::uint16_t discoveryPort = NetSync::kLanDiscoveryPort);
    ~SessionDriver();

    SessionDriver(const SessionDriver &) = delete;
    SessionDriver &operator=(const SessionDriver &) = delete;

    /// @brief The frame's safe point: applies what @p world's systems asked of
    /// `ctx.net`, moves a join along (building the host's level here, since it
    /// frees and loads assets), answers and makes searches, raises the events,
    /// and tells `ctx.net` the session's state.
    void Update(World &world, float dt);

    /// @brief Hosts @p level from @p world as @p role, as `ctx.net.Host` does for
    /// a listen server. For a program that names its own level and role: the
    /// editor's temporary play-in-editor level, a dedicated server.
    void Host(World &world, const HostOptions &options, NetSync::LevelIdentity level, HostRole role);

    /// @brief Joins @p host on @p port with the session's @p secret.
    void Join(World &world, std::string_view host, std::uint16_t port, std::string_view secret);

    /// @brief Leaves: a host ends the session for everyone, a client plays alone
    /// again. SessionEnded{Left} follows. Nothing when not in a session.
    void Leave();

    /// @brief Starts a LAN search; see Network::FindSessions.
    void FindSessions();

    /// @brief Ends the session without an event or a hook, for a program
    /// shutting down or putting its world back by itself (the editor's Stop).
    void Shutdown();

    /// @brief The start of a fixed step; see NetworkedPlay::BeginStep. The
    /// local tick when there is no session.
    [[nodiscard]] std::uint64_t BeginStep(std::uint64_t localTick, bool stepping);

    /// @brief One fixed step of @p ctx's world, inside the session if there is one.
    void StepWorld(const SystemContext &ctx);

    /// @brief The end of a fixed step; see NetworkedPlay::EndStep.
    void EndStep();

    /// @brief Smooths mirrors once a frame, after the physics writeback; see
    /// NetSync::NetSession::SmoothView.
    void SmoothView(float dt);

    /// @brief How fast fixed steps run against real time: a client's clock
    /// steers its lead with it. 1 with no session.
    [[nodiscard]] double TimeScale() const;

    /// @brief Whether this machine is hosting or in any stage of a join.
    [[nodiscard]] bool InSession() const { return _phase != Phase::Idle; }

    /// @brief The session, for a program's debug panel. Null when not in one.
    [[nodiscard]] NetworkedPlay *Play() { return _play.get(); }
    [[nodiscard]] const NetworkedPlay *Play() const { return _play.get(); }

    /// @brief The session this machine is in, as `ctx.net.Session()` gives it.
    [[nodiscard]] const std::optional<SessionInfo> &Info() const { return _info; }

    /// @brief Every session the searches so far know of.
    [[nodiscard]] const std::vector<SessionEntry> &Sessions() const { return _entries; }

    /// @brief Why the last host, join or session went wrong, in words, for a
    /// debug panel. Empty until something has.
    [[nodiscard]] const std::string &LastProblem() const { return _lastProblem; }

private:
    enum class Phase : std::uint8_t
    {
        Idle,
        Hosting,
        Connecting, ///< Waiting for the host's hello.
        Admitting,  ///< The host's level is built; waiting to be let in.
        Live,
        Count_
    };

    /// One LAN search in progress.
    struct Search
    {
        std::vector<std::string> broadcasts; ///< Where each query goes.
        std::unordered_set<Core::SessionId> answered;
        double startedSeconds = 0.0;
        double lastQuerySeconds = 0.0;
        std::uint32_t nonce = 0;
        std::uint32_t queriesSent = 0;
    };

    void ApplyRequest(World &world, SessionRequest request);
    void JoinEntry(World &world, const SessionEntry &entry, std::string_view secret);
    void JoinAddress(World &world, std::string_view address, std::string_view secret);

    /// Moves a join along, and notices a session that ended by itself.
    void Advance(float dt);
    void AdvanceConnecting(float dt);
    void AdvanceAdmitting();

    /// Refuses the join in progress for why its session went Offline.
    void RefuseFromSession(bool restore);
    /// Ends the join in progress, putting back a world played alone when
    /// @p restore, and announces why.
    void Refuse(JoinRefusal reason, bool restore);
    /// Only announces a refusal, for a join that never started.
    void Announce(JoinRefusal reason);
    void Fail(HostFailure reason);
    void End(EndReason reason, bool restore);

    /// Drops the session, the responder, and what Info() says.
    void Teardown();

    /// Answers every query waiting on the responder.
    void AnswerQueries();
    [[nodiscard]] NetSync::DiscoveryReply Reply(std::uint32_t nonce) const;

    void AdvanceSearch();
    void SendQuery();
    void ReadReplies();
    void TakeReply(const NetSync::DiscoveryReply &reply, const std::string &from);
    void EndSearch();

    /// Everyone in the session: the ready clients, and the host when it plays.
    [[nodiscard]] std::uint32_t PlayerCount() const;

    Core::EventQueue &_events;
    SessionHooks _hooks;
    NetSync::ReplicationConfig _config;

    std::unique_ptr<NetworkedPlay> _play;
    std::optional<SessionInfo> _info;
    /// The hosted session's game data, which every reply carries.
    GameData _gameData;
    std::string _level; ///< The hosted level's path, for a reply.
    std::string _lastProblem;
    std::vector<SessionEntry> _entries;

    std::optional<Net::UdpSocket> _responder; ///< A listed host's.
    std::optional<Net::UdpSocket> _searcher;
    Search _search; ///< The search in progress, while _searching.

    float _joinSeconds = 0.f;
    std::uint16_t _discoveryPort;
    Phase _phase = Phase::Idle;
    HostRole _role = HostRole::Player;
    bool _searching = false;
    bool _warnedNoDirect = false; ///< A search refused for direct connections being off says so once.
};

} // namespace Assisi::App
