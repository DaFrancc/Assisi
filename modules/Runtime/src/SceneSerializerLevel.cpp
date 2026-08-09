/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/Runtime/EditorOnly.hpp>
#include <Assisi/Runtime/NameComponent.hpp>
#include <Assisi/Runtime/Naming.hpp>

#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include "SceneSerializerContext.hpp"
#include "SceneSerializerInstances.hpp"

// ---------------------------------------------------------------------------
// Saving and loading a whole level.
// ---------------------------------------------------------------------------

namespace Assisi::Runtime
{

namespace
{

// nlohmann dumps a non-finite float (NaN/Inf — e.g. from a physics blow-up) as
// the JSON literal `null`, which then throws on reload (get<float>() on null) and
// would void the entire level. Replace non-finite numbers with 0 in the tree
// before it is dumped, so every file we write stays loadable — no load-side
// exceptions, no whole-scene loss from one bad value. Returns the number of
// values it had to fix up so the caller can warn (a non-finite value in a save is
// always a symptom of a bug upstream, not something to silently paper over).
[[nodiscard]] size_t SanitizeNonFinite(nlohmann::json &value)
{
    size_t fixed = 0;
    if (value.is_object() || value.is_array())
    {
        for (auto &child : value)
        {
            fixed += SanitizeNonFinite(child);
        }
    }
    else if (value.is_number_float() && !std::isfinite(value.get<double>()))
    {
        value = 0.0;
        ++fixed;
    }
    return fixed;
}

} // namespace

nlohmann::json SceneSerializer::Save(ECS::Scene &scene, const LevelHeader &header, const InstanceTable *instances)
{
    auto &registry = Core::Reflect::ComponentRegistry::Instance();

    // Pass 1: collect all entity keys into a sorted map so the array order is
    // deterministic. Members are excluded — an instance entry describes them, and
    // writing them as entities as well would bake the blueprint into the level.
    // They still need names, though: a level entity may reference one.
    std::map<uint64_t, nlohmann::json> entityMap;
    std::map<uint64_t, std::string>    memberNames;

    for (const auto *meta : registry.SerializableComponents())
    {
        meta->iterateEntities(&scene, [&](uint32_t idx, uint32_t gen, const void *)
        {
            const ECS::Entity entity{idx, gen};

            // Scaffolding the editor stood up to work by — the blueprint editor's
            // sun above all. Skipping it here is enough for the whole save: passes 2
            // and 3 both work off `entityMap`, and what never enters it is never
            // named and never serialized.
            if (scene.Has<EditorOnly>(entity))
                return;

            if (const ECS::BlueprintMember *tag = scene.Get<ECS::BlueprintMember>(entity))
            {
                if (instances != nullptr)
                    memberNames.emplace(EntityKey(idx, gen), MemberPathName(*instances, *tag));
                return;
            }
            entityMap.emplace(EntityKey(idx, gen), nlohmann::json{});
        });
    }

    if (instances == nullptr && scene.Query<ECS::BlueprintMember>().begin() != scene.Query<ECS::BlueprintMember>().end())
    {
        Core::Log::Error("SceneSerializer: saving a scene with blueprint members but no instance table — the "
                         "instances and their entities will both be missing from the file.");
    }

    // Pass 2: name every entity, before anything serializes — an EntityRef field
    // resolves to a *name*, and the target may be anywhere in the file, including
    // ahead of the entity that points at it.
    SerializationContext            ctx;
    std::unordered_set<std::string> usedNames;
    usedNames.reserve(entityMap.size());
    for (auto &[key, entityJson] : entityMap)
    {
        const ECS::Entity entity{static_cast<uint32_t>(key & 0xFFFFFFFFull), static_cast<uint32_t>(key >> 32)};
        std::string       name = UniqueName(AuthoredName(scene, entity), usedNames);

        entityJson["name"] = name;
        ctx.entityToName.emplace(key, std::move(name));
    }

    // Members address as `car_3/wheel_fl`, resolved through the tag. Added after
    // the entities so a level entity can never lose its own name to one.
    for (auto &[key, path] : memberNames)
    {
        if (!path.empty())
            ctx.entityToName.emplace(key, path);
    }

    s_context = std::move(ctx);
    const ScopedContextReset contextReset;

    // Pass 3: serialize components (the context is live, so EntityToRef works).
    for (const auto *meta : registry.SerializableComponents())
    {
        // Name is the entity's `name` key, written above. Emitting it here as well
        // would put the same string in two places with no rule about which wins.
        if (meta->name == "Name")
            continue;

        meta->iterateEntities(&scene, [&](uint32_t idx, uint32_t gen, const void *compPtr)
        {
            const uint64_t key = EntityKey(idx, gen);
            const auto     it  = entityMap.find(key);
            if (it == entityMap.end())
                return; // a member: described by its instance entry, not written here
            it->second["components"][meta->name] = meta->serialize(compPtr);
        });
    }

    nlohmann::json result;
    result["version"] = 2;
    // Only written when set, so levels that need no systems stay free of the key
    // rather than gaining an empty one on every save.
    if (!header.systems.empty())
        result["systems"] = header.systems;
    result["entities"] = nlohmann::json::array();

    for (auto &[key, entityJson] : entityMap)
        result["entities"].push_back(std::move(entityJson));

    // From the live table, not from `header`: the editor moves an instance by
    // writing its row, and the file follows rather than needing to be told twice.
    if (instances != nullptr)
    {
        const std::vector<LevelInstance> placed = InstancesForSave(*instances);
        if (!placed.empty())
        {
            result["instances"] = nlohmann::json::array();
            for (const LevelInstance &entry : placed)
            {
                nlohmann::json written{{"name", entry.name},
                                       {"source", entry.source},
                                       {"transform", TransformToJson(entry.transform)}};
                // Only when there is something to say, so an instance nobody edited
                // stays three lines rather than gaining two empty containers.
                if (!entry.overrides.empty())
                    written["overrides"] = entry.overrides;
                if (!entry.removed.empty())
                    written["removed"] = entry.removed;
                result["instances"].push_back(std::move(written));
            }
        }
    }

    // Never persist NaN/Inf: it dumps as null and won't reload. Warn if we had to
    // fix any up — it means a component held a non-finite value (a bug upstream).
    if (const size_t fixed = SanitizeNonFinite(result); fixed > 0)
    {
        Core::Log::Warn("SceneSerializer: replaced {} non-finite float value(s) with 0 while saving "
                        "(NaN/Inf cannot be persisted; a component held a bad value).",
                        fixed);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

LevelResult SceneSerializer::Load(ECS::Scene &scene, const nlohmann::json &j, const ProgressFn &onProgress,
                                  LevelHeader *header, InstanceTable *instances)
{
    const int32_t version = j.value("version", 0);
    if (version != 2)
    {
        // Reported, not swallowed: a caller that gets no signal reports a
        // *successful* load of an empty scene, which is how a level silently
        // becomes nothing. Refused before the Clear below, so the caller keeps
        // what it had.
        Core::Log::Error("SceneSerializer: unsupported level file version {} (this build reads version 2).",
                         version);
        return std::unexpected(LevelError::UnsupportedVersion);
    }

    // Read the instance entries before anything is destroyed: a file that names an
    // instance a caller has nowhere to put must not cost the caller the level it
    // already had.
    std::vector<LevelInstance> placed;
    if (const auto it = j.find("instances"); it != j.end() && it->is_array() && !it->empty())
    {
        if (instances == nullptr)
        {
            Core::Log::Error("SceneSerializer: the file places blueprint instances, but this load was given "
                             "no instance table to put them in.");
            return std::unexpected(LevelError::NoInstanceTable);
        }

        placed.reserve(it->size());
        for (const auto &entry : *it)
        {
            if (!entry.contains("name") || !entry.at("name").is_string() ||
                entry.at("name").get<std::string>().empty())
            {
                Core::Log::Error("SceneSerializer: an instance entry has no name.");
                return std::unexpected(LevelError::MissingName);
            }
            if (!entry.contains("source") || !entry.at("source").is_string())
            {
                Core::Log::Error("SceneSerializer: instance '{}' has no source.",
                                 entry.at("name").get<std::string>());
                return std::unexpected(LevelError::MissingSource);
            }
            LevelInstance instance{
                .name      = entry.at("name").get<std::string>(),
                .source    = entry.at("source").get<std::string>(),
                .transform = TransformFromJson(entry.value("transform", nlohmann::json::object())),
                .overrides = nlohmann::json::object(),
                .removed   = {}};
            if (const auto claims = entry.find("overrides"); claims != entry.end() && claims->is_object())
                instance.overrides = *claims;
            if (const auto claims = entry.find("removed"); claims != entry.end() && claims->is_array())
            {
                for (const auto &path : *claims)
                {
                    if (path.is_string())
                        instance.removed.push_back(path.get<std::string>());
                }
            }
            placed.push_back(std::move(instance));
        }
    }

    if (header != nullptr)
    {
        header->systems.clear();
        if (const auto it = j.find("systems"); it != j.end() && it->is_array())
        {
            for (const auto &name : *it)
            {
                if (name.is_string())
                    header->systems.push_back(name.get<std::string>());
            }
        }
        header->instances = placed;
    }

    auto &registry = Core::Reflect::ComponentRegistry::Instance();

    scene.Clear();
    if (instances != nullptr)
    {
        // Ids restart from 1 with the world, because the table is discarded with
        // it. Nothing may assume an id survives a load (§2).
        instances->Clear();
    }

    s_context = SerializationContext{};
    const ScopedContextReset contextReset;

    const auto &entities = j.at("entities");

    // Pass 1: read and validate every name, before a single entity is created.
    // Refusing here rather than mid-load is what keeps a bad file from leaving a
    // half-built scene, and every one of these means the file addresses something
    // other than what it appears to.
    std::vector<std::string> names;
    names.reserve(entities.size());
    for (size_t i = 0; i < entities.size(); ++i)
    {
        const auto &entityJson = entities[i];
        if (!entityJson.contains("name") || !entityJson.at("name").is_string())
        {
            Core::Log::Error("SceneSerializer: entity #{} has no name.", i);
            return std::unexpected(LevelError::MissingName);
        }

        std::string name = entityJson.at("name").get<std::string>();

        // One rule, stated once, in Naming.hpp — the editor's rename box refuses
        // exactly what this refuses, so a name the editor accepts is a name that
        // reloads. Banning the separator is what makes entity names and instance
        // member paths disjoint *by construction* (every member path has one and
        // no entity name does), which is why `car` the entity and `car` the
        // instance can coexist without anyone cross-checking them.
        //
        // The rule's own error is a NameError; this function's vocabulary is
        // LevelError, so the specific reason is logged with the entity index the
        // caller cannot see and the kind is widened to InvalidName.
        if (const std::expected<void, NameError> valid = ValidateName(name); !valid.has_value())
        {
            Core::Log::Error("SceneSerializer: entity #{} is named '{}': {}.", i, name,
                             Describe(valid.error()));
            return std::unexpected(LevelError::InvalidName);
        }
        names.push_back(std::move(name));
    }

    // Pass 2: create every entity up front so nameToEntity is complete before any
    // component deserializes. A reference may point *forward* to an entity that
    // has not been created yet; a single pass would resolve those to NullEntity
    // and silently flatten the hierarchy.
    s_context->nameToEntity.reserve(names.size());
    std::vector<ECS::Entity> created;
    created.reserve(names.size());
    for (size_t i = 0; i < names.size(); ++i)
    {
        const ECS::Entity e = scene.Create();
        created.push_back(e);

        if (!s_context->nameToEntity.emplace(names[i], e).second)
        {
            // Duplicate names make every reference and every override ambiguous,
            // and picking one is picking silently.
            Core::Log::Error("SceneSerializer: two entities are both named '{}'.", names[i]);
            scene.Clear();
            return std::unexpected(LevelError::DuplicateName);
        }

        // The name is the entity's Name, not a second identity beside it.
        (void)scene.Add(e, Name{Core::ShortString{names[i]}});
    }

    // Pass 2b: the same, for every member of every instance. Before any component
    // deserializes, because a level entity may name `car_3/body` and a member may
    // name a level entity — resolving either one early nulls it silently.
    std::vector<StagedInstance> staged;
    staged.reserve(placed.size());
    for (std::size_t i = 0; i < placed.size(); ++i)
    {
        StagedInstance row;
        const std::expected<void, LevelError> ok =
            StageInstance(scene, *instances, placed[i], static_cast<int32_t>(i), row);
        // Kept either way: what staging got as far as creating is in `row`, and it
        // has to be reachable for the clear below to be the whole cleanup.
        staged.push_back(std::move(row));
        if (!ok)
        {
            scene.Clear();
            instances->Clear();
            return std::unexpected(ok.error());
        }
    }

    // Pass 3: deserialize components now that every EntityRef can resolve.
    const size_t entityCount = entities.size();
    for (size_t i = 0; i < entityCount; ++i)
    {
        // Report progress across this pass — the load's dominant cost. Cheap
        // enough to call per entity (a few atomic stores through the callback).
        if (onProgress)
            onProgress(entityCount == 0 ? 1.f : static_cast<float>(i) / static_cast<float>(entityCount));

        const auto &entityJson = entities[i];
        if (!entityJson.contains("components"))
            continue;

        const ECS::Entity e = created[i];
        for (const auto &[compName, compData] : entityJson.at("components").items())
        {
            // The entity-level name already wrote this one, and it is authoritative:
            // a file that also lists Name in components is out of spec, and letting
            // it through would give the entity a name nothing else in the file
            // addresses it by.
            if (compName == "Name")
            {
                Core::Log::Warn("SceneSerializer: entity '{}' lists a Name component; the entity's "
                                "'name' key is authoritative and this one is ignored.",
                                names[i]);
                continue;
            }

            const auto *meta = registry.Find(compName);
            if (!meta)
            {
                Core::Log::Warn("SceneSerializer: unknown component '{}' - skipped", compName);
                continue;
            }
            // Non-serializable (ACOMP(transient)) components have no addToScene
            // hook and are never written by Save; if one appears in a file
            // (hand-edited or from an older format), skip it rather than deref
            // a null hook.
            if (!meta->serializable)
            {
                Core::Log::Warn("SceneSerializer: non-serializable component '{}' - skipped", compName);
                continue;
            }
            if (!meta->addToScene(&scene, e.index, e.generation, compData))
            {
                // The hook has already logged the component, the field and the
                // mismatch; this adds the entity, which it had no way to know.
                Core::Log::Error("SceneSerializer: entity '{}' has an unreadable '{}' — the file is refused.",
                                 names[i], compName);
                scene.Clear();
                return std::unexpected(LevelError::MalformedComponent);
            }
        }
    }

    // Pass 3b: and the members', which reach both their own instance and the file
    // that placed it.
    for (std::size_t i = 0; i < staged.size(); ++i)
        CommitInstance(scene, staged[i], placed[i].name);

    // Every reference had to resolve. One that did not means the file names an
    // entity it does not declare — a rename someone made by hand, a merge that
    // dropped an entity, or a v1 file whose numeric refs came through here.
    if (!s_context->unresolvedRefNames.empty())
    {
        const std::vector<std::string> &bad = s_context->unresolvedRefNames;
        std::string                     list;
        for (size_t i = 0; i < bad.size() && i < 8; ++i)
            list += (i == 0 ? "" : ", ") + bad[i];
        if (bad.size() > 8)
            list += std::format(", … ({} more)", bad.size() - 8);

        Core::Log::Error("SceneSerializer: {} entity reference(s) name an entity the file does not declare: {}.",
                         bad.size(), list);
        scene.Clear();
        return std::unexpected(LevelError::UnresolvedReference);
    }

    return {};
}

// ---------------------------------------------------------------------------
// The prepared form
// ---------------------------------------------------------------------------

} // namespace Assisi::Runtime
