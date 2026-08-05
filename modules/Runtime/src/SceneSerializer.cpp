/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/NameComponent.hpp>

#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Assisi::Runtime
{

// ---------------------------------------------------------------------------
// Thread-local serialization context
// ---------------------------------------------------------------------------

namespace
{

struct SerializationContext
{
    /// How an EntityRef field addresses its target while this context is live.
    /// Two callers, two vocabularies, and mixing them is the kind of bug that
    /// produces a plausible scene rather than an error — so the mode is explicit.
    enum class RefMode
    {
        Names,      ///< Save/Load of a file: a ref is the target entity's name.
        SetIndices, ///< TransferEntities: a ref is an index within the moved set.
    };
    RefMode mode = RefMode::Names;

    // SetIndices. Save side: entity key (gen<<32|idx) → index within the set.
    // Load side: index → live Entity.
    std::unordered_map<uint64_t, uint32_t> entityToIndex;
    std::vector<ECS::Entity>               indexToEntity;

    // Names. Save side: entity key → the unique name it is being written under.
    // Load side: name → the live Entity created for it.
    std::unordered_map<uint64_t, std::string>    entityToName;
    std::unordered_map<std::string, ECS::Entity> nameToEntity;

    /// Names an EntityRef asked for that the file never declared. Collected here
    /// rather than thrown where they are found: RefToEntity runs inside a
    /// component's generated deserialize, which knows neither the file nor the
    /// entity it is speaking for. Load reports them together and refuses the file.
    std::vector<std::string> unresolvedRefNames;
};

thread_local std::optional<SerializationContext> s_context;

// Raw-entity (identity) context — engaged by ScopedRawEntityContext. When set,
// EntityRef fields map through raw slot indices against this scene instead of the
// Save/Load remap table. Mutually exclusive with s_context by construction (the
// editor never captures/applies mid-Save/Load), and asserted non-reentrant.
thread_local ECS::Scene *s_rawContextScene = nullptr;

inline uint64_t EntityKey(uint32_t idx, uint32_t gen)
{
    return (static_cast<uint64_t>(gen) << 32) | idx;
}

// Tears down the thread-local context on every exit path, including a
// component serialize/deserialize throwing mid-pass (malformed field data
// reaches j.at(...)/_v[i].get<T>() in generated code). Without it the context
// would stay engaged after a throw and a later EntityToIndex / IndexToEntity
// call would resolve against a stale, half-populated context.
struct ScopedContextReset
{
    ~ScopedContextReset() { s_context.reset(); }
};

} // namespace

// ---------------------------------------------------------------------------
// Public context accessors (called from component serialize/addToScene lambdas)
// ---------------------------------------------------------------------------

nlohmann::json SceneSerializer::EntityToRef(ECS::Entity entity)
{
    // Null first, and unconditionally: every mode spells "no target" the same way,
    // and none of the lookups below has a meaningful answer for it.
    if (entity == ECS::NullEntity)
        return nullptr;

    // Raw-entity context wins: identity mapping, keyed by slot AND generation.
    //
    // Round-6 M11: this used to return the bare slot index, dropping the
    // generation. A ref captured to an entity that was later destroyed then
    // resolved to whatever new entity reused the slot — silently, because the
    // reused slot is perfectly alive, so no liveness check can catch it. Packing
    // the generation in lets RefToEntity below reject the stale ref instead.
    if (s_rawContextScene != nullptr)
        return EntityKey(entity.index, entity.generation);

    if (!s_context)
        return nullptr;

    const uint64_t key = EntityKey(entity.index, entity.generation);

    if (s_context->mode == SerializationContext::RefMode::SetIndices)
    {
        const auto it = s_context->entityToIndex.find(key);
        // Out of the moved set: ~0ull, which RefToEntity reads as out of range and
        // nulls. TransferEntities warns about each of these before it gets here.
        return it != s_context->entityToIndex.end() ? nlohmann::json(it->second) : nlohmann::json(~0ull);
    }

    const auto it = s_context->entityToName.find(key);
    return it != s_context->entityToName.end() ? nlohmann::json(it->second) : nlohmann::json(nullptr);
}

ECS::Entity SceneSerializer::RefToEntity(const nlohmann::json &value)
{
    if (value.is_null())
        return ECS::NullEntity;

    // Raw-entity context wins: the value is a packed (slot, generation) key. The
    // paired restore revives entities at their original handle, so in the intended
    // flow the generation matches exactly. When it does not, the slot has been
    // recycled by an unrelated entity and the ref is stale — resolve to null
    // rather than silently redirecting onto whoever moved in (round-6 M11).
    if (s_rawContextScene != nullptr)
    {
        if (!value.is_number_unsigned())
            return ECS::NullEntity;
        const uint64_t    key     = value.get<uint64_t>();
        const auto        slot    = static_cast<uint32_t>(key & 0xFFFFFFFFull);
        const auto        wantGen = static_cast<uint32_t>(key >> 32);
        const ECS::Entity live    = s_rawContextScene->EntityAt(slot);
        return live.generation == wantGen ? live : ECS::NullEntity;
    }

    if (!s_context)
        return ECS::NullEntity;

    if (s_context->mode == SerializationContext::RefMode::SetIndices)
    {
        if (!value.is_number_unsigned())
            return ECS::NullEntity;
        const uint64_t index = value.get<uint64_t>();
        return index < s_context->indexToEntity.size() ? s_context->indexToEntity[static_cast<std::size_t>(index)]
                                                       : ECS::NullEntity;
    }

    // A file ref is a name. Anything else is a v1 file or a hand-edit that meant a
    // position — both of which would resolve to *some* entity if we guessed, which
    // is the failure names exist to remove. Record it and let Load refuse.
    if (!value.is_string())
    {
        s_context->unresolvedRefNames.push_back(value.dump());
        return ECS::NullEntity;
    }

    // A leading slash means "the file that wrote this", which for a reference in
    // the outermost file is that file — so it addresses the same names a plain
    // one does. Inside an instance the prefixing already resolved the difference
    // (Runtime::Blueprint's QualifyName), and this is what makes the two agree.
    auto name = value.get<std::string>();
    if (!name.empty() && name.front() == '/')
        name.erase(0, 1);

    const auto it = s_context->nameToEntity.find(name);
    if (it == s_context->nameToEntity.end())
    {
        s_context->unresolvedRefNames.push_back(std::move(name));
        return ECS::NullEntity;
    }
    return it->second;
}

// ---------------------------------------------------------------------------
// TransferEntities (entity migration)
// ---------------------------------------------------------------------------

std::vector<ECS::Entity> SceneSerializer::TransferEntities(ECS::Scene &src, ECS::Scene &dst,
                                                           std::span<const ECS::Entity> entities)
{
    if (s_context || s_rawContextScene != nullptr)
    {
        Core::Log::Error("TransferEntities: a serialization context is already active on this thread.");
        return {};
    }
    if (entities.empty())
        return {};

    ScopedContextReset guard;
    s_context       = SerializationContext{};
    s_context->mode = SerializationContext::RefMode::SetIndices;

    // Source half of the remap: each migrated entity → its index within the set.
    // A ref to any entity NOT in this map returns nullopt from EntityToIndex,
    // serializes as ~0ull, and resolves to NullEntity on the destination side —
    // exactly the "null a ref that leaves the set" behaviour we want.
    for (uint32_t i = 0; i < entities.size(); ++i)
    {
        const ECS::Entity e = entities[i];
        s_context->entityToIndex[EntityKey(e.index, e.generation)] = i;
    }

    const auto &registry = Core::Reflect::ComponentRegistry::Instance();

    // Diagnose refs that will be dropped, before serialize silently loses them.
    // Walk each migrated entity's reflected EntityRef fields; a non-null handle
    // that is not itself in the migrated set is about to become null.
    for (const ECS::Entity e : entities)
    {
        for (const Core::Reflect::ComponentMeta *meta : registry.SerializableComponents())
        {
            const void *comp = meta->getByEntity(&src, e.index, e.generation);
            if (comp == nullptr)
                continue;
            for (const Core::Reflect::FieldMeta &field : meta->fields)
            {
                if (field.type != Core::Reflect::FieldType::EntityRef || field.transient)
                    continue;
                const auto ref = *reinterpret_cast<const ECS::Entity *>(static_cast<const char *>(comp) +
                                                                        field.offset);
                if (ref == ECS::NullEntity)
                    continue;
                if (!s_context->entityToIndex.contains(EntityKey(ref.index, ref.generation)))
                {
                    Core::Log::Warn("Migrate: {}::{} on entity (index {}, gen {}) references entity "
                                    "(index {}, gen {}) outside the migrated set — it will be null in "
                                    "the destination.",
                                    meta->name, field.name, e.index, e.generation, ref.index, ref.generation);
                }
            }
        }
    }

    // Pass 1: serialize every migrated component while the source map is live, so
    // in-set EntityRefs capture their set index. Held per entity for pass 3.
    struct CapturedComponent
    {
        const Core::Reflect::ComponentMeta *meta;
        nlohmann::json                      data;
    };
    std::vector<std::vector<CapturedComponent>> captured(entities.size());
    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        const ECS::Entity e = entities[i];
        for (const Core::Reflect::ComponentMeta *meta : registry.SerializableComponents())
        {
            if (const void *comp = meta->getByEntity(&src, e.index, e.generation))
                captured[i].push_back({meta, meta->serialize(comp)});
        }
    }

    // Pass 2: create the destination entities and record the destination half of
    // the remap. All created first, so a ref to a sibling that appears later in
    // the set still resolves in pass 3 (same forward-ref handling as Load).
    std::vector<ECS::Entity> created;
    created.reserve(entities.size());
    s_context->indexToEntity.reserve(entities.size());
    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        const ECS::Entity d = dst.Create();
        created.push_back(d);
        s_context->indexToEntity.push_back(d);
    }

    // Pass 3: deserialize each captured component into its destination entity.
    // IndexToEntity now maps in-set refs onto the created handles.
    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        for (const CapturedComponent &c : captured[i])
        {
            if (c.meta->addToScene)
                c.meta->addToScene(&dst, created[i].index, created[i].generation, c.data);
        }
    }

    // The originals leave the source scene. Structural, so defer to the source's
    // own FlushDestroyed (end of its frame) rather than mutating mid-iteration —
    // the caller tears down their Jolt bodies, which the ECS destroy does not.
    for (const ECS::Entity e : entities)
        src.Destroy(e);

    return created;
}

// ---------------------------------------------------------------------------
// ScopedRawEntityContext
// ---------------------------------------------------------------------------

SceneSerializer::ScopedRawEntityContext::ScopedRawEntityContext(ECS::Scene &scene)
{
    // One context per thread. Nesting raw-in-raw or raw-in-Save/Load is a bug:
    // the mappings are incompatible and the flat pointer/optional can't stack.
    ASSISI_ASSERT(s_rawContextScene == nullptr && !s_context,
                  "SceneSerializer: a serialization context is already active on this thread "
                  "(raw-entity and Save/Load contexts are mutually exclusive and non-reentrant).");
    s_rawContextScene = &scene;
}

SceneSerializer::ScopedRawEntityContext::~ScopedRawEntityContext()
{
    s_rawContextScene = nullptr;
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

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

    const BlueprintDefinition *definition = GetBlueprintDefinition(row->source);
    if (definition == nullptr || tag.memberIndex >= definition->members.size())
        return {};

    const std::string &leaf = definition->members[tag.memberIndex].name;
    return row->name.empty() ? leaf : row->name + "/" + leaf;
}

/// Everything one placed instance produced, kept so a later failure can undo it.
struct StagedInstance
{
    uint32_t                   id         = 0;
    const BlueprintDefinition *definition = nullptr;
    ECS::Transform             placement;
    std::vector<ECS::Entity>   members; ///< Parallel to definition->members.
};

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
/// Throws on a name collision — with a level entity, with another instance, or
/// with a member of one. Every one of them makes a reference ambiguous.
StagedInstance StageInstance(ECS::Scene &scene, InstanceTable &table, const LevelInstance &entry,
                             int32_t levelInstanceIndex)
{
    if (!HasUniformScale(entry.transform))
    {
        throw std::runtime_error(std::format(
            "instance '{}' has a non-uniform scale ({}, {}, {}); an instance may only translate, rotate, "
            "or scale uniformly",
            entry.name, entry.transform.scale.x, entry.transform.scale.y, entry.transform.scale.z));
    }

    const BlueprintDefinition *definition = GetBlueprintDefinition(entry.source);
    if (definition == nullptr)
        throw std::runtime_error(std::format("instance '{}' cannot use '{}'", entry.name, entry.source));

    StagedInstance staged;
    staged.definition = definition;
    staged.placement  = entry.transform;
    staged.id         = table.Add(BlueprintInstance{.name               = entry.name,
                                                    .source             = definition->source,
                                                    .transform          = entry.transform,
                                                    .levelInstanceIndex = levelInstanceIndex});

    staged.members.reserve(definition->members.size());
    for (uint32_t i = 0; i < definition->members.size(); ++i)
    {
        const BlueprintMemberDesc &desc = definition->members[i];
        const ECS::Entity          e    = scene.Create();
        staged.members.push_back(e);

        const std::string path = entry.name.empty() ? desc.name : entry.name + "/" + desc.name;
        if (!s_context->nameToEntity.emplace(path, e).second)
            throw std::runtime_error(std::format("'{}' is claimed twice", path));

        // The leaf, not the path: a Name is what the member's own file calls it,
        // and a path would not fit anyway. The path is derived from the tag when
        // something needs it.
        (void)scene.Add(e, Name{Core::ShortString{LeafName(desc.name)}});
        (void)scene.Add(e, ECS::BlueprintMember{.instanceId = staged.id, .memberIndex = i});
    }

    return staged;
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

    for (std::size_t i = 0; i < staged.members.size(); ++i)
    {
        const BlueprintMemberDesc &desc = staged.definition->members[i];
        const ECS::Entity          e    = staged.members[i];

        if (desc.components.is_object())
        {
            nlohmann::json components = desc.components;
            QualifyReferences(components, prefix);

            for (const auto &[componentName, componentData] : components.items())
            {
                const Core::Reflect::ComponentMeta *meta = registry.Find(componentName);
                if (meta == nullptr || !meta->serializable)
                {
                    Core::Log::Warn("Blueprint: '{}' member '{}' names component '{}', which this build does "
                                    "not have — skipped.",
                                    staged.definition->source, desc.name, componentName);
                    continue;
                }
                meta->addToScene(&scene, e.index, e.generation, componentData);
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
    // Only written when set, so levels that never name a profile stay free of the
    // key rather than gaining an empty one on every save.
    if (!header.profile.empty())
        result["profile"] = header.profile;
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
                result["instances"].push_back({{"name", entry.name},
                                               {"source", entry.source},
                                               {"transform", TransformToJson(entry.transform)}});
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

void SceneSerializer::Load(ECS::Scene &scene, const nlohmann::json &j, const ProgressFn &onProgress,
                           LevelHeader *header, InstanceTable *instances)
{
    const int32_t version = j.value("version", 0);
    if (version != 2)
    {
        // Thrown, not returned: a caller that gets no signal reports a *successful*
        // load of an empty scene, which is how a level silently becomes nothing.
        // Thrown before the Clear below, so a direct caller keeps what it had.
        throw std::runtime_error(
            std::format("unsupported level file version {} (this build reads version 2)", version));
    }

    // Read the instance entries before anything is destroyed: a file that names an
    // instance a caller has nowhere to put must not cost the caller the level it
    // already had.
    std::vector<LevelInstance> placed;
    if (const auto it = j.find("instances"); it != j.end() && it->is_array() && !it->empty())
    {
        if (instances == nullptr)
        {
            throw std::runtime_error("the file places blueprint instances, but this load was given no "
                                     "instance table to put them in");
        }

        placed.reserve(it->size());
        for (const auto &entry : *it)
        {
            if (!entry.contains("name") || !entry.at("name").is_string() ||
                entry.at("name").get<std::string>().empty())
            {
                throw std::runtime_error("an instance entry has no name");
            }
            if (!entry.contains("source") || !entry.at("source").is_string())
            {
                throw std::runtime_error(
                    std::format("instance '{}' has no source", entry.at("name").get<std::string>()));
            }
            placed.push_back(LevelInstance{
                .name      = entry.at("name").get<std::string>(),
                .source    = entry.at("source").get<std::string>(),
                .transform = TransformFromJson(entry.value("transform", nlohmann::json::object()))});
        }
    }

    if (header != nullptr)
    {
        header->profile   = j.value("profile", std::string{});
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
            throw std::runtime_error(std::format("entity #{} has no name", i));

        std::string name = entityJson.at("name").get<std::string>();
        if (name.empty())
            throw std::runtime_error(std::format("entity #{} has an empty name", i));
        if (name.size() > Core::kShortStringMax)
        {
            // Refused rather than truncated: truncation is how two members become
            // indistinguishable, and an override that then picks the wrong one is
            // exactly the failure named entities exist to prevent.
            throw std::runtime_error(std::format("entity name '{}' is longer than the {}-byte limit", name,
                                                 Core::kShortStringMax));
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
            scene.Clear();
            throw std::runtime_error(std::format("two entities are both named '{}'", names[i]));
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
        staged.push_back(StageInstance(scene, *instances, placed[i], static_cast<int32_t>(i)));

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
            meta->addToScene(&scene, e.index, e.generation, compData);
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

        scene.Clear();
        throw std::runtime_error(
            std::format("{} entity reference(s) name an entity the file does not declare: {}", bad.size(), list));
    }
}

// ---------------------------------------------------------------------------
// Runtime expansion
// ---------------------------------------------------------------------------

std::optional<uint32_t> SceneSerializer::ExpandInstance(ECS::Scene &scene, InstanceTable &table,
                                                       std::string_view source, const ECS::Transform &placement)
{
    if (s_context || s_rawContextScene != nullptr)
    {
        Core::Log::Error("ExpandInstance: a serialization context is already active on this thread.");
        return std::nullopt;
    }

    // Its own name context, holding only this instance's members. A runtime spawn
    // has no file around it, so there is nothing else its references could name —
    // and giving it the whole scene's names would let a blueprint silently wire
    // itself to whatever happened to share a name.
    ScopedContextReset guard;
    s_context = SerializationContext{};

    // No instance name: the members are `body`, not `car_3/body`, because nothing
    // placed this one and no file addresses into it.
    const LevelInstance entry{.name = {}, .source = std::string{source}, .transform = placement};

    StagedInstance instance;
    try
    {
        instance = StageInstance(scene, table, entry, /*levelInstanceIndex=*/-1);
        CommitInstance(scene, instance, /*instanceName=*/"");
    }
    catch (const std::exception &ex)
    {
        // All or nothing (§7): a missing nested file three members in must leave no
        // partial instance behind.
        Core::Log::Error("SpawnBlueprint: '{}' failed: {}", source, ex.what());
        for (const ECS::Entity member : instance.members)
            scene.Destroy(member);
        if (instance.id != 0)
            table.Remove(instance.id);
        return std::nullopt;
    }

    if (!s_context->unresolvedRefNames.empty())
    {
        Core::Log::Error("SpawnBlueprint: '{}' has {} reference(s) naming a member it does not declare; the "
                         "first is '{}'.",
                         source, s_context->unresolvedRefNames.size(), s_context->unresolvedRefNames.front());
        for (const ECS::Entity member : instance.members)
            scene.Destroy(member);
        table.Remove(instance.id);
        return std::nullopt;
    }

    return instance.id;
}

// ---------------------------------------------------------------------------
// File I/O helpers
// ---------------------------------------------------------------------------

bool SceneSerializer::SaveToFile(ECS::Scene &scene, const std::filesystem::path &path, const LevelHeader &header,
                                 const InstanceTable *instances)
{
    // Binary, so the newlines written are the newlines that land on disk. A
    // text-mode write expands every '\n' to "\r\n" on Windows and leaves it
    // alone elsewhere, which makes the same level two different files depending
    // on who saved it — noise in every diff, and a refused join for the network
    // session's content-hash check.
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open())
    {
        Core::Log::Error("SceneSerializer: cannot open '{}' for writing", path.string());
        return false;
    }
    f << Save(scene, header, instances).dump(2);
    if (!f.good())
    {
        Core::Log::Error("SceneSerializer: write failed for '{}'", path.string());
        return false;
    }
    return true;
}

bool SceneSerializer::LoadFromDisk(ECS::Scene &scene, const std::filesystem::path &path,
                                   const ProgressFn &onProgress, LevelHeader *header, InstanceTable *instances)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        Core::Log::Error("SceneSerializer: cannot open '{}' for reading", path.string());
        return false;
    }

    try
    {
        Load(scene, nlohmann::json::parse(file), onProgress, header, instances);
        return true;
    }
    catch (const std::exception &ex)
    {
        // Same contract as LoadFromFile below: a failed load yields an empty
        // scene rather than a half-populated one.
        Core::Log::Error("SceneSerializer: failed to load '{}': {}", path.string(), ex.what());
        scene.Clear();
        return false;
    }
}

bool SceneSerializer::LoadFromFile(ECS::Scene &scene, std::string_view assetPath, const ProgressFn &onProgress,
                                   LevelHeader *header, InstanceTable *instances)
{
    const auto text = Core::AssetSystem::ReadText(assetPath);
    if (!text)
    {
        Core::Log::Error("SceneSerializer: cannot read asset '{}'", assetPath);
        return false;
    }

    try
    {
        Load(scene, nlohmann::json::parse(*text), onProgress, header, instances);
        return true;
    }
    catch (const std::exception &ex)
    {
        // A parse error leaves the scene untouched; a throw partway through
        // Load (a malformed component field) leaves it half-populated. Clear
        // it either way so a failed load yields an empty scene, never a
        // corrupt one. (ScopedContextReset in Load already freed s_context.)
        //
        // Catches std::exception, not just json::exception: Load runs arbitrary
        // component addToScene hooks, and one throwing anything else (bad_alloc,
        // out_of_range from a hook's own container) would otherwise escape and
        // leave the half-populated scene behind.
        Core::Log::Error("SceneSerializer: failed to load '{}': {}", assetPath, ex.what());
        scene.Clear();
        return false;
    }
}

} // namespace Assisi::Runtime