/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file NetworkedPlay.hpp
/// @brief A world played over the network: its session, its players, and the
///        one order everything around a fixed step happens in.
///
/// Built only with networking. Every program that hosts or joins drives a
/// session through this, so the order of hosting, the content set, the players
/// and the per-step calls is written once. What differs between programs stays
/// with them: how a joining program builds the host's level, and what it draws.
///
/// @code
/// NetworkedPlay play{world};
/// play.Host(port, level, HostRole::Player);
/// // each fixed step:
/// const std::uint64_t tick = play.BeginStep(GetSimTick(), /*stepping=*/ true);
/// StepWorld(contextAt(tick));
/// play.EndStep();
/// @endcode

#include <Assisi/App/PlayerSession.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/NetSync/NetSession.hpp>
#include <Assisi/NetSync/ReplicationConfig.hpp>

#include <cstdint>
#include <string_view>

namespace Assisi::App
{

/// @brief Whether the machine that hosts plays too.
enum class HostRole : std::uint8_t
{
    Player,    ///< A listen server: the person hosting has a character of their own.
    Dedicated, ///< A dedicated server: only the players who join have characters.
};

class NetworkedPlay
{
public:
    /// @param world The world played. Must outlive this; its scene and physics
    ///   are what the session replicates from or into.
    explicit NetworkedPlay(World &world, NetSync::ReplicationConfig config = {});

    /// Leaves the session: a client's mirrors go with it, and the world is
    /// played alone again.
    ~NetworkedPlay();

    NetworkedPlay(const NetworkedPlay &) = delete;
    NetworkedPlay &operator=(const NetworkedPlay &) = delete;

    /// @brief Hosts on @p port, telling joiners to load @p level.
    ///
    /// The content set is built and applied before this returns, so a blueprint
    /// spawned on the first step, such as the host's own player, goes out as an
    /// instance.
    /// @return false with Session().LastError() set if the port could not be bound.
    [[nodiscard]] bool Host(std::uint16_t port, NetSync::LevelIdentity level, HostRole role);

    /// @brief Joins the host at @p address. The handshake waits for
    /// ConfirmLevelReady(), once the caller has built the level the host names
    /// (Session().Handshake()).
    /// @return false with Session().LastError() set if the connection could not start.
    [[nodiscard]] bool Join(std::string_view address, std::uint16_t port);

    /// @brief The start of a fixed step: reads the wire, decides which tick this
    /// step is, and, when @p stepping, gives the world's players their controls.
    ///
    /// Call every fixed step, stepping or not: the wire is read either way, so a
    /// paused or joining machine still hears from the host. Returns the tick every
    /// system of the step runs at, the same on every machine.
    std::uint64_t BeginStep(std::uint64_t localTick, bool stepping);

    /// @brief The end of a fixed step, after everything that writes in it: puts
    /// mirrored bodies the step woke back to sleep, then sends the step's
    /// snapshot (host) or command (client).
    void EndStep();

    /// @brief Answers the host's handshake. Call once the level it named is built.
    void ConfirmLevelReady();

    /// @brief How fast fixed steps should run against real time: a client's
    /// clock steers its lead with it. 1 for a host.
    [[nodiscard]] double TimeScale() const { return _session.TimeScale(); }

    [[nodiscard]] NetSync::NetSession &Session() { return _session; }
    [[nodiscard]] const NetSync::NetSession &Session() const { return _session; }

private:
    World &_world;
    NetSync::NetSession _session;
    PlayerSession _players;
};

} // namespace Assisi::App
