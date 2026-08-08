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

/// A packed EntityRef, the way BinaryCodec spells one: index low, generation high.
/// The same packing EntityKey uses, named separately because it means something
/// different — this one crosses the codec's reference hook.
constexpr uint64_t PackEntity(ECS::Entity entity)
{
    return (static_cast<uint64_t>(entity.generation) << 32) | entity.index;
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

std::string_view Describe(LevelError error)
{
    switch (error)
    {
    case LevelError::FileUnreadable:
        return "the file could not be read";
    case LevelError::MalformedJson:
        return "the file is not readable JSON";
    case LevelError::UnsupportedVersion:
        return "the file is a version this build does not read";
    case LevelError::NoInstanceTable:
        return "the file places blueprint instances and this load has nowhere to put them";
    case LevelError::MissingName:
        return "an entity or instance in the file has no name";
    case LevelError::MissingSource:
        return "an instance in the file names no source";
    case LevelError::InvalidName:
        return "the file gives something a name that is not usable as one";
    case LevelError::DuplicateName:
        return "two things in the file answer to one name";
    case LevelError::NonUniformScale:
        return "an instance has a non-uniform scale";
    case LevelError::BlueprintUnusable:
        return "an instance names a blueprint that will not load";
    case LevelError::UnresolvedReference:
        return "a reference names something the file does not declare";
    case LevelError::MalformedComponent:
        return "a component field holds something the engine cannot read";
    case LevelError::ContextBusy:
        return "a serialization context is already active on this thread";
    case LevelError::InstanceNotLive:
        return "no such instance is live";
    case LevelError::NameAlreadyLive:
        return "an instance of that name is already live in this world";
    }
    return "the file cannot be used";
}

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

    // Claimed but mapped at nothing: a member an instance removed. That is a
    // legitimate thing for a file to say, so the reference nulls with a warning
    // rather than refusing the file the way an unknown name does.
    if (it->second == ECS::NullEntity)
        Core::Log::Warn("SceneSerializer: a reference names '{}', which its instance removed — left null.", name);

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
            // The data came out of Save on the *source* scene a moment ago, so a
            // refusal here means the codec disagrees with itself rather than that
            // a file is wrong. Loud, and the migration continues: the entity has
            // already moved and there is nothing to roll back to.
            if (c.meta->addToScene &&
                !c.meta->addToScene(&dst, created[i].index, created[i].generation, c.data))
            {
                Core::Log::Error("SceneSerializer: migrating '{}' lost a component the source scene had "
                                 "written — this is an engine bug, not a bad file.",
                                 c.meta->name);
            }
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

    const BlueprintResult definition = GetBlueprintDefinition(row->source);
    if (!definition || tag.memberIndex >= (*definition)->members.size())
        return {};

    const std::string &leaf = (*definition)->members[tag.memberIndex].name;
    return row->name.empty() ? leaf : row->name + "/" + leaf;
}

/// Everything one placed instance produced, kept so a later failure can undo it.
struct StagedInstance
{
    ECS::InstanceId id;

    /// Held, not borrowed. A load stages every instance before committing any of
    /// them, so this outlives the lookup that produced it — and on the async-travel
    /// worker, a blueprint save on the main thread can evict the cache entry in
    /// between.
    std::shared_ptr<const BlueprintDefinition> definition;

    ECS::Transform placement;

    /// Parallel to definition->members, with NullEntity where this instance
    /// removed one. A hole rather than a shorter list because the index *is* the
    /// NetId offset (§9): two instances of one file that removed different members
    /// must still agree about which index names which member.
    std::vector<ECS::Entity> members;

    /// This instance's view of each member, after its own overrides. Parallel to
    /// members; empty entries where there is a hole.
    std::vector<BlueprintMemberDesc> resolved;

    /// The instance's own claims, kept so the commit knows which components it may
    /// take off the fast path. A prepared block is *full state*, so decoding one
    /// over a component an override just set would undo the override.
    nlohmann::json overrides = nlohmann::json::object();
};

/// Which entities a re-expansion may take over instead of creating, keyed by the
/// member name they currently stand for.
///
/// Entries are **erased as they are adopted**, so what is left when staging ends is
/// exactly the set of members the edit deleted. That is the whole diff, and it falls
/// out of the staging walk rather than needing a second comparison that could
/// disagree with it.
struct AdoptionSet
{
    /// The row to keep, instead of allocating a new one. A re-expansion must land on
    /// the same instance id: every BlueprintMember tag in the world names it, and so
    /// does every transaction in the editor's history.
    ECS::InstanceId instanceId;

    std::unordered_map<std::string, ECS::Entity> byName;
};

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
///        whose names it knows are taken over rather than created.
std::expected<void, LevelError> StageInstance(ECS::Scene &scene, InstanceTable &table,
                                              const LevelInstance &entry, int32_t levelInstanceIndex,
                                              StagedInstance &staged, AdoptionSet *adopt = nullptr)
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

bool SceneSerializer::SaveEntitiesToFile(ECS::Scene &scene, std::span<const ECS::Entity> entities,
                                         const std::filesystem::path &path, const ECS::Transform &origin)
{
    if (entities.empty())
    {
        Core::Log::Error("Blueprint: nothing selected to save.");
        return false;
    }
    if (s_context || s_rawContextScene != nullptr)
    {
        Core::Log::Error("SaveEntitiesToFile: a serialization context is already active on this thread.");
        return false;
    }

    for (const ECS::Entity entity : entities)
    {
        if (scene.Has<ECS::BlueprintMember>(entity))
        {
            // Nesting is an `instances` entry, not copied entities. Copying them
            // would bake the inner blueprint into the new file and stop a fix to it
            // from reaching this one — the exact thing the format exists to avoid.
            Core::Log::Error("Blueprint: the selection contains a blueprint member. Prune it from its "
                             "instance first, or nest by adding an `instances` entry by hand.");
            return false;
        }
    }

    const auto &registry = Core::Reflect::ComponentRegistry::Instance();

    ScopedContextReset guard;
    s_context = SerializationContext{};

    // Names first, so a reference between two selected entities resolves whichever
    // order they are serialized in.
    std::unordered_set<std::string> usedNames;
    std::vector<std::string>        names;
    names.reserve(entities.size());
    for (const ECS::Entity entity : entities)
    {
        std::string name = UniqueName(AuthoredName(scene, entity), usedNames);
        s_context->entityToName.emplace(EntityKey(entity.index, entity.generation), name);
        names.push_back(std::move(name));
    }

    nlohmann::json doc;
    doc["version"]  = 2;
    doc["entities"] = nlohmann::json::array();

    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        const ECS::Entity entity = entities[i];

        nlohmann::json written;
        written["name"]       = names[i];
        written["components"] = nlohmann::json::object();

        for (const Core::Reflect::ComponentMeta *meta : registry.SerializableComponents())
        {
            if (meta->name == "Name")
                continue; // the entity's `name` key already carries it

            const void *component = meta->getByEntity(&scene, entity.index, entity.generation);
            if (component == nullptr)
                continue;
            written["components"][meta->name] = meta->serialize(component);
        }

        // Around the new file's own origin, so the result is placeable. A parented
        // entity is already relative to its parent and reaches the origin through
        // the chain; dividing it too would divide twice.
        if (!scene.Has<Parent>(entity))
        {
            if (const ECS::Transform *transform = scene.Get<ECS::Transform>(entity))
                written["components"]["Transform"] = TransformToJson(InverseComposeTransform(origin, *transform));
        }

        doc["entities"].push_back(std::move(written));
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        Core::Log::Error("Blueprint: cannot open '{}' for writing.", path.string());
        return false;
    }
    file << doc.dump(2);
    if (!file.good())
    {
        Core::Log::Error("Blueprint: write failed for '{}'.", path.string());
        return false;
    }

    return true;
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

LevelResult SceneSerializer::LoadFromDisk(ECS::Scene &scene, const std::filesystem::path &path,
                                   const ProgressFn &onProgress, LevelHeader *header, InstanceTable *instances)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        Core::Log::Error("SceneSerializer: cannot open '{}' for reading", path.string());
        return std::unexpected(LevelError::FileUnreadable);
    }

    const nlohmann::json doc = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded())
    {
        Core::Log::Error("SceneSerializer: '{}' is not readable JSON", path.string());
        return std::unexpected(LevelError::MalformedJson);
    }

    try
    {
        return Load(scene, doc, onProgress, header, instances);
    }
    catch (const std::exception &ex)
    {
        // Same contract as LoadFromFile below: a failed load yields an empty
        // scene rather than a half-populated one. Load itself no longer throws —
        // this is for the component `addToScene` hooks it runs, which are
        // generated code over nlohmann and can.
        Core::Log::Error("SceneSerializer: failed to load '{}': {}", path.string(), ex.what());
        scene.Clear();
        return std::unexpected(LevelError::MalformedJson);
    }
}

std::expected<std::vector<std::string>, LevelError> SceneSerializer::ReadLevelSystems(
    std::string_view assetPath)
{
    const auto text = Core::AssetSystem::ReadText(assetPath);
    if (!text)
    {
        Core::Log::Error("SceneSerializer: cannot read asset '{}'", assetPath);
        return std::unexpected(LevelError::FileUnreadable);
    }

    const nlohmann::json doc = nlohmann::json::parse(*text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded())
    {
        Core::Log::Error("SceneSerializer: cannot parse '{}'", assetPath);
        return std::unexpected(LevelError::MalformedJson);
    }

    std::vector<std::string> out;
    if (const auto it = doc.find("systems"); it != doc.end() && it->is_array())
    {
        for (const auto &name : *it)
        {
            if (name.is_string())
                out.push_back(name.get<std::string>());
        }
    }
    return out;
}

LevelResult SceneSerializer::LoadFromFile(ECS::Scene &scene, std::string_view assetPath,
                                          const ProgressFn &onProgress, LevelHeader *header,
                                          InstanceTable *instances)
{
    const auto text = Core::AssetSystem::ReadText(assetPath);
    if (!text)
    {
        Core::Log::Error("SceneSerializer: cannot read asset '{}'", assetPath);
        return std::unexpected(LevelError::FileUnreadable);
    }

    // A parse failure leaves the scene untouched, so it is refused before Load is
    // ever called rather than caught after.
    const nlohmann::json doc = nlohmann::json::parse(*text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded())
    {
        Core::Log::Error("SceneSerializer: '{}' is not readable JSON", assetPath);
        return std::unexpected(LevelError::MalformedJson);
    }

    try
    {
        return Load(scene, doc, onProgress, header, instances);
    }
    catch (const std::exception &ex)
    {
        // Load reports its own failures by value and clears as it goes; what is
        // left for this to catch is a throw out of a component's addToScene hook
        // partway through, which leaves the scene half-populated. Clear it so a
        // failed load yields an empty scene, never a corrupt one.
        // (ScopedContextReset in Load already freed s_context.)
        //
        // Catches std::exception, not just json::exception: Load runs arbitrary
        // component addToScene hooks, and one throwing anything else (bad_alloc,
        // out_of_range from a hook's own container) would otherwise escape and
        // leave the half-populated scene behind.
        Core::Log::Error("SceneSerializer: failed to load '{}': {}", assetPath, ex.what());
        scene.Clear();
        return std::unexpected(LevelError::MalformedJson);
    }
}

} // namespace Assisi::Runtime