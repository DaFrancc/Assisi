/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/ReplicationServer.hpp>

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
// ReplicationServer: writing and sending a snapshot.
// ===========================================================================

namespace Assisi::NetSync
{
void ReplicationServer::WriteEntityComponents(NetId netId, ECS::Entity entity, std::uint64_t sinceChangeTick,
                                              const Connection &connection, Core::BitWriter &writer,
                                              std::vector<std::uint64_t> &outComponents)
{
    Core::Reflect::CodecContext context;
    context.entityToWire = [this](std::uint64_t packed) -> std::uint64_t
    {
        // Entity references cross the wire as NetIds. A reference to something
        // that does not replicate resolves to zero rather than to a local handle
        // the peer would misread as one of its own.
        const NetId referenced = NetIdOf(UnpackEntity(packed));
        return referenced.value; // wire boundary
    };

    // A BlueprintMember's instanceId is per-world and per-machine, so it is
    // rewritten to the instance's base NetId on the way out and back to the
    // peer's own id on the way in. Without this the tag replicates as a number
    // that names nothing on the far side.
    context.instanceToWire = [this](std::uint32_t instanceId) -> std::uint32_t
    {
        const auto block = _instanceBlocks.find(ECS::InstanceId{instanceId});
        return block == _instanceBlocks.end() ? 0u : block->second.base.value;
    };

    const Core::Reflect::ComponentRegistry &registry = Core::Reflect::ComponentRegistry::Instance();

    // This entity's own policy, read live. There is deliberately no cache: the
    // mask is a plain value on a component we already have to look up, so
    // caching would buy a hash lookup and cost an invalidation problem — which
    // is also why `Replicated` needs no change tracking.
    Core::Reflect::ComponentMask excluded;
    if (const Replicated *marker = _scene.Get<Replicated>(entity))
        excluded = marker->excluded;

    // What this entity has right now *and* is willing to send, in the same
    // packed, sorted form as the acked baseline — _replicatedComponents is
    // registry order, which is ascending by id, so this comes out sorted without
    // a sort.
    //
    // Excluded components are absent from this list, which is what makes the
    // rest fall out for free: the removal diff below sees them disappear from
    // the client's acked slice and sends removals, and D11 sees them reappear
    // and force-sends. One filter, both directions.
    const std::size_t componentsBegin = outComponents.size();
    for (std::size_t slot = 0; slot < _replicatedComponents.size(); ++slot)
    {
        if (excluded.Test(_replicatedOrdinals[slot]))
            continue;
        const Core::Reflect::ComponentId    id   = _replicatedComponents[slot];
        const Core::Reflect::ComponentMeta *meta = registry.ById(id);
        if (meta != nullptr && meta->getByEntity(&_scene, entity.index, entity.generation) != nullptr)
            outComponents.push_back(PackComponentRef(netId, id));
    }

    // Removals. Change detection stamps writes, not removals — there is no tick
    // to consult for "this component is gone" — so it is found by diffing this
    // entity's slice of the acked baseline against what it has now. Exactly the
    // shape of the despawn comparison, one level down.
    // ComponentId{0}: the packed key's lower bound, not "invalid" — 0 is the
    // lowest possible ordinal, and component ids are dense from there.
    const auto ackedLow  = std::lower_bound(connection.ackedComponents.begin(), connection.ackedComponents.end(),
                                            PackComponentRef(netId, Core::Reflect::ComponentId{0}));
    // netId + 1: the exclusive upper bound of this netId's packed-ref range,
    // spelled explicitly since NetId has no arithmetic of its own.
    const auto ackedHigh = std::lower_bound(connection.ackedComponents.begin(), connection.ackedComponents.end(),
                                            PackComponentRef(NetId{netId.value + 1}, Core::Reflect::ComponentId{0}));

    std::vector<Core::Reflect::ComponentId> removed;
    for (auto it = ackedLow; it != ackedHigh; ++it)
    {
        const bool stillPresent = std::binary_search(outComponents.begin() + static_cast<std::ptrdiff_t>(componentsBegin),
                                                     outComponents.end(), *it);
        if (!stillPresent)
            removed.push_back(ComponentIdOfRef(*it));
    }

    writer.WriteVarUInt32(static_cast<std::uint32_t>(removed.size()));
    for (const Core::Reflect::ComponentId id : removed)
        writer.WriteVarUInt32(id.value); // wire write

    // For an entity the *solver* owns, motion travels as body state, not as a
    // Transform. Sending both would double the cost of every moving object and
    // hand the client two disagreeing answers about where it is.
    //
    // The same predicate the capture uses, and it must be: an entity one of them
    // considers bodied and the other does not gets its Transform suppressed *and*
    // no body state, leaving the mirror frozen at its load pose.
    const bool bodied = ReplicatesAsBody(entity, excluded);

    // Read once rather than per component: the blueprint baseline below needs it
    // for every component this entity has.
    const ECS::BlueprintMember *memberTag = _scene.Get<ECS::BlueprintMember>(entity);

    // ...and so is the one question that decides whether the blueprint baseline
    // may be used at all: did the client's own expansion of this instance
    // produce this member? A member that was absent for even a tick was either
    // never expanded there (the record's presence bits said so) or was destroyed
    // there by a despawn, and in both cases the file's copy is not what the
    // client holds. Resolved per entity rather than per component, since it is
    // the same answer for all of them.
    bool derivedFromBlueprint = false;
    if (memberTag != nullptr && _instanceInfo != nullptr)
    {
        if (const auto block = _instanceBlocks.find(memberTag->instanceId); block != _instanceBlocks.end())
        {
            derivedFromBlueprint = memberTag->memberIndex < block->second.memberCount &&
                                   block->second.derivable[memberTag->memberIndex] != 0u;
        }
    }

    for (std::size_t slot = 0; slot < _replicatedComponents.size(); ++slot)
    {
        if (excluded.Test(_replicatedOrdinals[slot]))
            continue; // this entity declines to send it

        const Core::Reflect::ComponentId     id   = _replicatedComponents[slot];
        const Core::Reflect::ComponentMeta *meta = registry.ById(id);
        if (meta == nullptr)
            continue;

        const void *component = meta->getByEntity(&_scene, entity.index, entity.generation);
        if (component == nullptr)
            continue;

        // D11, the removal diff's dual: the client does not have this component,
        // so send its full state regardless of what the change tick says.
        //
        // The change tick answers "did this value change since the client last
        // saw it", which is the wrong question whenever the *presence* changed
        // instead. Re-including an excluded component is exactly that case —
        // policy moved, the component did not, so its tick still predates the
        // baseline and the gate below would skip it until the next keyframe
        // sweep, up to several seconds of a mirror the server believes is whole.
        // It also covers the general case of a component the client lost while
        // the server has nothing new to stamp on it.
        const bool clientHasIt =
            std::binary_search(ackedLow, ackedHigh, PackComponentRef(netId, id));

        // sinceChangeTick == 0 is the empty baseline: spawn, late join, and a
        // client that has acked nothing all take this path, which is the whole
        // point of not having a separate full-state message.
        if (sinceChangeTick != 0 && clientHasIt && !_scene.ChangedById(entity, id, sinceChangeTick))
            continue;

        // The blueprint baseline. On the empty baseline a member does not start
        // from nothing on the far side — the client expanded the same file — so
        // a component still holding its authored value is already correct there.
        //
        // Deliberately not recorded as absent: `outComponents` above still lists
        // it, because the client really does have it. Treating it as missing
        // would make the removal diff announce a removal for a component nobody
        // removed.
        if (sinceChangeTick == 0 && !clientHasIt && derivedFromBlueprint &&
            _instanceInfo->MatchesAuthored(memberTag->instanceId, memberTag->memberIndex, id, component))
        {
            continue;
        }

        // ...and on that empty baseline a bodied entity's Transform still goes,
        // because it carries scale and the initial placement the client builds
        // its body at. Afterwards it is suppressed. The honest cost: a *non-pose*
        // Transform edit on a live body — a runtime scale change — reaches
        // clients only at the next keyframe sweep. Accepted for v1, because a
        // scale change on a live body needs a collider reshape the engine does
        // not do yet either (docs/replication-plan-v4.md §5).
        //
        // Suppression yields to D11 for the same reason the change gate does: a
        // client that has never received this Transform has nowhere to build its
        // body from, so "the body owns motion" has nothing to be true about yet.
        if (bodied && sinceChangeTick != 0 && clientHasIt && id == _transformComponentId)
            continue;

        writer.WriteBool(true);
        Core::Reflect::WriteComponent(*meta, component, writer, Core::Reflect::kAllFields, &context);
    }
    writer.WriteBool(false);
}

void ReplicationServer::WriteBodyStates(Connection &connection, const std::vector<NetId> &effective,
                                        Core::BitWriter &writer, SentSnapshot &record,
                                        std::size_t writtenFromComponents)
{
    // Bool-chained like the entity blocks above, rather than the count-prefixed
    // form the design sketch shows. A count has to be known before the first
    // record is written, which means predicting the budget cut instead of
    // discovering it — and the bool chain is cheaper anyway (one bit per record
    // plus a terminator, against a varint).
    if (_physics == nullptr)
    {
        writer.WriteBool(false);
        return;
    }

    const auto prefixEnd = record.written.begin() + static_cast<std::ptrdiff_t>(writtenFromComponents);

    // The effective set, not the live one. This loop is an independent walk with
    // its own acked-based gate, and that gate does *not* imply relevancy: an
    // entity that has left the set is still acked until its despawn round-trips,
    // so walking `_liveNetIds` here would ship body state for it every tick of
    // that window. Zero bytes has to mean zero.
    for (const NetId netId : effective)
    {
        if (writer.BytesWritten() >= _config.maxSnapshotBytes)
            break; // the rest keep their baselines and go next snapshot

        const auto found = _bodyStates.find(netId);
        if (found == _bodyStates.end() || found->second.tick == 0)
            continue;

        const auto baseline = connection.baselines.find(netId);
        if (baseline != connection.baselines.end() && found->second.tick <= baseline->second.bodyTick)
            continue; // already delivered

        // The gate: a body state is only useful to a client that has something
        // to apply it to. Without it, an entity whose spawn block was cut for
        // budget would ship a body state the client has no mirror — and no
        // descriptor to build a body from — to receive it.
        //
        // `record.netIds` is this snapshot's entity set in ascending order, and
        // holds both the entities written here and the known ones the budget
        // skipped; the skipped ones are acked by definition, so testing against
        // it is exactly "acked, or written into this same snapshot".
        if (!std::binary_search(connection.acked.begin(), connection.acked.end(), netId) &&
            !std::binary_search(record.netIds.begin(), record.netIds.end(), netId))
        {
            continue;
        }

        writer.WriteBool(true);
        WriteBodyState(found->second.state, writer);

        // Record the delivery against this entity. It may already have an entry
        // from the component pass; if not, componentTick stays 0 and the ack's
        // max() leaves whatever component baseline it had untouched.
        const auto slot = std::lower_bound(record.written.begin(), prefixEnd, netId,
                                           [](const WrittenEntity &entry, NetId value)
                                           { return entry.netId < value; });
        if (slot != prefixEnd && slot->netId == netId)
            slot->ticks.bodyTick = found->second.tick;
        else
            record.written.push_back(WrittenEntity{netId, EntityBaseline{0, found->second.tick}});
    }

    writer.WriteBool(false);
}

void ReplicationServer::SendSnapshot(Connection &connection)
{
    // `live ∩ R(c)`, once, and every pass below uses it instead of the live set.
    // Four passes, not three: the body-state section walks the set on its own
    // (see WriteBodyStates). Filtering happens strictly here, before priority —
    // the ordering every scaled system converged on, because a prioritizer that
    // also filters is two axes fused into one number.
    const std::vector<NetId> &effective = ComputeEffective(connection);

    Core::BitWriter writer;
    WriteMessageType(MessageType::Snapshot, writer);

    SnapshotHeader header;
    header.serverTick       = _simTick;
    header.baselineTick     = connection.ackedTick;
    header.inputBufferDepth = static_cast<std::uint32_t>(connection.input.Depth());
    header.starvedTicks     = static_cast<std::uint32_t>(connection.input.StarvedTicks());
    // Complete when the acked set already covers everything this connection is
    // *told* about — which is what "am I done joining?" means for a filtered
    // connection, and what it has always meant for an unfiltered one. Computed
    // against the previous ack rather than this snapshot, because that is the
    // only thing the client has actually confirmed receiving.
    header.worldComplete =
        std::includes(connection.acked.begin(), connection.acked.end(), effective.begin(), effective.end());
    WriteSnapshotHeader(header, writer);

    // Instance records, ahead of everything else in the packet. An instance has
    // to be named before any block that belongs to it, because the client
    // composes a member's baseline from the blueprint rather than from empty —
    // a block arriving first would have nothing to delta against.
    //
    // Emitted for every instance with a relevant member, not only ones whose
    // members won the priority budget this tick. A record for an instance whose
    // members are all starved is exactly right: the client expands it from the
    // blueprint immediately and the deltas catch up.
    std::vector<ECS::InstanceId> neededInstances;
    for (const NetId netId : effective)
    {
        const ECS::BlueprintMember *tag = _scene.Get<ECS::BlueprintMember>(EntityOf(netId));
        if (tag == nullptr || !tag->instanceId.IsValid())
            continue;
        // Only instances that actually got a block: one the provider could not
        // describe has members replicating as ordinary entities, and naming it
        // would point the client at a blueprint index nothing agreed on.
        if (!_instanceBlocks.contains(tag->instanceId))
            continue;
        neededInstances.push_back(tag->instanceId);
    }
    std::sort(neededInstances.begin(), neededInstances.end());
    neededInstances.erase(std::unique(neededInstances.begin(), neededInstances.end()), neededInstances.end());

    std::vector<ECS::InstanceId> freshInstances;
    std::set_difference(neededInstances.begin(), neededInstances.end(), connection.knownInstances.begin(),
                        connection.knownInstances.end(), std::back_inserter(freshInstances));

    // How many of them this snapshot can afford (B11). The section used to be
    // written whole, before either budget check, so a join carrying more fresh
    // instances than `maxSnapshotBytes` allows produced one oversized packet per
    // snapshot until it was acked, with the entity loop starved behind it.
    //
    // Sized from an upper bound rather than by writing and measuring: the count
    // prefix has to be written before the records, so how many go has to be
    // decided before any of them does. Three varints at their widest, the ten
    // raw placement floats, and the presence bits.
    std::size_t writableRecords = 0;
    {
        constexpr std::size_t kVarIntMaxBytes = 5;
        std::size_t projected = writer.BytesWritten() + kVarIntMaxBytes; // the count prefix
        for (const ECS::InstanceId instanceId : freshInstances)
        {
            const InstanceBlock  &block = _instanceBlocks.find(instanceId)->second;
            const std::size_t     cost  = 3 * kVarIntMaxBytes + 10 * sizeof(float) + 1 +
                                     (static_cast<std::size_t>(block.memberCount) + 7) / 8;
            // Always at least one. A record wider than the whole budget must
            // still make progress, or the instance is never named and its
            // members never arrive — a stall traded for one oversized packet is
            // not a trade.
            if (writableRecords != 0 && projected + cost > _config.maxSnapshotBytes)
                break;
            projected += cost;
            ++writableRecords;
        }
    }

    // The rest wait for the next snapshot, and so must their members: the
    // section's whole reason for being first is that a member block arriving
    // before its record has nothing to attribute itself to. Sorted, because the
    // entity loop below binary-searches it.
    const std::vector<ECS::InstanceId> deferredInstances(
        freshInstances.begin() + static_cast<std::ptrdiff_t>(writableRecords), freshInstances.end());

    // One bit, not a varint zero, when there is nothing to say. A game with no
    // blueprint instances — or a steady state where every record is acked, which
    // is nearly every snapshot — must not pay a byte per snapshot per connection
    // for a feature it is not using. The empty-snapshot byte budget in
    // TestRelevancy is what holds this honest.
    writer.WriteBool(writableRecords != 0);
    if (writableRecords != 0)
        writer.WriteVarUInt32(static_cast<std::uint32_t>(writableRecords));
    std::vector<std::uint8_t> presence; ///< Reused across the records below.
    for (std::size_t i = 0; i < writableRecords; ++i)
    {
        const InstanceBlock &block = _instanceBlocks.find(freshInstances[i])->second;
        writer.WriteVarUInt32(block.info.blueprintIndex);
        writer.WriteVarUInt32(block.base.value); // wire write
        writer.WriteVarUInt32(block.memberCount);

        // Which members actually exist (B8). The block's width is the
        // definition's count and never shrinks — the surviving members' ids must
        // not shift — so "how many ids this instance owns" and "which of them
        // have an entity behind them" are two different facts and the wire has
        // to carry both. One bit for the ordinary all-present case; a bit per
        // member only when the host really has a hole.
        presence.clear();
        presence.resize(block.memberCount, 0u);
        std::uint32_t presentCount = 0;
        for (std::uint32_t member = 0; member < block.memberCount; ++member)
        {
            if (!_entityByNetId.contains(NetId{block.base.value + member}))
                continue;
            presence[member] = 1u;
            ++presentCount;
        }

        const bool allPresent = presentCount == block.memberCount;
        writer.WriteBool(allPresent);
        if (!allPresent)
        {
            for (const std::uint8_t member : presence)
                writer.WriteBool(member != 0u);
        }

        // Full precision, not quantized like BodyState: both sides compose every
        // member's transform from this one, so a rounding difference here is a
        // difference in every member's pose, permanently, on a member that never
        // changes and is therefore never corrected.
        writer.WriteFloat(block.info.placement.position.x);
        writer.WriteFloat(block.info.placement.position.y);
        writer.WriteFloat(block.info.placement.position.z);
        writer.WriteFloat(block.info.placement.rotation.w);
        writer.WriteFloat(block.info.placement.rotation.x);
        writer.WriteFloat(block.info.placement.rotation.y);
        writer.WriteFloat(block.info.placement.rotation.z);
        writer.WriteFloat(block.info.placement.scale.x);
        writer.WriteFloat(block.info.placement.scale.y);
        writer.WriteFloat(block.info.placement.scale.z);
    }

    // Despawns: everything the client is known to have that it should no longer
    // have. Falls straight out of the same set difference that already found
    // destroyed entities — which is why leaving relevancy is not a second
    // mechanism. A revoke is a despawn, resent until acked, with the baseline
    // and priority entries erased when it lands.
    std::vector<NetId> despawns;
    std::set_difference(connection.acked.begin(), connection.acked.end(), effective.begin(), effective.end(),
                        std::back_inserter(despawns));
    // Run-length encoded, which is what the contiguous blocks buy: a destroyed
    // car is one run rather than one varint per wheel, and a revoked instance
    // leaving relevancy is the same shape. Ordinary entities rarely form runs
    // and cost one extra byte each for the length — the trade is deliberate,
    // since the case that matters is the one that scales with member count.
    //
    // `despawns` comes out of set_difference already ascending, which is what
    // makes a single pass enough.
    std::vector<std::pair<NetId, std::uint32_t>> despawnRuns;
    for (const NetId netId : despawns)
    {
        if (!despawnRuns.empty())
        {
            auto &[start, length] = despawnRuns.back();
            if (netId.value == start.value + length)
            {
                ++length;
                continue;
            }
        }
        despawnRuns.emplace_back(netId, 1u);
    }

    writer.WriteVarUInt32(static_cast<std::uint32_t>(despawnRuns.size()));
    for (const auto &[start, length] : despawnRuns)
    {
        writer.WriteVarUInt32(start.value); // wire write
        writer.WriteVarUInt32(length);
    }

    // Collect, order, drain to a budget. The ordering is a Tribes-style priority
    // accumulator: every live entity gains max(Replicated::priority, eps) each
    // snapshot tick, entities go out highest-first, and only the ones that
    // actually went reset. Under no budget pressure this is inert — everything
    // dirty goes every tick and everything resets. Under pressure, correction
    // *frequency* degrades smoothly and per object, steered by an authored
    // number, instead of by whichever NetId happened to be lowest.
    SentSnapshot record;
    record.serverTick = _simTick;
    // Cumulative, like netIds: what the client will know once this lands, not
    // the records that went out in it. The ones the byte budget left behind are
    // the exception — the client cannot know an instance whose record never
    // went, and claiming otherwise would retire the resend that owes it.
    std::set_difference(neededInstances.begin(), neededInstances.end(), deferredInstances.begin(),
                        deferredInstances.end(), std::back_inserter(record.instances));
    record.netIds.reserve(effective.size());
    record.written.reserve(effective.size());

    // Sampled once, before anything is written, and stamped onto every entity
    // this snapshot writes. Nothing mutates the scene while a snapshot is being
    // built, so one reading is honest for all of them — and taking it up front
    // means a change made after this point cannot be mistaken for delivered.
    const std::uint64_t captureTick = _scene.CurrentChangeTick();

    std::vector<std::pair<float, NetId>> order;
    order.reserve(effective.size());
    for (const NetId netId : effective)
    {
        const ECS::Entity entity = EntityOf(netId);
        if (entity == ECS::NullEntity)
            continue;

        float authored = 1.f;
        if (const Replicated *marker = _scene.Get<Replicated>(entity))
            authored = marker->priority;

        float &accumulator = connection.priority[netId];
        accumulator += std::max(authored, kMinPriorityGain);
        order.emplace_back(accumulator, netId);
    }
    // Highest first; NetId breaks ties so the order is stable rather than
    // whatever the float comparison happened to do.
    std::sort(order.begin(), order.end(),
              [](const auto &lhs, const auto &rhs)
              { return lhs.first != rhs.first ? lhs.first > rhs.first : lhs.second < rhs.second; });

    std::uint32_t backlog = 0;
    for (const auto &[accumulated, netId] : order)
    {
        (void)accumulated;
        const ECS::Entity entity = EntityOf(netId);
        if (entity == ECS::NullEntity)
            continue;

        // A member whose record the budget held back waits with it. Left out of
        // the record entirely, exactly like an entity the byte budget skipped
        // before the client ever knew it, so the next snapshot still treats it
        // as a spawn — and by then its record has gone ahead of it.
        if (!deferredInstances.empty())
        {
            const ECS::BlueprintMember *tag = _scene.Get<ECS::BlueprintMember>(entity);
            if (tag != nullptr && tag->instanceId.IsValid() &&
                std::binary_search(deferredInstances.begin(), deferredInstances.end(), tag->instanceId))
            {
                ++backlog;
                continue;
            }
        }

        const bool known = std::binary_search(connection.acked.begin(), connection.acked.end(), netId);

        if (writer.BytesWritten() >= _config.maxSnapshotBytes)
        {
            ++backlog;
            // Out of room. An entity the client already has simply misses this
            // update and stays in the record — its baseline is unaffected. One
            // it does not have must be left out of the record entirely, so the
            // next snapshot still treats it as a spawn.
            if (known)
            {
                record.netIds.push_back(netId);
                // Carry its component baseline forward untouched: we told the
                // client nothing about this entity, so nothing about it changed
                // from the client's point of view.
                //
                // ComponentId{0}: the packed key's lower bound, not "invalid" —
                // 0 is the lowest possible ordinal, and component ids are dense
                // from there.
                const auto low  = std::lower_bound(connection.ackedComponents.begin(),
                                                   connection.ackedComponents.end(),
                                                   PackComponentRef(netId, Core::Reflect::ComponentId{0}));
                // netId + 1: the exclusive upper bound of this netId's packed-ref
                // range, spelled explicitly since NetId has no arithmetic of its own.
                const auto high = std::lower_bound(connection.ackedComponents.begin(),
                                                   connection.ackedComponents.end(),
                                                   PackComponentRef(NetId{netId.value + 1}, Core::Reflect::ComponentId{0}));
                record.components.insert(record.components.end(), low, high);
            }
            continue;
        }

        // The delta baseline is this entity's own. A missing entry reads as 0,
        // which is the empty baseline — spawn, late join, and a post-sweep
        // re-anchor all arrive here, and all take the one full-state path.
        std::uint64_t sinceChangeTick = 0;
        if (known)
        {
            if (const auto baseline = connection.baselines.find(netId); baseline != connection.baselines.end())
                sinceChangeTick = baseline->second.componentTick;
        }

        writer.WriteBool(true);
        writer.WriteVarUInt32(netId.value); // wire write
        writer.WriteBool(!known); // isSpawn

        WriteEntityComponents(netId, entity, sinceChangeTick, connection, writer, record.components);
        record.netIds.push_back(netId);
        record.written.push_back(WrittenEntity{netId, EntityBaseline{captureTick, 0}});

        // It went out, so its turn is over. The ones that did not go keep what
        // they accumulated — which is the whole anti-starvation property.
        connection.priority[netId] = 0.f;
    }
    writer.WriteBool(false);
    connection.diagnostics.dirtyBacklog = backlog;

    // Priority order is not NetId order, and both the body pass and the ack path
    // binary-search these.
    std::sort(record.netIds.begin(), record.netIds.end());
    std::sort(record.written.begin(), record.written.end(),
              [](const WrittenEntity &lhs, const WrittenEntity &rhs) { return lhs.netId < rhs.netId; });

    // Motion, for everything the physics world owns. After the entity blocks so
    // ordering with a spawn in the same packet is free: the entity and its
    // descriptor exist by the time the body state lands, and the client's body
    // starts at the authoritative state rather than re-settling from the level
    // file's pose.
    const std::size_t writtenFromComponents = record.written.size();
    WriteBodyStates(connection, effective, writer, record, writtenFromComponents);

    // Last, after the entity blocks and the body states, so a message about an
    // entity established earlier *in this same packet* inherits its ordering
    // from the framing itself. That is the one genuine improvement the survey
    // found in replicon's tick-sync guarantee, and here it costs nothing: the
    // snapshot already carries the tick and the wire order already puts entity
    // blocks first.
    WriteEventSection(connection, writer, record);

    // All three are built in ascending order — except `written`, which the body
    // pass may have appended to — so keep the invariant explicit, since the ack
    // path binary-searches them.
    std::sort(record.components.begin(), record.components.end());
    std::sort(record.written.begin(), record.written.end(),
              [](const WrittenEntity &lhs, const WrittenEntity &rhs) { return lhs.netId < rhs.netId; });

    connection.inFlight.push_back(std::move(record));
    while (connection.inFlight.size() > _config.maxInFlightSnapshots)
    {
        // The client has stopped acking. Dropping the oldest bounds our memory;
        // its ack, if it ever arrives, will simply find nothing and be ignored.
        connection.inFlight.pop_front();
    }

    // Unreliable: a lost snapshot must not be retransmitted. It would arrive
    // after the state it describes had already been superseded, costing latency
    // to deliver data that is worse than the data behind it.
    _transport.Send(connection.id, writer.Data(), Net::SendMode::Unreliable, Net::Lane::Snapshot);

    ++connection.diagnostics.snapshotsSent;
    connection.diagnostics.bytesSent += writer.BytesWritten();
    connection.diagnostics.inFlightSnapshots = static_cast<std::uint32_t>(connection.inFlight.size());
}

void ReplicationServer::Tick(std::uint64_t simTick)
{
    _simTick = simTick;

    // Once per tick, before any connection is served, so every client sees the
    // same world rather than a per-connection view of it.
    ReconcileNetIds();
    CaptureBodyStates();

    if (!IsSnapshotTick(simTick))
        return;

    // The keyframe sweep, which is not a third mechanism: it is the delta path
    // with its filter reset. Every entity's baseline goes back to zero, the
    // existing empty-baseline code path sends full state, and the byte budget
    // paginates it over as many snapshots as it needs — no new machinery, and
    // nothing on the wire that spawn and late-join do not already exercise.
    if (_config.keyframeIntervalTicks != 0 && simTick != 0 && simTick % _config.keyframeIntervalTicks == 0)
    {
        for (auto &[id, connection] : _connections)
        {
            if (connection.ready)
                ResetBaselines(connection);
        }
    }

    for (auto &[id, connection] : _connections)
    {
        if (connection.ready)
            SendSnapshot(connection);
    }

    // End of tick: after every state mutation this tick produced, so a host-side
    // handler sees a world at least as new as the message it is handling — the
    // property a remote client gets free from packet ordering.
    DispatchHostEvents();
}

} // namespace Assisi::NetSync
