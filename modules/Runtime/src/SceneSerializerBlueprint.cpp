/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/Runtime/Blueprint.hpp>

#include <algorithm>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
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

/// Whether a component survives a codec round-trip with everything the level file
/// holds.
///
/// `norep` fields are saved to disk but never sent over the wire, so the codec
/// skips them — right for replication, wrong for a blueprint, whose file *is*
/// disk. Such a component keeps the slower JSON path instead of silently losing
/// the field. Nothing in the engine declares one today.
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

    // Saved and restored rather than refused: definitions are built lazily, and the
    // first caller to want one is usually a level load with its own name context
    // already open. Nesting is safe because this one resolves against a scratch scene
    // nothing else can see.
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

    // Declared, mapped at nothing: a member an in-file `removed` took out. A sibling
    // still naming it resolves to null with a warning rather than making the whole
    // file unusable — the same treatment a per-instance removal gets, and the reason
    // the two removal paths no longer disagree about how bad a removal is (§6).
    for (const std::string &removed : definition.removedMembers)
        s_context->nameToEntity.emplace(removed, ECS::NullEntity);

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
                // Refused here rather than at every spawn: this is the one place that
                // reads member values, and one the engine cannot read means the file
                // is broken about itself.
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
        const ECS::Entity e      = scratchEntities[i];

        for (const Core::Reflect::ComponentMeta *meta : registry.SerializableComponents())
        {
            const void *component = meta->getByEntity(&scratch, e.index, e.generation);
            if (component == nullptr || !IsCodecLossless(*meta))
                continue;

            Core::BitWriter writer;
            if (!Core::Reflect::WriteComponent(*meta, component, writer))
            {
                // A codec refusal is a reflection bug, not this blueprint's fault:
                // the member keeps that component on the JSON path.
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

    // A name is the prefix its members are addressed by, so two live instances of one
    // name mean two entities answering to `car/body`. Load refuses that outright, so a
    // level saved with both is a level that never opens again. Checked here because
    // this is the one door both editor gestures come through, and before the context
    // is engaged so a refusal leaves the scene untouched.
    //
    // Unnamed instances are exempt and must stay that way: a runtime spawn and a
    // replicated mirror both pass no name, nothing addresses their members by path,
    // and refusing the second would break replication.
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

    // Its own name context, holding only this instance's members: a placement outside
    // a level load has no file around it, and lending it the whole scene's names would
    // let a blueprint silently wire itself to whatever happened to share one.
    ScopedContextReset guard;
    s_context = SerializationContext{};

    StagedInstance instance;
    const auto unwind = [&]
                        {
                            for (const ECS::Entity member : instance.members)
                            {
                                if (member != ECS::NullEntity)
                                    scene.Destroy(member);
                            }
                            if (instance.id.IsValid())
                                table.Remove(instance.id);
                        };

    // Staging reports by value; the try is for CommitInstance, which runs the generated
    // deserializers and can still take an nlohmann throw. Either way it is all or
    // nothing: a nested file missing three members in must leave no partial instance
    // behind, and `instance` holds whatever got as far as existing.
    try
    {
        if (const std::expected<void, LevelError> ok =
                StageInstance(scene, table, entry, /*levelInstanceIndex=*/ -1, instance);
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

    // Authorship is decided here rather than inside StageInstance: it is a property of
    // *who asked*, not of the file.
    if (authored)
    {
        BlueprintInstance row = *table.Find(instance.id);
        row.authored          = true;
        table.RestoreAt(instance.id, std::move(row));
    }

    return ExpandedInstance{.instanceId = instance.id, .members = std::move(instance.members)};
}

// Members are matched by name and their entities *adopted* — stripped and rebuilt in
// place — rather than destroyed and recreated. Undo history and every entity reference
// in the level store exact `(slot, generation)` handles, so a rebuild would leave them
// pointing at free or reused slots.
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
    // A copy: staging reads the row back out of the table, and this has to outlive
    // anything that touches it.
    const BlueprintInstance row = *found;

    // Both failure checks sit here, before the first member is stripped: that is what
    // lets the contract promise "changed nothing" on an error. Past this point failure
    // is structurally unreachable — the definition loaded, so its member names are
    // unique, and the placement was accepted once already, so its scale is uniform.
    // The check must stay non-throwing for that to hold; GetBlueprintDefinition reports
    // every way a file can be bad as an error value.
    if (const BlueprintResult definition = GetBlueprintDefinition(row.source); !definition)
    {
        Core::Log::Error("Blueprint: '{}' no longer loads ({}); instance {} is left as it was.", row.source,
                         Describe(definition.error()), instanceId);
        return std::unexpected(LevelError::BlueprintUnusable);
    }

    // What is live now, under the names the *old* definition gave it. A tag whose index
    // the old list does not cover is skipped rather than guessed at: it means the caller
    // passed the wrong list, and adopting the wrong entity for a name would silently
    // rebuild one member on top of another.
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
            // Unreachable by the reasoning above, reported rather than swallowed if it
            // ever stops holding. No unwind is possible: the adopted members have
            // already been stripped, so all that is left is to say so loudly.
            Core::Log::Error("Blueprint: re-expanding '{}' failed part-way ({}). Reload the level.",
                             row.source, Describe(ok.error()));
            return std::unexpected(ok.error());
        }
        CommitInstance(scene, staged, row.name);
    }
    catch (const std::exception &ex)
    {
        // Same, for the throw CommitInstance can still take out of the generated
        // deserializers.
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
                                                                           InstanceTable &table,
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
        PlaceInstance(scene, table, entry, /*authored=*/ false);
    if (!placed)
        return std::unexpected(placed.error());
    return placed->instanceId;
}

} // namespace Assisi::Runtime
