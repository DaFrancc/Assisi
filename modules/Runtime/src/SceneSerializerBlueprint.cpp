/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/EditorOnly.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/NameComponent.hpp>
#include <Assisi/Runtime/Naming.hpp>

#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "SceneSerializerContext.hpp"
#include "SceneSerializerInstances.hpp"

// ---------------------------------------------------------------------------
// Preparing a blueprint, and placing or re-expanding an instance.
// ---------------------------------------------------------------------------

namespace Assisi::Runtime
{

namespace
{


/// Whether a component can be round-tripped through the codec without losing
/// anything the level file holds.
///
/// `norep` fields are saved to disk and deliberately never sent over the network,
/// so the codec skips them — which is right for replication and wrong for a
/// blueprint, where the file *is* disk. Such a component keeps the JSON path.
/// Nothing in the engine declares one today; the check is here so the day one
/// appears it costs a little speed rather than a silently missing field.
bool IsCodecLossless(const Core::Reflect::ComponentMeta &meta)
{
    for (const Core::Reflect::FieldMeta &field : meta.fields)
    {
        if (field.norep && !field.transient)
            return false;
    }
    return true;
}

} // namespace

bool SceneSerializer::PrepareBlueprint(BlueprintDefinition &definition)
{
    if (s_rawContextScene != nullptr)
    {
        Core::Log::Error("PrepareBlueprint: a raw-entity context is active on this thread.");
        return false;
    }

    const auto &registry = Core::Reflect::ComponentRegistry::Instance();

    ECS::Scene scratch;

    // Saved and restored rather than refused: a definition is built lazily, and the
    // first caller to want one is usually a level load that already has its own
    // name context open. Nesting is safe here because this one resolves against a
    // scratch scene that nothing else can see.
    std::optional<SerializationContext> outer = std::exchange(s_context, SerializationContext{});
    struct RestoreOuter
    {
        std::optional<SerializationContext> *outer;
        ~RestoreOuter() { s_context = std::move(*outer); }
    } const restore{&outer};

    // Every member first, so a reference can point forward — and in order, so
    // member i is entity {i, 0} and its packed handle *is* i.
    std::vector<ECS::Entity> scratchEntities;
    scratchEntities.reserve(definition.members.size());
    for (const BlueprintMemberDesc &member : definition.members)
    {
        const ECS::Entity e = scratch.Create();
        scratchEntities.push_back(e);
        s_context->nameToEntity.emplace(member.name, e);
    }

    for (std::size_t i = 0; i < definition.members.size(); ++i)
    {
        const BlueprintMemberDesc &member = definition.members[i];
        if (!member.components.is_object())
            continue;

        for (const auto &[componentName, componentData] : member.components.items())
        {
            const Core::Reflect::ComponentMeta *meta = registry.Find(componentName);
            if (meta == nullptr || !meta->serializable)
                continue; // reported at expansion, where there is an instance to name
            if (!meta->addToScene(&scratch, scratchEntities[i].index, scratchEntities[i].generation,
                                  componentData))
            {
                // Refused here rather than at every spawn: a blueprint whose member
                // values the engine cannot read is broken about itself, and this is
                // the one place that reads them.
                Core::Log::Error("Blueprint: '{}' member '{}' has an unreadable '{}'.", definition.source,
                                 member.name, componentName);
                return false;
            }
        }
    }

    if (!s_context->unresolvedRefNames.empty())
    {
        Core::Log::Error("Blueprint: '{}' has {} reference(s) naming an entity it does not declare; the first "
                         "is '{}'.",
                         definition.source, s_context->unresolvedRefNames.size(),
                         s_context->unresolvedRefNames.front());
        return false;
    }

    for (std::size_t i = 0; i < definition.members.size(); ++i)
    {
        BlueprintMemberDesc &member = definition.members[i];
        const ECS::Entity    e      = scratchEntities[i];

        for (const Core::Reflect::ComponentMeta *meta : registry.SerializableComponents())
        {
            const void *component = meta->getByEntity(&scratch, e.index, e.generation);
            if (component == nullptr || !IsCodecLossless(*meta))
                continue;

            Core::BitWriter writer;
            if (!Core::Reflect::WriteComponent(*meta, component, writer))
            {
                // A component the codec refuses is a reflection bug, but it is not
                // this blueprint's fault and the JSON path still works — so the
                // member simply keeps that component on the slow path.
                continue;
            }

            const std::span<const std::byte> bytes = writer.Data();
            member.prepared.push_back(PreparedComponent{
                .id = meta->id, .name = meta->name, .block = {bytes.begin(), bytes.end()}});
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Runtime expansion
// ---------------------------------------------------------------------------

std::expected<SceneSerializer::ExpandedInstance, LevelError>
SceneSerializer::PlaceInstance(ECS::Scene &scene, InstanceTable &table, const LevelInstance &entry,
                               bool authored)
{
    if (s_context || s_rawContextScene != nullptr)
    {
        Core::Log::Error("PlaceInstance: a serialization context is already active on this thread.");
        return std::unexpected(LevelError::ContextBusy);
    }

    // A name is the prefix its members are addressed by, so two live instances of
    // one name mean two entities answering to `car/body`. Load refuses that
    // outright ("'car/body' is claimed twice"), which makes a level saved with
    // both a level that never opens again — authored happily, lost on reopen.
    //
    // Here rather than at the two editor gestures, because this is the one door
    // they both come through: "Place instance" checked and
    // CreateBlueprintFromSelection did not, which is round-7 S17 and exactly what
    // a per-caller rule produces. Before the context is engaged and before a
    // single member exists, so a refusal leaves the scene untouched rather than
    // relying on the unwind below.
    //
    // Unnamed instances are exempt and must stay that way: a runtime spawn and a
    // replicated mirror both pass no name, nothing addresses their members by
    // path, and refusing the second bullet would break replication.
    if (!entry.name.empty())
    {
        for (const auto &[id, row] : table.All())
        {
            if (row->name == entry.name)
            {
                Core::Log::Error("Blueprint: an instance named '{}' is already live in this world; placing a "
                                 "second would make '{}/…' name two entities.",
                                 entry.name, entry.name);
                return std::unexpected(LevelError::NameAlreadyLive);
            }
        }
    }

    // Its own name context, holding only this instance's members. Placing one
    // outside a level load has no file around it, so there is nothing else its
    // references could name — and giving it the whole scene's names would let a
    // blueprint silently wire itself to whatever happened to share a name.
    ScopedContextReset guard;
    s_context = SerializationContext{};

    StagedInstance instance;
    const auto     unwind = [&]
    {
        for (const ECS::Entity member : instance.members)
        {
            if (member != ECS::NullEntity)
                scene.Destroy(member);
        }
        if (instance.id.IsValid())
            table.Remove(instance.id);
    };

    // Staging reports by value; the try is for CommitInstance, which runs the
    // generated deserializers and so can still take an nlohmann throw on a member
    // value the reflection layer chokes on. Either way it is all or nothing (§7):
    // a missing nested file three members in must leave no partial instance behind,
    // and `instance` now holds whatever got as far as existing.
    try
    {
        if (const std::expected<void, LevelError> ok =
                StageInstance(scene, table, entry, /*levelInstanceIndex=*/-1, instance);
            !ok)
        {
            unwind();
            return std::unexpected(ok.error());
        }
        CommitInstance(scene, instance, entry.name);
    }
    catch (const std::exception &ex)
    {
        Core::Log::Error("Blueprint: placing '{}' failed: {}", entry.source, ex.what());
        unwind();
        return std::unexpected(LevelError::BlueprintUnusable);
    }

    if (!s_context->unresolvedRefNames.empty())
    {
        Core::Log::Error("Blueprint: '{}' has {} reference(s) naming a member it does not declare; the first "
                         "is '{}'.",
                         entry.source, s_context->unresolvedRefNames.size(),
                         s_context->unresolvedRefNames.front());
        unwind();
        return std::unexpected(LevelError::UnresolvedReference);
    }

    // Authorship is decided here rather than inside StageInstance, because it is a
    // property of *who asked* rather than of the file.
    if (authored)
    {
        BlueprintInstance row = *table.Find(instance.id);
        row.authored          = true;
        table.RestoreAt(instance.id, std::move(row));
    }

    return ExpandedInstance{.instanceId = instance.id, .members = std::move(instance.members)};
}

std::expected<SceneSerializer::ReexpandedInstance, LevelError>
SceneSerializer::ReexpandInstance(ECS::Scene &scene, InstanceTable &table, ECS::InstanceId instanceId,
                                  std::span<const std::string> previousMemberNames)
{
    if (s_context || s_rawContextScene != nullptr)
    {
        Core::Log::Error("ReexpandInstance: a serialization context is already active on this thread.");
        return std::unexpected(LevelError::ContextBusy);
    }

    const BlueprintInstance *found = table.Find(instanceId);
    if (found == nullptr)
    {
        Core::Log::Error("Blueprint: cannot re-expand instance {} — no such instance is live.", instanceId);
        return std::unexpected(LevelError::InstanceNotLive);
    }
    // A copy. Staging reads the row back out of the table, and the entry below has to
    // outlive anything that touches it.
    const BlueprintInstance row = *found;

    // Both failure checks happen here, before the first member is stripped, which is
    // what lets the contract promise "changed nothing" on an error. Past this point a
    // failure is structurally unreachable: the definition loaded, so its member names
    // are unique, and the placement was accepted once already, so its scale is
    // uniform.
    //
    // The check itself has to be non-throwing for that to hold, and it is —
    // GetBlueprintDefinition reports every way a file can be bad as an error value,
    // including a member value the reflection layer refuses.
    if (const BlueprintResult definition = GetBlueprintDefinition(row.source); !definition)
    {
        Core::Log::Error("Blueprint: '{}' no longer loads ({}); instance {} is left as it was.", row.source,
                         Describe(definition.error()), instanceId);
        return std::unexpected(LevelError::BlueprintUnusable);
    }

    // What is live now, under the names the *old* definition gave it. A tag whose
    // index the old list does not cover is skipped rather than guessed at — it can
    // only mean the caller passed the wrong list, and adopting the wrong entity for a
    // name would silently rebuild one member on top of another.
    AdoptionSet adopt;
    adopt.instanceId = instanceId;
    for (const ECS::Entity member : MembersOf(scene, instanceId))
    {
        const ECS::BlueprintMember *tag = scene.Get<ECS::BlueprintMember>(member);
        if (tag == nullptr || tag->memberIndex >= previousMemberNames.size())
            continue;
        adopt.byName.emplace(previousMemberNames[tag->memberIndex], member);
    }

    const LevelInstance entry{.name      = row.name,
                              .source    = row.source,
                              .transform = row.transform,
                              .overrides = row.overrides,
                              .removed   = row.removed};

    ScopedContextReset guard;
    s_context = SerializationContext{};

    StagedInstance staged;
    try
    {
        if (const std::expected<void, LevelError> ok =
                StageInstance(scene, table, entry, row.levelInstanceIndex, staged, &adopt);
            !ok)
        {
            // Unreachable by the reasoning above, and reported rather than swallowed
            // if that reasoning ever stops holding. There is no unwind: the adopted
            // members have already been stripped, so the honest thing is to say so
            // loudly.
            Core::Log::Error("Blueprint: re-expanding '{}' failed part-way ({}). Reload the level.",
                             row.source, Describe(ok.error()));
            return std::unexpected(ok.error());
        }
        CommitInstance(scene, staged, row.name);
    }
    catch (const std::exception &ex)
    {
        // Same position, for the throw CommitInstance can still take out of the
        // generated deserializers.
        Core::Log::Error("Blueprint: re-expanding '{}' failed part-way: {}. Reload the level.", row.source,
                         ex.what());
        return std::unexpected(LevelError::BlueprintUnusable);
    }

    if (!s_context->unresolvedRefNames.empty())
    {
        Core::Log::Error("Blueprint: '{}' has {} reference(s) naming a member it does not declare; the first "
                         "is '{}'. They are null in instance {}.",
                         row.source, s_context->unresolvedRefNames.size(),
                         s_context->unresolvedRefNames.front(), instanceId);
    }

    ReexpandedInstance out;
    out.members = std::move(staged.members);

    // Whatever was never adopted is a member the edit deleted.
    out.destroyed.reserve(adopt.byName.size());
    for (const auto &[name, entity] : adopt.byName)
        out.destroyed.push_back(entity);
    std::sort(out.destroyed.begin(), out.destroyed.end(),
              [](ECS::Entity a, ECS::Entity b) { return a.index < b.index; });
    for (const ECS::Entity entity : out.destroyed)
        scene.Destroy(entity);

    return out;
}

std::expected<ECS::InstanceId, LevelError> SceneSerializer::ExpandInstance(ECS::Scene &scene,
                                                                          InstanceTable   &table,
                                                                          std::string_view source,
                                                                          const ECS::Transform &placement)
{
    // No instance name: the members are `body`, not `car_3/body`, because nothing
    // placed this one and no file addresses into it.
    const LevelInstance entry{.name      = {},
                              .source    = std::string{source},
                              .transform = placement,
                              .overrides = nlohmann::json::object(),
                              .removed   = {}};

    const std::expected<ExpandedInstance, LevelError> placed =
        PlaceInstance(scene, table, entry, /*authored=*/false);
    if (!placed)
        return std::unexpected(placed.error());
    return placed->instanceId;
}

} // namespace Assisi::Runtime
