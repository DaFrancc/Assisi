/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file BodyOwnership.hpp
/// @brief Which player's machine simulates each dynamic body: the host's rules
/// for handing bodies out and taking them back, apart from the scene and the
/// physics world they are read from.

#include <Assisi/NetSync/NetProtocol.hpp>
#include <Assisi/NetSync/ReplicationConfig.hpp>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace Assisi::NetSync
{

/// @brief How near one player's character is to a group of bodies in a tick.
struct PlayerReach
{
    /// From the nearest of the player's characters' feet to the nearest
    /// body's centre (m).
    float distance = 0.f;
    std::uint32_t client = 0;
    /// Whether one of the player's characters touches one of the bodies.
    bool touching = false;
};

/// @brief Bodies touching one another, directly or through each other, and the
/// players near them, as one tick finds them. Handed out together, so a pile
/// is one player's and moves as one.
struct BodyGroup
{
    std::vector<NetId> bodies;
    /// Every player with a character, the host's own included.
    std::vector<PlayerReach> players;
};

/// @brief Decides, each tick, which player simulates each body.
///
/// A body goes to the player nearest it inside the ownership distance, and
/// stays with that player inside the leave distance, whoever is nearer. A
/// player whose character touches it while its owner's does not takes it after
/// the touch dwell. Once it has changed hands it is given to no one else for
/// the hold, though it can always be lost. No player is given more than the
/// most bodies a client may own, nearest first. The host's own player is a
/// player like any other here; a body it holds is simulated by the host.
///
/// A body two or more players touch at once is contested: the host simulates
/// it, hold or no hold, and each player touching it predicts it. It stays
/// contested until fewer than two have touched it for the touch dwell, and
/// then goes at once to the one still touching it, if any.
class BodyOwnership
{
public:
    explicit BodyOwnership(const OwnershipConfig &config) : _config(config) {}

    /// @brief Hands out every body of @p groups for @p tick, and forgets every
    /// body not in them, which the host simulates.
    void Assign(std::span<const BodyGroup> groups, std::uint64_t tick);

    /// @brief The player who simulates @p netId, or 0 for none.
    [[nodiscard]] std::uint32_t OwnerOf(NetId netId) const;

    /// @brief Takes @p netId back from its player at @p tick: the host
    /// simulates it, and the hold starts again.
    void TakeBack(NetId netId, std::uint64_t tick);

    /// @brief Takes back, with no hold, every body @p client had: it has left.
    void ForgetPlayer(std::uint32_t client);

    /// @brief What is remembered of one body between ticks.
    struct Holding
    {
        /// When it last changed hands; 0 for never.
        std::uint64_t changedTick = 0;
        /// When `challenger` began touching it while its owner did not.
        std::uint64_t challengedSince = 0;
        /// The last tick two players touched it; 0 for not contested.
        std::uint64_t contestedTick = 0;
        /// Who simulates it; 0 for no one.
        std::uint32_t client = 0;
        /// A player touching it that its owner's character does not; 0 for
        /// none.
        std::uint32_t challenger = 0;
        /// Taken back from its player rather than lost: kept from the rest of
        /// its group until the hold is over, or the group's player would be
        /// given it straight back.
        bool takenBack = false;
    };

    [[nodiscard]] const std::unordered_map<NetId, Holding> &Holdings() const { return _holdings; }

private:
    /// Who @p group goes to this tick, given who holds its bodies.
    [[nodiscard]] std::uint32_t Choose(const BodyGroup &group, std::uint64_t tick);

    /// Where a group's contest stands after a tick.
    enum class ContestState
    {
        None,
        Contested,
        Ended
    };

    /// Starts, keeps or ends @p group's contest at @p tick.
    ContestState Contest(const BodyGroup &group, std::uint64_t tick);

    /// Who holds @p group now: of its bodies' players, the nearest; 0 when
    /// none holds any.
    [[nodiscard]] std::uint32_t HolderOf(const BodyGroup &group) const;

    /// A player that is touching @p group while @p holder is not, nearest
    /// first; 0 for none.
    [[nodiscard]] static std::uint32_t ChallengerOf(const BodyGroup &group, std::uint32_t holder);

    /// Who @p group goes to with no holder to keep it: a player touching it,
    /// nearest first, or the nearest inside the ownership distance; 0 for none.
    [[nodiscard]] std::uint32_t NewHolderOf(const BodyGroup &group) const;

    /// Whether @p group changed hands within the hold of @p tick.
    [[nodiscard]] bool Held(const BodyGroup &group, std::uint64_t tick) const;

    /// How near @p client is to @p group, or null when it has no character.
    [[nodiscard]] static const PlayerReach *ReachOf(const BodyGroup &group, std::uint32_t client);

    /// Gives every body of @p group to @p client at @p tick.
    void Give(const BodyGroup &group, std::uint32_t client, std::uint64_t tick);

    std::unordered_map<NetId, Holding> _holdings;
    OwnershipConfig _config;
};

} // namespace Assisi::NetSync
