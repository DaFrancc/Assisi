/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/NameComponent.hpp>
#include <Assisi/Runtime/Naming.hpp>

#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

#include "SceneSerializerContext.hpp"
#include "SceneSerializerInstances.hpp"

// ---------------------------------------------------------------------------
// Staging and committing one blueprint instance.
// ---------------------------------------------------------------------------

namespace Assisi::Runtime
{
namespace
{

/// Spelled once so the two loops that skip it cannot drift apart.
constexpr std::string_view kNameComponent = "Name";

/// Says a member's declared Name was not honoured, and why.
///
/// The level path refuses the same thing for the same reason
/// (SceneSerializerLevel.cpp): the `name` key is the address, not a suggestion.
/// A member's address is its path — `car_3/body` is what an override, a
/// reference and the Inspector all spell — so a Name that displaced the leaf
/// would leave the member answering to something nothing in the level can reach
/// it by. Warned rather than dropped in silence, because the author wrote a
/// declaration that is not going to happen (round-7 S23).
void WarnNameIgnored(std::string_view source, std::string_view memberName)
{
    Core::Log::Warn("Blueprint: '{}' member '{}' declares a Name component; the member's own name is "
                    "authoritative and this one is ignored.",
                    source, memberName);
}

} // namespace

/// The name a saved entity is written under, before uniquing.
///
/// Runtime::Name is the entity's only identity — there is no file-only one — so a
/// nameless entity gets a placeholder. Load writes back whatever it read, so the
/// *next* save finds a real name here and nothing shifts underneath an override.
std::string AuthoredName(ECS::Scene &scene, ECS::Entity entity)
{
    if (const Name *name = scene.Get<Name>(entity); name != nullptr && !name->value.View().empty())
        return std::string{name->value.View()};
    return "Entity";
}

/// @p base if nothing has claimed it, else `base_1`, `base_2`, … until something
/// is free. Claims the result in @p used.
///
/// Two entities really can share a Name — it is a free-form label — and a file
/// where they do is refused on load, so uniquing on the way out is what makes the
/// format's rule enforceable. Deterministic: the caller walks entities in the same
/// sorted order the array uses, so re-saving an unchanged scene produces
/// byte-identical names.
std::string UniqueName(std::string base, std::unordered_set<std::string> &used)
{
    // Runtime::Name truncates, so truncate here too: the file then says exactly
    // what the load will hold.
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
/// The handle survives, which is the point: a member the author neither deleted nor
/// renamed keeps its exact (slot, generation), so every undo transaction and every
/// EntityRef pointing at it stays true.
///
/// Non-serializable state — a Jolt body, a resolved asset pointer — is deliberately
/// *not* touched, because Runtime cannot see it. Removing it is the caller's
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

} // namespace

/// Creates one instance's member entities and claims their names, applying no
/// components yet.
///
/// Two phases because a reference may point *forward*: a level entity may name
/// `car_3/body`, and a member may name a level entity. Deserializing anything
/// before every name exists resolves those to null and silently unwires the file.
///
/// Fails on a name collision — with a level entity, with another instance, or with
/// a member of one. Every one of them makes a reference ambiguous.
///
/// @param staged filled as the work happens, **including on failure**: a row is
///        added to @p table and entities are created before the last thing that can
///        fail, so on error the caller must unwind whatever got as far as existing.
///        "All or nothing" is the caller's to keep, not a promise made here.
/// @param adopt when non-null, a re-expansion: the row already exists and members
///        are taken over from it by name instead of being created.
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
        // The reason comes from the definition, not from here: the file that
        // actually failed may be nested several levels below `entry.source`.
        Core::Log::Error("Blueprint: instance '{}' cannot use '{}': {}.", entry.name, entry.source,
                         Describe(loaded.error()));
        return std::unexpected(LevelError::BlueprintUnusable);
    }
    const std::shared_ptr<const BlueprintDefinition> &definition = *loaded;

    staged.definition = definition;
    staged.placement  = entry.transform;
    staged.overrides  = entry.overrides;
    // A re-expansion keeps its row exactly as it is: placement, overrides and
    // removals belong to the level that placed the instance, not to the file being
    // edited.
    staged.id = adopt != nullptr
                    ? adopt->instanceId
                    : table.Add(BlueprintInstance{.name      = entry.name,
                                                  .source    = definition->source,
                                                  .transform = entry.transform,
                                                  // Anything the loader places came out of a file, so it is
                                                  // authored by construction; a placement made outside a
                                                  // load decides for itself (see PlaceInstance).
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
            // The hole. Its name is still claimed, mapped at nothing, so a
            // reference to a removed member resolves to null with a warning rather
            // than refusing the file the way an unknown name would.
            staged.members.push_back(ECS::NullEntity);
            staged.resolved.emplace_back();
            if (!s_context->nameToEntity.emplace(path, ECS::NullEntity).second)
            {
                Core::Log::Error("SceneSerializer: '{}' is claimed twice.", path);
                return std::unexpected(LevelError::DuplicateName);
            }
            continue;
        }

        // A member the edit left alone keeps its entity (see AdoptionSet). Stripped
        // rather than patched: the new definition may not declare a component the
        // old one did, and a patch cannot express an absence.
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
        // and a path would not fit. MemberPathName rebuilds the path from the tag.
        (void)scene.Add(e, Name{Core::ShortString{LeafName(desc.name)}});
        (void)scene.Add(e, ECS::BlueprintMember{.instanceId = staged.id, .memberIndex = i});
    }

    // The other half of the same rule, for the removals baked into the file rather
    // than written on this placement: their names are claimed here too, mapped at
    // nothing. A level that still names one — in an entity's reference or in an
    // override's — is a level that has not caught up with a file it does not own,
    // which nulls with a warning. Refusing it would mean deleting a member of a
    // blueprint can break levels the author never opened.
    for (const std::string &removed : definition->removedMembers)
    {
        const std::string path = entry.name.empty() ? removed : entry.name + "/" + removed;
        if (!s_context->nameToEntity.emplace(path, ECS::NullEntity).second)
        {
            Core::Log::Error("SceneSerializer: '{}' is claimed twice.", path);
            return std::unexpected(LevelError::DuplicateName);
        }
    }

    // This instance's own claims, on top of whatever the file already resolved.
    // Outermost wins per field, so a lot's colour and a level's radius can both
    // apply to the same wheel.
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
    // the instance's own name is only known here. Without this pass a wheel still
    // says its parent is `body`, and with two cars placed `body` names whichever
    // one answered first.
    const std::string prefix = instanceName.empty() ? std::string{} : std::string{instanceName} + "/";

    // A prepared block's EntityRefs are member *indices*; this turns them into this
    // instance's handles during the read, so nothing has to walk decoded components
    // looking for references afterwards.
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

        // Which components this instance had something to say about. Those skip the
        // prepared blocks and take the JSON path below: an override is a patch, and
        // a block is full state, so decoding it would overwrite what the override
        // just set.
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

            // Checked after the removal above, so a Name something already took
            // off the member is dropped in silence — there is nothing left to
            // warn about — while one the file still declares is refused out loud.
            if (prepared.name == kNameComponent)
            {
                WarnNameIgnored(staged.definition->source, desc.name);
                continue;
            }

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

                // Reached by an override claiming a Name, which skips the prepared
                // block above and lands here. Scene::Add refuses an occupied slot,
                // so this one is a no-op rather than a rename — but a claim that
                // silently does nothing is what the author needs telling about.
                if (componentName == kNameComponent)
                {
                    WarnNameIgnored(staged.definition->source, desc.name);
                    continue;
                }

                const Core::Reflect::ComponentMeta *meta = registry.Find(componentName);
                if (meta == nullptr || !meta->serializable)
                {
                    Core::Log::Warn("Blueprint: '{}' member '{}' names component '{}', which this build does "
                                    "not have — skipped.",
                                    staged.definition->source, desc.name, componentName);
                    continue;
                }

                // A one-key wrapper because the qualifier takes a component *set* —
                // it looks each component up to know which fields are references.
                nlohmann::json wrapper{{componentName, componentData}};
                QualifyInstanceReferences(wrapper, prefix);
                // PrepareBlueprint already refused any member whose values do not
                // read, so a failure here is the *instance*'s claim: the member goes
                // without that component rather than the expansion collapsing.
                if (!meta->addToScene(&scene, e.index, e.generation, wrapper.at(componentName)))
                {
                    Core::Log::Error("Blueprint: instance '{}' member '{}' overrides '{}' with something "
                                     "unreadable — the component is left as the blueprint had it.",
                                     instanceName, staged.resolved[i].name, componentName);
                }
            }
        }

        // An override may have flipped `parented` after the definition was flattened
        // *and* prepared. The nesting placement is baked into the prepared bytes
        // that were just decoded, so fixing the JSON is not enough: it comes off
        // here or not at all.
        if (const bool bakedIn = !staged.definition->members[i].parented; bakedIn != !desc.parented)
        {
            if (ECS::Transform *transform = scene.GetMut<ECS::Transform>(e))
            {
                *transform = desc.parented ? InverseComposeTransform(desc.placement, *transform)
                                           : ComposeTransform(desc.placement, *transform);
            }
        }

        // There is no root entity after expansion — the root is placement and only
        // placement — so a parentless member's Transform ends up in world space. A
        // parented one is relative to a member that already absorbed the placement;
        // composing again would apply it twice.
        if (!desc.parented)
        {
            if (ECS::Transform *transform = scene.GetMut<ECS::Transform>(e))
                *transform = ComposeTransform(staged.placement, *transform);
        }
    }
}

} // namespace Assisi::Runtime
