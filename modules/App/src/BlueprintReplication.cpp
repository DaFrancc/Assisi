/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/BlueprintReplication.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/NetSync/Replication.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <algorithm>
#include <memory>
#include <utility>

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
        const Runtime::BlueprintDefinition *definition = Runtime::GetBlueprintDefinition(row->source);
        if (definition == nullptr || definition->members.empty())
            return false;

        out.blueprintIndex = static_cast<std::uint32_t>(index);
        out.memberCount    = static_cast<std::uint32_t>(definition->members.size());
        out.placement      = row->transform;
        return true;
    }

  private:
    World                   &_world;
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

        // authored=false, for the same reason a runtime spawn is: this instance
        // exists because the server said so, and writing it into a saved level
        // would make it authored content the next time that level loads.
        std::optional<Runtime::SceneSerializer::ExpandedInstance> placed =
            Runtime::SceneSerializer::PlaceInstance(_world.scene, _world.instances, entry, /*authored=*/false);
        if (!placed.has_value())
            return false;

        // Parallel to the blueprint's member list, which is exactly the order
        // `base + i` indexes — the two orderings being the same one is the whole
        // reason the server never sends a member list.
        out.assign(placed->members.begin(), placed->members.end());
        outInstance = placed->instanceId;
        return true;
    }

  private:
    World                   &_world;
    std::vector<std::string> _manifest;
};

} // namespace

void InstallInstanceInfoProvider(NetSync::ReplicationServer &server, World &world,
                                 std::vector<std::string> manifest)
{
    server.SetInstanceInfoProvider(std::make_unique<WorldInstanceInfo>(world, std::move(manifest)));
}

void InstallInstanceExpander(NetSync::ReplicationClient &client, World &world,
                             std::vector<std::string> manifest)
{
    client.SetInstanceExpander(std::make_unique<WorldInstanceExpander>(world, std::move(manifest)));
}

} // namespace Assisi::App
