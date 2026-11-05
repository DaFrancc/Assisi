/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/DistanceRelevancy.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>

#include <algorithm>
#include <cmath>

namespace Assisi::NetSync
{

DistanceRelevancy::DistanceRelevancy(RelevancyConfig config) : _config(config)
{
    if (_config.radius <= 0.f)
    {
        Core::Log::Warn("NetSync: relevancy radius {} is not positive - using 60 m.",
                        static_cast<double>(_config.radius));
        _config.radius = 60.f;
    }

    // An exit radius at or inside the enter radius is not a tuning choice, it is
    // hysteresis switched off — and switched off silently, since the symptom is
    // bandwidth rather than an error. Widen it and say so.
    if (_config.exitRadius <= _config.radius)
    {
        const float widened = _config.radius * 1.25f;
        Core::Log::Warn("NetSync: relevancy exitRadius {} must exceed radius {} or entities on the boundary "
                        "despawn and respawn on every crossing - using {}.",
                        static_cast<double>(_config.exitRadius), static_cast<double>(_config.radius),
                        static_cast<double>(widened));
        _config.exitRadius = widened;
    }
}

void DistanceRelevancy::Compute(const RelevancyQuery &query, std::vector<NetId> &out)
{
    // Fail open. A connection with no viewpoint is not a connection that should
    // see nothing — filtering here is a bandwidth tool, and the failure mode of
    // a missing anchor has to be "the spectator sees the world".
    if (query.anchors.empty())
    {
        out.assign(query.live.begin(), query.live.end());
        return;
    }

    std::unordered_map<NetId, PairState> &pairs = _pairs[query.client.value];

    // Squared throughout: a square root per pair per tick buys nothing.
    const double enterSq = static_cast<double>(_config.radius) * static_cast<double>(_config.radius);
    const double exitSq  = static_cast<double>(_config.exitRadius) * static_cast<double>(_config.exitRadius);
    // An entity's own radius leaves at the same proportion further out as the
    // game's does.
    const double exitRatioSq = exitSq / enterSq;

    // Anchor positions, resolved once rather than per candidate entity. In the
    // world, not under a parent: a camera 1.5 m above its character views from
    // wherever the character is.
    std::vector<glm::dvec3> origins;
    origins.reserve(query.anchors.size());
    for (const ECS::Entity anchor : query.anchors)
    {
        if (query.scene->Has<ECS::Transform>(anchor))
            origins.push_back(ECS::WorldTransformOf(*query.scene, anchor).position);
    }

    // Every anchor named something without a Transform — a dead handle, an
    // entity mid-teardown. Same answer as no anchors at all: we have no
    // viewpoint, so we withhold nothing.
    if (origins.empty())
    {
        out.assign(query.live.begin(), query.live.end());
        return;
    }

    for (const NetId netId : query.live)
    {
        const ECS::Entity entity = query.server->EntityOf(netId);
        if (entity == ECS::NullEntity)
            continue;

        if (!query.scene->Has<ECS::Transform>(entity))
        {
            // Nothing to measure. Included rather than culled: an entity with no
            // position is not far away, it is unplaceable, and silently
            // withholding it would make "my UI proxy never replicates" a mystery
            // with no error anywhere.
            out.push_back(netId);
            continue;
        }

        // Where the entity stands, which for a child is not its Transform.
        const glm::dvec3 position = ECS::WorldTransformOf(*query.scene, entity).position;
        double nearestSq = std::numeric_limits<double>::max();
        for (const glm::dvec3 &origin : origins)
        {
            const glm::dvec3 delta = position - origin;
            nearestSq             = std::min(nearestSq, glm::dot(delta, delta));
        }

        double entityEnterSq = enterSq;
        double entityExitSq  = exitSq;
        if (const Replicated *marker = query.scene->Get<Replicated>(entity);
            marker != nullptr && marker->relevanceRadius > 0.f)
        {
            entityEnterSq = static_cast<double>(marker->relevanceRadius) * static_cast<double>(marker->relevanceRadius);
            entityExitSq  = entityEnterSq * exitRatioSq;
        }

        const auto member = pairs.find(netId);
        if (member == pairs.end())
        {
            // Outside the set. Entry is immediate and unconditional — no dwell,
            // so an anchor teleport shows the world at once.
            if (nearestSq <= entityEnterSq)
            {
                pairs.emplace(netId, PairState{});
                out.push_back(netId);
            }
            continue;
        }

        // Inside the set. Leaving takes both radii and the dwell.
        if (nearestSq <= entityExitSq)
        {
            member->second.outsideSince = 0; // came back inside; the clock resets
            out.push_back(netId);
            continue;
        }

        if (member->second.outsideSince == 0)
        {
            // First tick beyond the exit radius. Start the clock, stay in.
            // Recording simTick + 1 keeps 0 meaning "inside" even at tick zero.
            member->second.outsideSince = query.simTick + 1;
            out.push_back(netId);
            continue;
        }

        if (query.simTick + 1 - member->second.outsideSince < _config.dwellTicks)
        {
            out.push_back(netId); // still serving out the dwell
            continue;
        }

        pairs.erase(member);
    }

    // A member whose entity has been destroyed leaves no other trace here: the
    // loop above only walks live ids, so its pair entry would survive the
    // session. NetIds are never reused, so a leftover cannot cause a wrong
    // answer — only unbounded growth under projectile-style churn, which is
    // exactly the shape that would grow it fastest.
    if (pairs.size() > query.live.size())
    {
        std::erase_if(pairs,
                      [&query](const auto &entry) {
                return !std::binary_search(query.live.begin(), query.live.end(), entry.first);
            });
    }

    // The engine expects sorted output, and `live` is sorted, so this is already
    // in order — but the contract is the contract, and the day a candidate loop
    // stops being a single pass over `live` is not the day to discover it.
    std::sort(out.begin(), out.end());
}

void DistanceRelevancy::ForgetClient(ClientId client) { _pairs.erase(client.value); }

} // namespace Assisi::NetSync
