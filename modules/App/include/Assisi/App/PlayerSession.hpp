/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PlayerSession.hpp
/// @brief Players in a network session: a character for each player that joins,
///        each player's controls carried to the host, and which client this
///        machine is.
///
/// Built only with networking. Everything that moves a player's character is
/// in PlayerSystems.hpp and runs the same in a session or out of one; this is
/// only what a session adds to it. A host runs it once per fixed step, before
/// the step's systems; so does a client.

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/NetSync/InputCommand.hpp>
#include <Assisi/NetSync/NetProtocol.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Assisi::NetSync
{
class NetSession;
} // namespace Assisi::NetSync

namespace Assisi::App
{

/// @brief Tells the network protocol which button each bit of a player's
/// controls is: the engine's actions and the game's.
///
/// Every process that hosts or joins calls it first, through PlayerSession or on
/// its own, or it cannot pair with one that did.
void PublishPlayerActions();

/// @brief How many fixed steps a remote player's movement and held buttons
/// carry on when their commands stop arriving, before they let go.
///
/// A command a step late should not jerk a running player to a stop and back;
/// a connection that has gone quiet should not leave one running into a wall.
inline constexpr std::uint32_t kInputHoldTicks = 6;

/// @brief Hands a remote player's character the controls the host steps it on
/// this tick: @p command when one came, otherwise what it held, released once
/// @p starvedTicks, the steps in a row with no command, passes kInputHoldTicks.
///
/// The one rule for it, so a client stepping a tick again steps it on what the
/// host did.
void RelayHeldCommand(PlayerInput &controls, const NetSync::InputCommand *command, std::uint32_t starvedTicks);

/// @brief A world's players, for one network session.
class PlayerSession
{
public:
    /// @brief Readies @p session to play @p world. Call it after making the
    /// session and before Host() or Join().
    ///
    /// Publishes the player actions, and has a leaving player's character
    /// despawned whole: every member of its blueprint, camera and all. A host
    /// gives itself a character only when @p hostPlays; a dedicated server
    /// does not.
    void Attach(NetSync::NetSession &session, World &world, bool hostPlays);

    /// @brief This fixed step's part, after the session has polled and before
    /// the world's systems run.
    ///
    /// Tells @p world which client this machine is. A host gives every player a
    /// character and hands each the controls its client sent for @p simTick; a
    /// client takes its own player's controls, to send.
    void BeginStep(NetSync::NetSession &session, World &world, std::uint64_t simTick);

    /// @brief The session is over: @p world is played alone again.
    void Detach(World &world);

    /// @brief The command a client sends for this step, or null when it has no
    /// player yet or is not a client.
    [[nodiscard]] const NetSync::InputCommand *LocalCommand() const;

    // A client stepping ticks again steps its player on what the host applied
    // for each, from the controls it held after the tick it went back to. The
    // controls this frame has gathered for the next live step wait meanwhile.

    /// @brief Remembers this machine's player's controls as the step of
    /// @p tick left them. Call after every step a client takes, live or replayed.
    void RecordControls(World &world, std::uint64_t tick);

    /// @brief Starts stepping ticks again from @p tick: sets this frame's
    /// controls aside and puts back the ones the step of @p tick left.
    void BeginReplay(World &world, std::uint64_t tick);

    /// @brief Hands this machine's player the controls the host stepped it on
    /// for a tick stepped again: @p command, or what it held after
    /// @p starvedTicks steps without one, as RelayHeldCommand has it.
    void ReplayCommand(World &world, const NetSync::InputCommand *command, std::uint32_t starvedTicks);

    /// @brief Gives this frame's controls back, for the live step.
    void EndReplay(World &world);

private:
    /// This machine's player and its controls, or NullEntity and null.
    [[nodiscard]] static std::pair<ECS::Entity, PlayerInput *> LocalControls(World &world);

    /// One tick's controls, as the step of it left them.
    struct RecordedControls
    {
        std::uint64_t tick = 0; ///< 0 for none yet.
        PlayerInput controls;
    };

    void GivePlayers(NetSync::NetSession &session, World &world);
    void RelayControls(NetSync::NetSession &session, World &world, std::uint64_t simTick);
    void CaptureLocalControls(World &world);

    /// The client ids that found no character to take, so each is said once.
    std::unordered_set<std::uint32_t> _warned;

    /// Per client id, how many steps in a row brought no command.
    std::unordered_map<std::uint32_t, std::uint32_t> _starved;

    std::optional<NetSync::InputCommand> _local;

    /// The controls each recent step left, by tick, as far back as a client
    /// keeps anything it predicted.
    std::array<RecordedControls, NetSync::kPredictionHistoryTicks> _recordedControls{};

    /// The controls gathered for the next live step, set aside by BeginReplay.
    std::optional<PlayerInput> _liveControls;

    /// Whether the host is a player too. See Attach.
    bool _hostPlays = true;
};

} // namespace Assisi::App
