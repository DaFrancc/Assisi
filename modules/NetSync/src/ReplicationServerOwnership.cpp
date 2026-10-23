/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/ReplicationServer.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/NetSync/NetComponents.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <vector>

#include "ReplicationInternal.hpp"

// ===========================================================================
// ReplicationServer: which client simulates which body, and its reports.
// ===========================================================================

namespace Assisi::NetSync
{
namespace
{

/// Whether @p entity is a body a client may be given to simulate: one the
/// solver moves, not a character, not kept for the host, and not one the
/// host's game gave a client to predict.
bool Ownable(const ECS::Scene &scene, ECS::Entity entity)
{
    const Physics::RigidBody *body = scene.Get<Physics::RigidBody>(entity);
    return body != nullptr && body->motion == Physics::MotionType::Dynamic && !scene.Has<Physics::Character>(entity) &&
           !scene.Has<HostSimulated>(entity) && !scene.Has<Predicted>(entity);
}

/// The root of @p index's set, halving the path to it on the way.
std::size_t Root(std::vector<std::size_t> &parent, std::size_t index)
{
    while (parent[index] != index)
    {
        parent[index] = parent[parent[index]];
        index = parent[index];
    }
    return index;
}

bool Finite(glm::vec3 value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool Finite(const BodyState &state)
{
    return Finite(state.position) && Finite(state.linearVelocity) && Finite(state.angularVelocity) &&
           std::isfinite(state.rotation.x) && std::isfinite(state.rotation.y) && std::isfinite(state.rotation.z) &&
           std::isfinite(state.rotation.w);
}

} // namespace

ClientId ReplicationServer::SimulatorOf(ECS::Entity entity) const
{
    const SimulatedBy *simulator = _scene.Get<SimulatedBy>(entity);
    return simulator != nullptr ? ClientId{simulator->client} : InvalidClientId;
}

void ReplicationServer::SimulateReportedBodies(std::uint64_t tick)
{
    if (_physics == nullptr)
    {
        return;
    }
    TakeBackMovedByGameplay(tick);
    for (std::pair<const Net::ConnectionId, Connection> &entry : _connections)
    {
        ApplyReports(entry.second, tick);
    }
    TakeBackSilent(tick);
    AssignSimulators(tick);
}

void ReplicationServer::TakeBackMovedByGameplay(std::uint64_t tick)
{
    for (const ECS::Entity entity : _physics->MovedByGameplay())
    {
        if (SimulatorOf(entity).IsValid())
        {
            TakeBack(NetIdOf(entity), entity, tick);
        }
    }
}

void ReplicationServer::ApplyReports(Connection &connection, std::uint64_t tick)
{
    std::deque<BodyReport> &held = connection.reports;
    while (!held.empty() && held.front().tick < tick)
    {
        held.pop_front();
    }
    if (held.empty() || held.front().tick != tick)
    {
        return;
    }
    const BodyReport report = std::move(held.front());
    held.pop_front();

    const std::uint32_t client = connection.clientId.value;
    for (const BodyState &state : report.bodies)
    {
        // A body given to someone else since is that one's to report: a report
        // in flight across a handover is normal, and simply not this client's.
        const ECS::Entity entity = EntityOf(state.netId);
        if (entity == ECS::NullEntity || !_scene.IsAlive(entity) || SimulatorOf(entity).value != client)
        {
            continue;
        }
        const std::string_view refusal = RefusalOf(state, client, tick);
        if (!refusal.empty())
        {
            ++connection.diagnostics.reportsRefused;
            Core::Log::Warn("NetSync: client {} reported body {} at tick {} {} - the host simulates it again.",
                            client, state.netId, tick, refusal);
            TakeBack(state.netId, entity, tick);
            continue;
        }
        ++connection.diagnostics.reportsAccepted;
        _physics->ApplyCorrection(entity, Physics::Pose{state.rotation, state.position},
                                  Physics::BodyState{.linearVelocity = state.linearVelocity,
                                                     .angularVelocity = state.angularVelocity,
                                                     .asleep = state.asleep});
        _believed[state.netId] = BelievedReport{.state = state, .tick = tick};
    }
}

std::string_view ReplicationServer::RefusalOf(const BodyState &state, std::uint32_t client, std::uint64_t tick) const
{
    const OwnershipConfig &limits = _config.ownership;
    if (!Finite(state))
    {
        return "with a number that is not one";
    }
    if (glm::length(state.linearVelocity) > limits.reportMaxSpeed)
    {
        return "moving faster than reportMaxSpeed";
    }
    if (glm::length(state.angularVelocity) > limits.reportMaxSpin)
    {
        return "spinning faster than reportMaxSpin";
    }

    std::vector<glm::vec3> feet;
    FeetOf(client, feet);
    float nearest = std::numeric_limits<float>::infinity();
    for (const glm::vec3 at : feet)
    {
        nearest = std::min(nearest, glm::length(state.position - at));
    }
    if (nearest > limits.leaveDistance + limits.reportReachSlack)
    {
        return "out of its character's reach";
    }

    const std::unordered_map<NetId, BelievedReport>::const_iterator believed = _believed.find(state.netId);
    if (believed != _believed.end() && tick > believed->second.tick)
    {
        const float seconds = static_cast<float>(tick - believed->second.tick) / static_cast<float>(_config.tickRateHz);
        const float moved = glm::length(state.position - believed->second.state.position);
        if (moved > limits.reportMaxSpeed * seconds + limits.reportReachSlack)
        {
            return "further from its last report than reportMaxSpeed reaches";
        }
    }
    return {};
}

void ReplicationServer::TakeBackSilent(std::uint64_t tick)
{
    std::vector<ECS::Entity> silent;
    for (auto [entity, simulator] : _scene.Query<SimulatedBy>())
    {
        (void)simulator;
        const std::unordered_map<NetId, BelievedReport>::const_iterator believed = _believed.find(NetIdOf(entity));
        const std::uint64_t since = believed != _believed.end() ? believed->second.tick : 0u;
        if (tick > since + _config.ownership.reportTimeoutTicks)
        {
            silent.push_back(entity);
        }
    }
    for (const ECS::Entity entity : silent)
    {
        Core::Log::Info("NetSync: client {} has not reported body {} for {} ticks - the host simulates it again.",
                        SimulatorOf(entity).value, NetIdOf(entity), _config.ownership.reportTimeoutTicks);
        TakeBack(NetIdOf(entity), entity, tick);
    }
}

void ReplicationServer::AssignSimulators(std::uint64_t tick)
{
    const std::vector<BodyGroup> groups = BodyGroups();
    _ownership.Assign(groups, tick);

    // Unmarked first: a body the rules no longer list at all is the host's.
    std::vector<ECS::Entity> marked;
    for (auto [entity, simulator] : _scene.Query<SimulatedBy>())
    {
        (void)simulator;
        marked.push_back(entity);
    }
    for (const ECS::Entity entity : marked)
    {
        const NetId netId = NetIdOf(entity);
        if (_ownership.OwnerOf(netId) < kFirstRemoteClientId)
        {
            PlaceSimulator(netId, entity, 0, tick);
        }
    }
    for (const BodyGroup &group : groups)
    {
        for (const NetId netId : group.bodies)
        {
            const std::uint32_t owner = _ownership.OwnerOf(netId);
            if (owner >= kFirstRemoteClientId)
            {
                PlaceSimulator(netId, EntityOf(netId), owner, tick);
            }
        }
    }
}

std::vector<BodyGroup> ReplicationServer::BodyGroups() const
{
    std::vector<ECS::Entity> bodies;
    std::vector<NetId> netIds;
    std::unordered_map<std::uint64_t, std::size_t> indexOf;
    for (auto [entity, rigidBody, replicated] : _scene.Query<Physics::RigidBody, Replicated>())
    {
        (void)rigidBody;
        (void)replicated;
        const NetId netId = NetIdOf(entity);
        if (!netId.IsValid() || !Ownable(_scene, entity) || !_physics->HasBody(entity))
        {
            continue;
        }
        indexOf.emplace(PackEntity(entity), bodies.size());
        bodies.push_back(entity);
        netIds.push_back(netId);
    }

    // Touching bodies, directly or through one another, are one group.
    std::vector<std::size_t> parent(bodies.size());
    std::iota(parent.begin(), parent.end(), std::size_t{0});
    std::vector<ECS::Entity> touching;
    for (std::size_t i = 0; i < bodies.size(); ++i)
    {
        _physics->Touching(bodies[i], touching);
        for (const ECS::Entity other : touching)
        {
            const std::unordered_map<std::uint64_t, std::size_t>::const_iterator found = indexOf.find(PackEntity(other));
            if (found != indexOf.end())
            {
                parent[Root(parent, i)] = Root(parent, found->second);
            }
        }
    }

    std::vector<BodyGroup> groups;
    std::unordered_map<std::size_t, std::size_t> groupOf;
    std::vector<std::vector<ECS::Entity>> members;
    for (std::size_t i = 0; i < bodies.size(); ++i)
    {
        const std::pair<std::unordered_map<std::size_t, std::size_t>::iterator, bool> added =
            groupOf.try_emplace(Root(parent, i), groups.size());
        if (added.second)
        {
            groups.emplace_back();
            members.emplace_back();
        }
        groups[added.first->second].bodies.push_back(netIds[i]);
        members[added.first->second].push_back(bodies[i]);
    }

    // How near each player is: its nearest character to the group's nearest
    // body, and whether any of its characters touches any of them.
    std::vector<glm::vec3> feet;
    for (const std::pair<const std::uint32_t, std::vector<ECS::Entity>> &controlled : _controlledByClient)
    {
        FeetOf(controlled.first, feet);
        if (feet.empty())
        {
            continue;
        }
        for (std::size_t g = 0; g < groups.size(); ++g)
        {
            PlayerReach reach{.distance = std::numeric_limits<float>::infinity(), .client = controlled.first};
            for (const ECS::Entity body : members[g])
            {
                const glm::vec3 centre = _physics->GetBodyPose(body).position;
                for (const glm::vec3 at : feet)
                {
                    reach.distance = std::min(reach.distance, glm::length(centre - at));
                }
                for (const ECS::Entity character : controlled.second)
                {
                    reach.touching = reach.touching || _physics->IsTouching(character, body);
                }
            }
            groups[g].players.push_back(reach);
        }
    }
    for (BodyGroup &group : groups)
    {
        std::sort(group.bodies.begin(), group.bodies.end());
    }
    return groups;
}

void ReplicationServer::FeetOf(std::uint32_t client, std::vector<glm::vec3> &out) const
{
    out.clear();
    const std::unordered_map<std::uint32_t, std::vector<ECS::Entity>>::const_iterator controlled =
        _controlledByClient.find(client);
    if (controlled == _controlledByClient.end())
    {
        return;
    }
    for (const ECS::Entity entity : controlled->second)
    {
        if (_scene.IsAlive(entity) && _scene.Has<Physics::Character>(entity) && _physics->HasBody(entity))
        {
            out.push_back(_physics->CaptureCharacter(entity).position);
        }
    }
}

void ReplicationServer::TakeBack(NetId netId, ECS::Entity entity, std::uint64_t tick)
{
    _ownership.TakeBack(netId, tick);
    PlaceSimulator(netId, entity, 0, tick);
}

void ReplicationServer::PlaceSimulator(NetId netId, ECS::Entity entity, std::uint32_t client, std::uint64_t tick)
{
    if (entity == ECS::NullEntity || !_scene.IsAlive(entity))
    {
        return;
    }
    const SimulatedBy *simulator = _scene.Get<SimulatedBy>(entity);
    if ((simulator == nullptr && client == 0) || (simulator != nullptr && simulator->client == client))
    {
        return;
    }
    // Restated to every client as if seen for the first time: the one that
    // simulated it has not been told the host's word about it since, and a
    // body at rest would otherwise never be spoken of again.
    _bodyStates.erase(netId);
    if (client == 0)
    {
        _scene.Remove<SimulatedBy>(entity);
        _believed.erase(netId);
        return;
    }

    // Measured from where the host has it as it hands it over: the client's
    // first report is about a tick a round trip later, which the displacement
    // check allows for over the ticks between.
    const Physics::Pose pose = _physics->GetBodyPose(entity);
    const Physics::BodyState motion = _physics->GetBodyState(entity);
    _believed[netId] = BelievedReport{.state = BodyState{.netId = netId,
                                                         .position = pose.position,
                                                         .rotation = pose.rotation,
                                                         .linearVelocity = motion.linearVelocity,
                                                         .angularVelocity = motion.angularVelocity,
                                                         .asleep = motion.asleep},
                                      .tick = tick};
    // Through GetMut, so the change reaches the clients.
    if (simulator != nullptr)
    {
        _scene.GetMut<SimulatedBy>(entity)->client = client;
        return;
    }
    (void)_scene.Add<SimulatedBy>(entity, SimulatedBy{client});
}

} // namespace Assisi::NetSync
