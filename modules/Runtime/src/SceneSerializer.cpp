/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
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

    auto       name = value.get<std::string>();
    const auto it   = s_context->nameToEntity.find(name);
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
} // namespace

nlohmann::json SceneSerializer::Save(ECS::Scene &scene, const LevelHeader &header)
{
    auto &registry = Core::Reflect::ComponentRegistry::Instance();

    // Pass 1: collect all entity keys into a sorted map so the array order is
    // deterministic. No serialization yet.
    std::map<uint64_t, nlohmann::json> entityMap;

    for (const auto *meta : registry.SerializableComponents())
    {
        meta->iterateEntities(&scene, [&](uint32_t idx, uint32_t gen, const void *)
        {
            entityMap.emplace(EntityKey(idx, gen), nlohmann::json{});
        });
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
            entityMap[key]["components"][meta->name] = meta->serialize(compPtr);
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
                           LevelHeader *header)
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

    if (header != nullptr)
        header->profile = j.value("profile", std::string{});

    auto &registry = Core::Reflect::ComponentRegistry::Instance();

    scene.Clear();

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
// File I/O helpers
// ---------------------------------------------------------------------------

bool SceneSerializer::SaveToFile(ECS::Scene &scene, const std::filesystem::path &path,
                                 const LevelHeader &header)
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
    f << Save(scene, header).dump(2);
    if (!f.good())
    {
        Core::Log::Error("SceneSerializer: write failed for '{}'", path.string());
        return false;
    }
    return true;
}

bool SceneSerializer::LoadFromDisk(ECS::Scene &scene, const std::filesystem::path &path,
                                   const ProgressFn &onProgress, LevelHeader *header)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        Core::Log::Error("SceneSerializer: cannot open '{}' for reading", path.string());
        return false;
    }

    try
    {
        Load(scene, nlohmann::json::parse(file), onProgress, header);
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

bool SceneSerializer::LoadFromFile(ECS::Scene &scene, std::string_view assetPath,
                                   const ProgressFn &onProgress, LevelHeader *header)
{
    const auto text = Core::AssetSystem::ReadText(assetPath);
    if (!text)
    {
        Core::Log::Error("SceneSerializer: cannot read asset '{}'", assetPath);
        return false;
    }

    try
    {
        Load(scene, nlohmann::json::parse(*text), onProgress, header);
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