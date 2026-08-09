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
// Staging and committing one blueprint instance.
// ---------------------------------------------------------------------------

namespace Assisi::Runtime
{

/// The name a saved entity is written under, before uniquing.
///
/// Runtime::Name is the entity's name — there is no second, file-only identity —
/// so an entity that has one keeps it and an entity that does not gets a
/// placeholder. Placeholders are stable across a round trip because Load writes
/// whatever it read back onto the entity, so the *next* save reads a real name
/// here and nothing shifts underneath an override.
std::string AuthoredName(ECS::Scene &scene, ECS::Entity entity)
{
    if (const Name *name = scene.Get<Name>(entity); name != nullptr && !name->value.View().empty())
        return std::string{name->value.View()};
    return "Entity";
}

/// @p base if nothing has claimed it, else `base_1`, `base_2`, … until something
/// is free. Claims the result in @p used.
///
/// Two entities really can share a Name today (it is a free-form label and always
/// has been), and a file where they do would be refused on load. Disambiguating
/// on the way out is what makes the format's uniqueness rule enforceable without
/// a migration step that could not have known which "Cube" was which. Deterministic
/// because the caller walks entities in the same sorted order the array uses, so
/// re-saving an unchanged scene produces byte-identical names.
std::string UniqueName(std::string base, std::unordered_set<std::string> &used)
{
    // A name has to survive a round trip through Runtime::Name, which truncates.
    // Truncating here instead means the file says exactly what the load will hold.
    if (base.size() > Core::kShortStringMax)
        base.resize(Core::kShortStringMax);

    if (used.insert(base).second)
        return base;

    for (uint32_t suffix = 1;; ++suffix)
    {
        std::string candidate = std::format("{}_{}", base, suffix);
        if (candidate.size() > Core::kShortStringMax)
        {
            // Make room for the suffix rather than dropping it: a truncated
            // duplicate is still a duplicate.
            const std::string tail = std::format("_{}", suffix);
            candidate = base.substr(0, Core::kShortStringMax - tail.size()) + tail;
        }
        if (used.insert(candidate).second)
            return candidate;
    }
}

/// The path name a live member is addressed by — `car_3/wheel_fl` — or empty if
/// the tag names an instance or a member index that no longer exists.
///
/// This is the walk BlueprintMember exists to be small enough to need: instance
/// table → source → the cached member list → `[memberIndex].name`.
std::string MemberPathName(const InstanceTable &instances, const ECS::BlueprintMember &tag)
{
    const BlueprintInstance *row = instances.Find(tag.instanceId);
    if (row == nullptr)
        return {};

    const BlueprintResult definition = GetBlueprintDefinition(row->source);
    if (!definition || tag.memberIndex >= (*definition)->members.size())
        return {};

    const std::string &leaf = (*definition)->members[tag.memberIndex].name;
    return row->name.empty() ? leaf : row->name + "/" + leaf;
}

namespace
{

/// Takes every serializable component off @p entity, so it can be rebuilt from a
/// definition with none of the previous version's leftovers.
///
/// The handle survives, which is the entire point: a member the author neither
/// deleted nor renamed keeps its exact (slot, generation), so every undo transaction
/// and every EntityRef pointing at it stays true.
///
/// Non-serializable state — a Jolt body, a resolved asset pointer — is deliberately
/// *not* touched here, because Runtime cannot see it. Removing it is the caller's
/// precondition (see SceneSerializer::ReexpandInstance).
void StripSerializable(ECS::Scene &scene, ECS::Entity entity)
{
    for (const Core::Reflect::ComponentMeta *meta :
         Core::Reflect::ComponentRegistry::Instance().SerializableComponents())
    {
        scene.RemoveById(entity, meta->id);
    }
}

/// The last segment of a member path — what the member is called in its own file,
/// and what fits in a Name.
std::string_view LeafName(std::string_view path)
{
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

/// Creates one instance's member entities and claims their names, applying no
/// components yet.
///
/// Two phases because a reference may point *forward*: a level entity may name
/// `car_3/body`, and a member may name a level entity. Deserializing anything
/// before every name exists resolves those to null and silently unwires the file.
///
/// Fails on a name collision — with a level entity, with another instance, or
/// with a member of one. Every one of them makes a reference ambiguous.
///
/// @param staged filled as the work happens, **including on failure**. A row is
///        added to @p table and entities are created before the last thing that
///        can fail, so the caller has to be able to unwind whatever got as far as
///        existing; handing that back is what makes "all or nothing" the caller's
///        to keep rather than a promise this function cannot make.
/// @param adopt when non-null, a re-expansion: the row already exists and members

} // namespace

std::expected<void, LevelError> StageInstance(ECS::Scene &scene, InstanceTable &table,
                                              const LevelInstance &entry, int32_t levelInstanceIndex,
                                              StagedInstance &staged, AdoptionSet *adopt)
{
    if (!HasUniformScale(entry.transform))
    {
        Core::Log::Error("Blueprint: instance '{}' has a non-uniform scale ({}, {}, {}); an instance may only "
                         "translate, rotate, or scale uniformly.",
                         entry.name, entry.transform.scale.x, entry.transform.scale.y,
                         entry.transform.scale.z);
        return std::unexpected(LevelError::NonUniformScale);
    }

    const BlueprintResult loaded = GetBlueprintDefinition(entry.source);
    if (!loaded)
    {
        // The reason comes from the definition rather than being invented here: the
        // file that actually failed may be several levels of nesting below
        // `entry.source`, and this call site cannot know which.
        Core::Log::Error("Blueprint: instance '{}' cannot use '{}': {}.", entry.name, entry.source,
                         Describe(loaded.error()));
        return std::unexpected(LevelError::BlueprintUnusable);
    }
    const std::shared_ptr<const BlueprintDefinition> &definition = *loaded;

    staged.definition = definition;
    staged.placement  = entry.transform;
    staged.overrides  = entry.overrides;
    // A re-expansion keeps its row exactly as it is: the placement, the overrides and
    // the removals belong to the level that placed this instance, and the file being
    // edited has nothing to say about any of them.
    staged.id = adopt != nullptr
                    ? adopt->instanceId
                    : table.Add(BlueprintInstance{.name      = entry.name,
                                                  .source    = definition->source,
                                                  .transform = entry.transform,
                                                  // Anything the loader places came out of a file, so it is
                                                  // authored by construction. A placement made outside a load
                                                  // decides for itself — see PlaceInstance.
                                                  .authored           = levelInstanceIndex >= 0,
                                                  .levelInstanceIndex = levelInstanceIndex,
                                                  .overrides          = entry.overrides,
                                                  .removed            = entry.removed});

    staged.members.reserve(definition->members.size());
    staged.resolved.reserve(definition->members.size());
    for (uint32_t i = 0; i < definition->members.size(); ++i)
    {
        const BlueprintMemberDesc &desc = definition->members[i];
        const std::string          path = entry.name.empty() ? desc.name : entry.name + "/" + desc.name;

        if (IsMemberRemoved(desc.name, entry.removed))
        {
            // The hole. Its name is still claimed, mapped at nothing: a reference
            // to a removed member then resolves to null with a warning, rather than
            // refusing the file as an unknown name would.
            staged.members.push_back(ECS::NullEntity);
            staged.resolved.emplace_back();
            if (!s_context->nameToEntity.emplace(path, ECS::NullEntity).second)
            {
                Core::Log::Error("SceneSerializer: '{}' is claimed twice.", path);
                return std::unexpected(LevelError::DuplicateName);
            }
            continue;
        }

        // A member the edit left alone keeps its entity — see AdoptionSet. Stripped
        // rather than patched, because the new definition may simply not declare a
        // component the old one did, and a patch cannot express an absence.
        ECS::Entity e = ECS::NullEntity;
        if (adopt != nullptr)
        {
            if (const auto it = adopt->byName.find(desc.name); it != adopt->byName.end())
            {
                e = it->second;
                adopt->byName.erase(it);
                StripSerializable(scene, e);
            }
        }
        if (e == ECS::NullEntity)
            e = scene.Create();

        staged.members.push_back(e);
        staged.resolved.push_back(desc);

        if (!s_context->nameToEntity.emplace(path, e).second)
        {
            Core::Log::Error("SceneSerializer: '{}' is claimed twice.", path);
            return std::unexpected(LevelError::DuplicateName);
        }

        // The leaf, not the path: a Name is what the member's own file calls it,
        // and a path would not fit anyway. The path is derived from the tag when
        // something needs it.
        (void)scene.Add(e, Name{Core::ShortString{LeafName(desc.name)}});
        (void)scene.Add(e, ECS::BlueprintMember{.instanceId = staged.id, .memberIndex = i});
    }

    // This instance's own claims, on top of whatever the file already resolved.
    // Outermost wins per field, which is what makes a lot's colour and a level's
    // radius both apply to the same wheel.
    for (const auto &[memberPath, componentOverrides] : entry.overrides.items())
    {
        const std::optional<uint32_t> index = definition->IndexOf(memberPath);
        if (!index.has_value())
        {
            Core::Log::Warn("Blueprint: instance '{}' overrides '{}', which '{}' does not declare — dropped.",
                            entry.name.empty() ? definition->source : entry.name, memberPath,
                            definition->source);
            continue;
        }
        if (staged.members[*index] == ECS::NullEntity)
            continue; // overriding a member this instance removed: nothing to apply it to

        ApplyMemberOverride(staged.resolved[*index], componentOverrides,
                            entry.name.empty() ? memberPath : entry.name + "/" + memberPath);
    }

    return {};
}

/// Applies a staged instance's components, then composes the placement onto every
/// member the placement actually reaches.
void CommitInstance(ECS::Scene &scene, const StagedInstance &staged, std::string_view instanceName)
{
    const auto &registry = Core::Reflect::ComponentRegistry::Instance();

    // The definition's references are qualified for the nesting *inside* the file;
    // the instance's own name is only known here. Without this second pass a wheel
    // still says its parent is `body`, and with two cars placed, `body` names
    // whichever one answered first.
    const std::string prefix = instanceName.empty() ? std::string{} : std::string{instanceName} + "/";

    // A prepared block's EntityRefs are member *indices*; this turns them into this
    // instance's handles. It is why nothing has to walk a decoded component looking
    // for references afterwards — the codec's own hook does it during the read.
    Core::Reflect::CodecContext codec;
    codec.entityFromWire = [&staged](uint64_t packed) -> uint64_t
    {
        const auto index = static_cast<uint32_t>(packed & 0xFFFFFFFFull);
        if (packed == PackEntity(ECS::NullEntity) || index >= staged.members.size())
            return PackEntity(ECS::NullEntity);
        return PackEntity(staged.members[index]);
    };

    for (std::size_t i = 0; i < staged.members.size(); ++i)
    {
        const ECS::Entity e = staged.members[i];
        if (e == ECS::NullEntity)
            continue; // a member this instance removed

        const BlueprintMemberDesc &desc = staged.resolved[i];

        // Which components this instance had something to say about. Those fall
        // back to the JSON below, because an override is a patch and the codec has
        // no patch: a block is full state, so decoding it would overwrite the very
        // fields the override just set.
        std::unordered_set<std::string> claimed;
        if (const auto it = staged.overrides.find(desc.name); it != staged.overrides.end() && it->is_object())
        {
            for (const auto &[componentName, claim] : it->items())
                claimed.insert(componentName);
        }

        for (const PreparedComponent &prepared : staged.definition->members[i].prepared)
        {
            if (claimed.contains(prepared.name))
                continue;
            // A component the instance removed is simply absent from `components`,
            // so it is absent here too.
            if (!desc.components.contains(prepared.name))
                continue;

            const Core::Reflect::ComponentMeta *meta = registry.Find(prepared.name);
            if (meta == nullptr)
                continue;

            void *component = meta->construct(&scene, e.index, e.generation);
            if (component == nullptr)
                continue;

            Core::BitReader reader{prepared.block};
            (void)Core::Reflect::ReadComponentId(reader); // the block leads with it
            if (!Core::Reflect::ReadComponent(*meta, component, reader, /*appliedMask=*/nullptr, &codec))
            {
                Core::Log::Error("Blueprint: '{}' member '{}' failed to decode its '{}' block.",
                                 staged.definition->source, desc.name, prepared.name);
            }
        }

        if (desc.components.is_object())
        {
            for (const auto &[componentName, componentData] : desc.components.items())
            {
                // Already decoded, and faster than this path would have been.
                const bool wasPrepared =
                    !claimed.contains(componentName) &&
                    std::any_of(staged.definition->members[i].prepared.begin(),
                                staged.definition->members[i].prepared.end(),
                                [&](const PreparedComponent &p) { return p.name == componentName; });
                if (wasPrepared)
                    continue;

                const Core::Reflect::ComponentMeta *meta = registry.Find(componentName);
                if (meta == nullptr || !meta->serializable)
                {
                    Core::Log::Warn("Blueprint: '{}' member '{}' names component '{}', which this build does "
                                    "not have — skipped.",
                                    staged.definition->source, desc.name, componentName);
                    continue;
                }

                // Qualified through a one-key wrapper because the qualifier takes a
                // component *set* — it has to look the component up to know which of
                // its fields are references.
                nlohmann::json wrapper{{componentName, componentData}};
                QualifyInstanceReferences(wrapper, prefix);
                // The definition was read once and prepared, and PrepareBlueprint
                // refuses a member whose values do not read — so a failure here is
                // a claim the *instance* wrote, and the member simply does not get
                // that component rather than the whole expansion collapsing.
                if (!meta->addToScene(&scene, e.index, e.generation, wrapper.at(componentName)))
                {
                    Core::Log::Error("Blueprint: instance '{}' member '{}' overrides '{}' with something "
                                     "unreadable — the component is left as the blueprint had it.",
                                     instanceName, staged.resolved[i].name, componentName);
                }
            }
        }

        // An override this instance wrote may have flipped `parented` after the
        // definition was flattened *and* prepared. The prepared block is the
        // authority for a component the instance did not claim, so correcting the
        // JSON is not enough — the nesting placement is baked into the bytes that
        // were just decoded, and it comes off here or not at all.
        if (const bool bakedIn = !staged.definition->members[i].parented; bakedIn != !desc.parented)
        {
            if (ECS::Transform *transform = scene.GetMut<ECS::Transform>(e))
            {
                *transform = desc.parented ? InverseComposeTransform(desc.placement, *transform)
                                           : ComposeTransform(desc.placement, *transform);
            }
        }

        // The root is placement and only placement, and it does not exist after
        // expansion (§3): a parentless member's Transform ends up in world space.
        // A parented one is relative to a member that already absorbed the
        // placement, so composing again would apply it twice.
        if (!desc.parented)
        {
            if (ECS::Transform *transform = scene.GetMut<ECS::Transform>(e))
                *transform = ComposeTransform(staged.placement, *transform);
        }
    }
}

} // namespace Assisi::Runtime
