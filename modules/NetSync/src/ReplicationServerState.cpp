/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/ReplicationServer.hpp>

#include <Assisi/NetSync/NetComponents.hpp>

#include <algorithm>

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
            // The second id-assignment path, and it must consult the block too:
            // an instance first noticed by the snapshot walk rather than by an
            // event send still needs one contiguous range.
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
    // retires with it and is never reused: a stale reference must fail to
    // resolve rather than quietly address whoever took the slot.
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

    // Which members a client's own expansion could still stand in for. Clears
    // only, never re-set: a member missing for one tick may have been missing
    // when a record went out, and the server keeps no per-connection memory of
    // which record said what. Over-sending that member's components afterwards
    // is the cheap side; the expensive side is a revived member whose
    // authored-equal components are elided against a client copy that does not
    // exist.
    for (auto &[instanceId, block] : _instanceBlocks)
    {
        (void)instanceId;
        for (std::uint32_t member = 0; member < block.memberCount; ++member)
        {
            if (block.derivable[member] != 0u && !_entityByNetId.contains(NetId{block.base.value + member}))
                block.derivable[member] = 0u;
        }
    }

    // Here rather than incrementally: this is the one point per tick that has
    // already accepted the scene as it is, revivals included.
    RebuildControlIndex();

    // The escape classes, resolved once per tick rather than per connection.
    // Both lists are tiny by construction, and the alternative is walking the
    // whole live set once per connection — the cost relevancy exists to avoid.
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
    // Both lists come out sorted because `_liveNetIds` is, and the binary
    // searches downstream depend on that.
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
    // static body's pose is authored data and already replicates as the ordinary
    // tracked-Transform delta. **Keyed off the descriptor, not the live motion
    // type** — the editor holds a *dynamic* body Static for the length of a
    // gizmo drag, and that body is still a simulated one being placed.
    const Physics::RigidBodyDescriptor *descriptor = _scene.Get<Physics::RigidBodyDescriptor>(entity);
    if (descriptor == nullptr || descriptor->isStatic)
        return false;

    // A client never builds a body it was sent no descriptor for, so correcting
    // one would be talking to nobody. Its Transform replicates normally instead
    // and the mirror becomes an interpolated visual: "show this moving, don't
    // simulate it", which is a useful thing to author.
    if (excluded.Test(_descriptorOrdinal))
        return false;

    // ...and both client-side body builders require a Transform, so withholding
    // one also means no body is ever built. Sending body state it must drop on
    // arrival is waste; the editor warns that the mirror sits at its load pose.
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

        if (!ReplicatesAsBody(entity, ExclusionMaskOf(entity)))
        {
            // **Erasing is not tidiness.** The sweep at the bottom only drops
            // records for *retired* NetIds, so a record left by an entity that
            // stopped being bodied keeps its nonzero tick — which beats every
            // post-sweep empty baseline, resending stale state to every client
            // after every keyframe sweep for the rest of the session. It would
            // also suppress the first-sighting capture if the entity became
            // bodied again.
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

        // Not active. Three reasons to record it anyway, none of them "every
        // tick":
        //
        //  - first sighting, so joining an already-settled world gives sleeping
        //    mirrors at the server's rest poses rather than a client re-settle;
        //  - it has just gone to sleep — the transition is the change;
        //  - it *moved while not simulating*, which nothing wakes a body for, so
        //    the active set says nothing about it: the editor gizmo and the
        //    inspector both hold a body Static while it is being moved, gameplay
        //    can reposition a sleeping body, and a static body is never active
        //    at all — for a replicated wall, "record it when it stops being
        //    active" fires once, at its load pose, and never again.
        //
        // Hence polling the pose instead of trusting every mutation site to
        // announce itself: correctness that depends on every call site
        // remembering to flush is correctness that will be forgotten. The cost
        // is one transform read per resting replicated body per tick.
        //
        // A body asleep and unmoved records nothing, which is where the
        // idle-bandwidth property comes from.
        {
            const auto [position, rotation] = _physics->GetBodyTransform(*body);

            const bool firstSighting = record.tick == 0;
            const bool justSlept     = !firstSighting && !record.state.asleep;
            // Nothing integrates a resting pose, so this compares against float
            // noise rather than motion. The epsilons sit well under one
            // quantization step: a move too small to survive the encoder cannot
            // trigger a send either.
            const bool movedAtRest =
                !firstSighting && record.state.asleep &&
                (glm::distance(record.state.position, position) > 1e-5f ||
                 std::abs(glm::dot(record.state.rotation, rotation)) < 1.f - 1e-6f);

            if (!firstSighting && !justSlept && !movedAtRest)
                continue;

            const bool transition = justSlept;
            record.state = BodyState{netId, position, rotation, glm::vec3{0.f}, glm::vec3{0.f}, /*asleep=*/true};
            record.tick  = _bodyStateTick;

            // Every other update is superseded by the next one; a delayed final
            // rest pose stays visible, because after it the server has nothing
            // more to say about this body.
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
