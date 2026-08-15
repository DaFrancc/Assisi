/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file DistanceRelevancy.hpp
/// @brief The relevancy provider almost every game wants: a radius around what
/// you are looking at, with hysteresis so the boundary is not a bug.

#include <Assisi/NetSync/ReplicationConfig.hpp>
#include <Assisi/NetSync/ReplicationProviders.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Assisi::NetSync
{

/// @brief Tells a connection about entities near its view anchors.
///
/// The industry default, from Unreal's `NetCullDistanceSquared` to Fusion's
/// area of interest, and the only provider that ships with the engine — a game
/// wanting an information boundary (line of sight, fog of war) writes its own,
/// on top of the engine's guarantee that an entity outside the set costs zero
/// bytes.
///
/// ## Hysteresis is the mechanism, not a refinement
///
/// Two radii and a dwell, because one radius makes a boundary into a
/// despawn/full-respawn generator: an entity hovering at exactly the cull
/// distance leaves and re-enters on float noise, and each crossing costs a
/// despawn one way and a complete re-send the other. Enter is strictly nearer
/// than exit, and a revoke additionally waits out a dwell.
///
/// **The dwell gates revokes only.** Entering is immediate. Delaying entry
/// would mean that an anchor teleport — a level transition, a spectator jumping
/// across the map — shows an empty world for the length of the dwell and then
/// pops it in, which is the exact artefact the mechanism exists to prevent,
/// merely relocated. The symmetric version is the reflex to resist.
///
/// ## Fail open
///
/// A connection with no anchors is told about everything. Filtering here exists
/// for bandwidth, not secrecy, so "the spectator has no viewpoint yet" must
/// resolve to seeing the world rather than seeing nothing. A game whose
/// provider *is* an information boundary owns the opposite choice inside its
/// own provider.
///
/// ## Scale
///
/// One squared distance per (connection, entity) per snapshot tick: tens of
/// connections against hundreds of entities is a few thousand compares, which
/// is noise. The recorded escape route, if entity counts ever make this bind,
/// is a shared spatial grid whose cells hold entity lists and are maintained on
/// movement — membership evaluated where things change rather than where it is
/// read. Nothing is built until a profile says so; the trigger is Chiara.
class DistanceRelevancy final : public RelevancyProvider
{
public:
    explicit DistanceRelevancy(RelevancyConfig config);

    void Compute(const RelevancyQuery &query, std::vector<NetId> &out) override;
    void ForgetClient(ClientId client) override;

    [[nodiscard]] const RelevancyConfig &Config() const { return _config; }

private:
    /// What one (connection, entity) pair is doing, kept only for pairs that
    /// are currently *in* the set — the ones a revoke could apply to.
    struct PairState
    {
        /// Tick this entity first went beyond the exit radius, or 0 while it is
        /// inside it. The dwell is measured from here.
        std::uint64_t outsideSince = 0;
    };

    RelevancyConfig _config;

    /// Per client, per NetId. Erased wholesale when a connection leaves, and
    /// entry by entry as entities stop being members.
    std::unordered_map<std::uint32_t, std::unordered_map<NetId, PairState>> _pairs;
};

} // namespace Assisi::NetSync
