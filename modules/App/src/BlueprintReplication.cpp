/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/BlueprintReplication.hpp>

#include <Assisi/App/SystemCatalog.hpp>
#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/ComponentId.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/InstanceRecord.hpp>
#include <Assisi/NetSync/NetSession.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>
#include <Assisi/NetSync/ReplicationProviders.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Assisi::App
{
namespace
{

/// The wire index of @p source, or npos. The manifest is sorted, which is what
/// makes this a search rather than a scan and what makes the index mean the same
/// thing on both machines.
std::size_t IndexOf(const std::vector<std::string> &manifest, const std::string &source)
{
    const auto it = std::lower_bound(manifest.begin(), manifest.end(), source);
    if (it == manifest.end() || *it != source)
        return static_cast<std::size_t>(-1);
    return static_cast<std::size_t>(it - manifest.begin());
}

/// Whether @p manifest can be used as the wire naming, complaining if not.
///
/// IndexOf binary-searches it, and the far side reads back an index into its own
/// copy. Unsorted, both halves of that are wrong — silently, because a blueprint
/// of the same member count passes NetSync's only structural check and the client
/// expands the wrong file. Declining is the loud version of what an unlisted
/// blueprint already does: the members replicate one by one, larger and correct.
bool ManifestIsUsable(const std::vector<std::string> &manifest)
{
    if (std::is_sorted(manifest.begin(), manifest.end()))
        return true;

    Core::Log::Error("BlueprintReplication: the content manifest is not sorted; blueprint instances will "
                     "replicate member by member.");
    return false;
}

bool SameBytes(std::span<const std::byte> left, std::span<const std::byte> right)
{
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
}

/// Encodes a member's prepared Transform block with @p placement composed onto it.
///
/// Forward, not inverse-composing the live component: the pair is an exact inverse
/// in arithmetic but not in float, and this ends in a byte comparison. Repeating
/// the client's operation on the client's operands is what makes it exact.
bool WritePlacedTransform(const Core::Reflect::ComponentMeta &meta,
                          const std::vector<std::byte> &authored, const ECS::Transform &placement,
                          Core::BitWriter &out)
{
    Core::BitReader reader{authored};
    (void)Core::Reflect::ReadComponentId(reader); // the block leads with it
    ECS::Transform local;
    if (!Core::Reflect::ReadComponent(meta, &local, reader, /*appliedMask=*/ nullptr, nullptr))
        return false;

    const ECS::Transform placed = Runtime::ComposeTransform(placement, local);
    return Core::Reflect::WriteComponent(meta, &placed, out, Core::Reflect::kAllFields, nullptr);
}

class WorldInstanceInfo final : public NetSync::InstanceInfoProvider
{
public:
    WorldInstanceInfo(World &world, std::vector<std::string> manifest)
        : _world(world), _manifest(std::move(manifest))
    {
    }

    [[nodiscard]] bool Describe(ECS::InstanceId instanceId, NetSync::InstanceInfo &out) override
    {
        const Runtime::BlueprintInstance *row = _world.instances.Find(instanceId);
        if (row == nullptr)
            return false;

        const std::size_t index = IndexOf(_manifest, row->source);
        if (index == static_cast<std::size_t>(-1))
        {
            // Spawned from a blueprint the content set does not list — which the
            // join never agreed on, so an index would name a different file over
            // there. Its members replicate individually instead, which is right
            // rather than merely safe.
            Core::Log::Warn("BlueprintReplication: '{}' is not in the content set; instance {} replicates "
                            "member by member",
                            row->source, instanceId);
            return false;
        }

        // The member count has to be the definition's, not a count of live
        // members: it fixes the width of the NetId block, and a member destroyed
        // later must not shrink the block its siblings were numbered from.
        const Runtime::BlueprintResult definition = Runtime::GetBlueprintDefinition(row->source);
        if (!definition || (*definition)->members.empty())
            return false;

        out.blueprintIndex = static_cast<std::uint32_t>(index);
        out.memberCount    = static_cast<std::uint32_t>((*definition)->members.size());
        out.placement      = row->transform;
        return true;
    }

    [[nodiscard]] bool MatchesAuthored(ECS::InstanceId instanceId, std::uint32_t memberIndex,
                                       Core::Reflect::ComponentId id, const void *component) override
    {
        const Runtime::BlueprintInstance *row = _world.instances.Find(instanceId);
        if (row == nullptr)
            return false;

        const Runtime::BlueprintResult definition = Runtime::GetBlueprintDefinition(row->source);
        if (!definition || memberIndex >= (*definition)->members.size())
            return false;

        // An overridden member is not the file's member. The override is recorded
        // in the *level*, which a client joining mid-session never read, so its
        // expansion produced the file's value and the override has to travel as
        // ordinary state.
        if (row->overrides.contains((*definition)->members[memberIndex].name))
            return false;

        // The prepared block is what the client decoded when it expanded, so it is
        // the right operand to compare against rather than a re-derivation that
        // could drift from it. It is not always the *whole* operand: see the
        // placement composition below, which the expansion applies afterwards.
        const std::vector<std::byte> *authored = nullptr;
        for (const Runtime::PreparedComponent &prepared : (*definition)->members[memberIndex].prepared)
        {
            if (prepared.id == id)
            {
                authored = &prepared.block;
                break;
            }
        }
        if (authored == nullptr)
            return false; // the blueprint does not declare it; the client cannot have derived it

        const Core::Reflect::ComponentMeta *meta = Core::Reflect::ComponentRegistry::Instance().ById(id);
        if (meta == nullptr)
            return false;

        // Encoded the same way the prepared form was, with no codec context:
        // a component holding an entity reference encodes a local handle here and
        // a *member index* there, so it can never compare equal and simply
        // travels as state. Correct, and the case is rare enough not to chase.
        Core::BitWriter live;
        if (!Core::Reflect::WriteComponent(*meta, component, live, Core::Reflect::kAllFields, nullptr))
            return false;

        // Expansion composes the placement onto every *parentless* member's
        // Transform (SceneSerializerInstances.cpp), so the block alone is only the
        // authored local. Away from the origin it matches nothing — and
        // matches exactly when the member has been moved *to* that local, eliding
        // the one value the far side did not already have.
        //
        // A parented member is relative to one that already absorbed the placement,
        // so composing would apply it twice. No other component is rewritten after
        // it is decoded.
        if (id == Core::Reflect::ComponentIdOf<ECS::Transform>() &&
            !(*definition)->members[memberIndex].parented)
        {
            Core::BitWriter placed;
            if (!WritePlacedTransform(*meta, *authored, row->transform, placed))
                return false;
            return SameBytes(placed.Data(), live.Data());
        }

        return SameBytes(*authored, live.Data());
    }

private:
    World &_world;
    std::vector<std::string> _manifest;
};

class WorldInstanceExpander final : public NetSync::InstanceExpander
{
public:
    WorldInstanceExpander(World &world, std::vector<std::string> manifest)
        : _world(world), _manifest(std::move(manifest))
    {
    }

    [[nodiscard]] bool Expand(const NetSync::InstanceRecord &record, std::vector<ECS::Entity> &out,
                              ECS::InstanceId &outInstance) override
    {
        if (record.blueprintIndex >= _manifest.size())
        {
            // The two content sets disagree despite the handshake having passed,
            // which is a broken invariant rather than a missing file.
            Core::Log::Error("BlueprintReplication: blueprint index {} is outside a content set of {}",
                             record.blueprintIndex, _manifest.size());
            return false;
        }

        Runtime::LevelInstance entry;
        entry.source    = _manifest[record.blueprintIndex];
        entry.transform = record.placement;
        // No name: a name is what a *file* calls an instance, and no file placed
        // this one. No overrides either — the server sends member state as
        // ordinary deltas, so an override here would be a second, conflicting
        // source of truth for the same fields.

        // **PlaceInstance, deliberately not App::SpawnBlueprint.** The verb also
        // builds Jolt bodies; this must not. A mirrored member has no authority
        // over its own motion — that arrives as body state and the client raises
        // a mirror body for it through the ordinary path — so giving it a
        // simulated body here would have two things moving one entity and the
        // solver arguing with the server every tick.
        //
        // authored=false, for the same reason a runtime spawn is: this instance
        // exists because the server said so, and writing it into a saved level
        // would make it authored content the next time that level loads.
        const std::expected<Runtime::SceneSerializer::ExpandedInstance, Runtime::LevelError> placed =
            Runtime::SceneSerializer::PlaceInstance(_world.scene, _world.instances, entry, /*authored=*/ false);
        if (!placed)
            return false;

        // The systems the blueprint names, queued for the next safe point —
        // exactly what SpawnBlueprint does on the host, and for a sharper reason
        // here. The client never ran the spawn, so without this a client-side
        // system the content needs (effects, prediction, audio) silently does not
        // run: "a component whose system was never installed just does nothing",
        // now across machines rather than across a spawn.
        //
        // A union, and idempotent — Install skips what is already present — so a
        // hundred cars arriving install one Bounce.
        if (const Runtime::BlueprintResult definition = Runtime::GetBlueprintDefinition(entry.source))
            QueueSystemInstall(_world, (*definition)->systems, entry.source);

        // Parallel to the blueprint's member list, which is exactly the order
        // `base + i` indexes — the two orderings being the same one is the whole
        // reason the server never sends a member list.
        out.assign(placed->members.begin(), placed->members.end());

        // Drawable, not merely present — the same resolve SpawnBlueprint runs, for
        // the same reason: expansion produces the members from the prepared form,
        // which holds asset *ids*, and the draw path reads the transient pointers
        // those resolve to. An expansion that skips it mirrors the world correctly
        // and renders none of it — invisible in the editor, which re-resolves the
        // whole scene when the client's structure revision moves (EditorApp.cpp),
        // and total in a build with no editor in it.
        //
        // A no-op on a headless client, which is the same answer SpawnBlueprint
        // gives: no cache, nothing to resolve onto, and no entity left worse off.
        ResolveEntityAssets(_world, placed->members);
        outInstance = placed->instanceId;
        return true;
    }

    void Collapse(ECS::InstanceId localInstance) override
    {
        // The row, and only the row. `PlaceInstance` added it and nothing else
        // here can: NetSync destroyed the members and took their mirror bodies
        // out of the simulation before calling this, and App::DestroyInstance
        // over the same members would find them still alive — Scene::Destroy is
        // deferred — and hand each one's Jolt body to RemoveBody a second time.
        //
        // Which is the division InstanceTable::Remove already draws: the members
        // are the caller's business, and a row outliving them is the leak this
        // exists to close. It is what the viewport draws an instance icon from
        // and what PickInstance ray-tests, so one left behind is a ghost that is
        // still clickable.
        _world.instances.Remove(localInstance);
    }

private:
    World &_world;
    std::vector<std::string> _manifest;
};

} // namespace

void InstallInstanceInfoProvider(NetSync::ReplicationServer &server, World &world,
                                 std::vector<std::string> manifest)
{
    if (!ManifestIsUsable(manifest))
        return;
    server.SetInstanceInfoProvider(std::make_unique<WorldInstanceInfo>(world, std::move(manifest)));
}

void InstallInstanceExpander(NetSync::ReplicationClient &client, World &world,
                             std::vector<std::string> manifest)
{
    if (!ManifestIsUsable(manifest))
        return;
    client.SetInstanceExpander(std::make_unique<WorldInstanceExpander>(world, std::move(manifest)));
}

void ApplyContentSet(NetSync::NetSession &session, World &world, ContentSet content)
{
    // The hash first and unconditionally: it is what releases a withheld hello,
    // and a session that never receives it never joins at all. Whatever the
    // manifest turns out to be, refusing the join is not this function's call.
    session.SetContentSetHash(content.hash);

    if (NetSync::ReplicationServer *server = session.Server())
        InstallInstanceInfoProvider(*server, world, content.paths);
    if (NetSync::ReplicationClient *client = session.Client())
        InstallInstanceExpander(*client, world, std::move(content.paths));
}

} // namespace Assisi::App
