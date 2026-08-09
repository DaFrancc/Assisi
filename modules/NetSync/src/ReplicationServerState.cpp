/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/Replication.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/DistanceRelevancy.hpp>
#include <Assisi/NetSync/NetComponents.hpp>

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <expected>
#include <string>
#include <utility>

#include "ReplicationInternal.hpp"

// ===========================================================================
// ReplicationServer: the per-tick bookkeeping behind a snapshot.
// ===========================================================================

namespace Assisi::NetSync
{
void ReplicationServer::ReconcileNetIds()
{
    _liveNetIds.clear();

    for (auto [entity, replicated] : _scene.Query<Replicated>())
    {
        (void)replicated;
        const std::uint64_t key = PackEntity(entity);
        const auto          it  = _netIdByEntity.find(key);
        NetId               netId;
        if (it == _netIdByEntity.end())
        {
            // The other id-assignment path, and it has to reach the block too:
            // an instance whose members are first noticed by the snapshot walk
            // rather than by an event send must still get one contiguous range.
            netId = EnsureInstanceBlock(entity);
            if (netId == InvalidNetId)
                netId = NetId{_nextNetId++};
            _netIdByEntity.emplace(key, netId);
            _entityByNetId.emplace(netId, entity);
        }
        else
        {
            netId = it->second;
        }
        _liveNetIds.push_back(netId);
    }

    std::sort(_liveNetIds.begin(), _liveNetIds.end());

    // Drop mappings whose entity is gone or has stopped replicating. The NetId
    // itself is retired with it and never reused — a stale reference must fail
    // to resolve rather than quietly address whoever took the slot.
    for (auto it = _entityByNetId.begin(); it != _entityByNetId.end();)
    {
        const ECS::Entity entity = it->second;
        if (!_scene.IsAlive(entity) || !_scene.Has<Replicated>(entity))
        {
            _netIdByEntity.erase(PackEntity(entity));
            it = _entityByNetId.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Which members a client's own expansion could still stand in for. Monotonic
    // and never re-set: a member that is missing for one tick may have been
    // missing when a record went out, and the server has no per-connection
    // memory of *which* record said what. Over-sending that member's components
    // from then on is the cheap side of the trade; the expensive side is a
    // revived member whose authored-equal components are elided against a client
    // copy that does not exist.
    for (auto &[instanceId, block] : _instanceBlocks)
    {
        (void)instanceId;
        for (std::uint32_t member = 0; member < block.memberCount; ++member)
        {
            if (block.derivable[member] != 0u && !_entityByNetId.contains(NetId{block.base.value + member}))
                block.derivable[member] = 0u;
        }
    }

    // Here rather than anywhere incremental: this is the one point per tick that
    // has already accepted the scene as it is, including whatever came back from
    // the dead since the last one.
    RebuildControlIndex();

    // The escape classes, resolved once per tick rather than per connection.
    // Both lists are tiny by construction — an entity opting out of the provider
    // is unusual by definition — and the alternative is walking the whole live
    // set once per connection, which is exactly the cost relevancy exists to
    // avoid paying.
    _alwaysRelevant.clear();
    _controllerOnly.clear();
    if (_relevancy != nullptr)
    {
        for (const NetId netId : _liveNetIds)
        {
            const ECS::Entity entity = EntityOf(netId);
            if (entity == ECS::NullEntity)
                continue;

            const Replicated *marker = _scene.Get<Replicated>(entity);
            if (marker == nullptr || marker->relevance == Relevance::Default)
                continue;

            if (marker->relevance == Relevance::Always)
            {
                _alwaysRelevant.push_back(netId);
            }
            else
            {
                const ControlledBy *claim = _scene.Get<ControlledBy>(entity);
                _controllerOnly.emplace_back(netId, claim == nullptr ? InvalidClientId.value : claim->client);
            }
        }
    }
    // `_liveNetIds` is sorted, so both come out sorted without a sort — but the
    // binary searches downstream depend on it, so it is stated rather than left
    // to be rediscovered.
}

Core::Reflect::ComponentMask ReplicationServer::ExclusionMaskOf(ECS::Entity entity) const
{
    if (const Replicated *marker = _scene.Get<Replicated>(entity))
        return marker->excluded;
    return Core::Reflect::ComponentMask{};
}

bool ReplicationServer::ReplicatesAsBody(ECS::Entity entity, const Core::Reflect::ComponentMask &excluded) const
{
    if (_physics == nullptr || _scene.Get<Physics::RigidBody>(entity) == nullptr)
        return false;

    // Authored geometry is not simulated, whatever its live motion type says: a
    // static body's pose is *authored data* and already has a replication path,
    // the ordinary tracked-Transform delta. Keyed off the descriptor rather than
    // the live motion type on purpose — the editor holds a *dynamic* body Static
    // for the duration of a gizmo drag, and that body is still a simulated one
    // being placed.
    const Physics::RigidBodyDescriptor *descriptor = _scene.Get<Physics::RigidBodyDescriptor>(entity);
    if (descriptor == nullptr || descriptor->isStatic)
        return false;

    // Policy: the client will never build a body it was never sent a descriptor
    // for, so correcting one would be talking to nobody. Its Transform replicates
    // normally instead and the mirror becomes an interpolated visual — "show this
    // moving, don't simulate it", which is a useful thing to be able to author.
    if (excluded.Test(_descriptorOrdinal))
        return false;

    // ...and both client-side body builders require a Transform, so a bodied
    // entity withholding one can never have a body built either. Sending body
    // state it must drop on arrival is pure waste; treat it as non-bodied and let
    // the editor's warning explain that the mirror will sit at its load pose.
    if (excluded.Test(_transformOrdinal))
        return false;

    return true;
}

void ReplicationServer::CaptureBodyStates()
{
    if (_physics == nullptr)
        return;

    ++_bodyStateTick;
    _physics->GetActiveBodyStates(_activeBodies);

    for (const NetId netId : _liveNetIds)
    {
        const ECS::Entity entity = EntityOf(netId);
        if (entity == ECS::NullEntity)
            continue;

        const Physics::RigidBody *body = _scene.Get<Physics::RigidBody>(entity);
        if (body == nullptr)
            continue; // not a simulated entity; its Transform replicates normally

        // One predicate for every "does motion travel as body state" question in
        // this file — see ReplicatesAsBody for the four cases it rules out.
        if (!ReplicatesAsBody(entity, ExclusionMaskOf(entity)))
        {
            // Erasing is not tidiness. The retirement sweep at the bottom only
            // drops records for *retired* NetIds, so a record left behind by an
            // entity that stopped being bodied would keep its nonzero tick — and
            // that tick beats every post-sweep empty baseline, so the stale state
            // would be resent to every client after every keyframe sweep, for the
            // rest of the session. It would also suppress the first-sighting
            // capture if the entity ever became bodied again.
            _bodyStates.erase(netId);
            continue;
        }

        const auto active = std::find_if(_activeBodies.begin(), _activeBodies.end(),
                                         [entity](const Physics::PhysicsWorld::ActiveBodyState &candidate)
                                         { return candidate.entity == entity; });

        BodyRecord &record = _bodyStates[netId];
        if (active != _activeBodies.end())
        {
            record.state = BodyState{netId,   active->position,        active->rotation,
                                     active->linearVelocity, active->angularVelocity, /*asleep=*/false};
            record.tick  = _bodyStateTick;
            continue;
        }

        // Not active. Three reasons to record it anyway, and none of them is
        // "every tick":
        //
        //  - first sighting, which is what makes joining an already-settled
        //    world produce sleeping mirrors at the server's rest poses rather
        //    than a client-side re-settle;
        //  - it has just gone to sleep — the transition is the change, and the
        //    one whose loss used to be permanent;
        //  - it *moved while not simulating*. Nothing wakes a body for that, so
        //    the active set says nothing about it: the editor's gizmo holds a
        //    body Static for the duration of a drag precisely so the solver
        //    stops fighting the teleport, an inspector Transform edit does the
        //    same, gameplay can reposition a sleeping body, and a static body is
        //    never active at all — so for a replicated wall, "record it when it
        //    stops being active" fires once, at its load pose, and never again.
        //
        // Polling the pose rather than having every mutation site announce
        // itself is deliberate, and it is the same argument this design already
        // makes against Unreal's dormancy discipline: correctness that depends
        // on every call site remembering to flush is correctness that will be
        // forgotten. The cost is one transform read per resting replicated body
        // per tick, which is what "the physics world is the truth" is worth.
        //
        // A body that is asleep and has not moved records nothing, which is
        // where the idle-bandwidth property comes from.
        {
            const auto [position, rotation] = _physics->GetBodyTransform(*body);

            const bool firstSighting = record.tick == 0;
            const bool justSlept     = !firstSighting && !record.state.asleep;
            // Both poses are frozen while a body rests — nothing integrates them
            // — so this compares against float noise, not against motion. The
            // epsilons are well under one quantization step, so a move too small
            // to survive the encoder cannot trigger a send either.
            const bool movedAtRest =
                !firstSighting && record.state.asleep &&
                (glm::distance(record.state.position, position) > 1e-5f ||
                 std::abs(glm::dot(record.state.rotation, rotation)) < 1.f - 1e-6f);

            if (!firstSighting && !justSlept && !movedAtRest)
                continue;

            const bool transition = justSlept;
            record.state = BodyState{netId, position, rotation, glm::vec3{0.f}, glm::vec3{0.f}, /*asleep=*/true};
            record.tick  = _bodyStateTick;

            // Every other update is superseded by the next one; the final rest
            // pose is the one whose delay is permanently visible, because after
            // it the server has nothing more to say about this body.
            if (transition)
            {
                for (auto &[id, connection] : _connections)
                {
                    (void)id;
                    connection.priority[netId] += kSleepTransitionBoost;
                }
            }
        }
    }

    // Retired NetIds take their records with them.
    std::erase_if(_bodyStates,
                  [this](const auto &entry)
                  { return !std::binary_search(_liveNetIds.begin(), _liveNetIds.end(), entry.first); });
}

} // namespace Assisi::NetSync
