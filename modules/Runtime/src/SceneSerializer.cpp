/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <unordered_map>
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
    // Save: entity key (gen<<32|idx) → serial index
    std::unordered_map<uint64_t, uint32_t> entityToIndex;

    // Load: serial index → live Entity
    std::vector<ECS::Entity> indexToEntity;
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

std::optional<uint64_t> SceneSerializer::EntityToIndex(ECS::Entity entity)
{
    // Raw-entity context wins: identity mapping, keyed by slot AND generation.
    //
    // Round-6 M11: this used to return the bare slot index, dropping the
    // generation. A ref captured to an entity that was later destroyed then
    // resolved to whatever new entity reused the slot — silently, because the
    // reused slot is perfectly alive, so no liveness check can catch it. Packing
    // the generation in lets IndexToEntity below reject the stale ref instead.
    if (s_rawContextScene != nullptr)
        return EntityKey(entity.index, entity.generation);

    if (!s_context)
        return std::nullopt;

    const uint64_t key = EntityKey(entity.index, entity.generation);
    const auto it = s_context->entityToIndex.find(key);
    if (it == s_context->entityToIndex.end())
        return std::nullopt;

    return it->second;
}

ECS::Entity SceneSerializer::IndexToEntity(uint64_t index)
{
    // Raw-entity context wins: `index` is a packed (slot, generation) key. The
    // paired restore revives entities at their original handle, so in the intended
    // flow the generation matches exactly. When it does not, the slot has been
    // recycled by an unrelated entity and the ref is stale — resolve to null
    // rather than silently redirecting onto whoever moved in (round-6 M11).
    if (s_rawContextScene != nullptr)
    {
        const auto        slot    = static_cast<uint32_t>(index & 0xFFFFFFFFull);
        const auto        wantGen = static_cast<uint32_t>(index >> 32);
        const ECS::Entity live    = s_rawContextScene->EntityAt(slot);
        return live.generation == wantGen ? live : ECS::NullEntity;
    }

    if (!s_context || index >= s_context->indexToEntity.size())
        return ECS::NullEntity;

    return s_context->indexToEntity[static_cast<std::size_t>(index)];
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
    s_context = SerializationContext{};

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
} // namespace

nlohmann::json SceneSerializer::Save(ECS::Scene &scene, const LevelHeader &header)
{
    auto &registry = Core::Reflect::ComponentRegistry::Instance();

    // Pass 1: collect all entity keys into a sorted map so serial indices
    // match the final array order. No serialization yet.
    std::map<uint64_t, nlohmann::json> entityMap;

    for (const auto *meta : registry.SerializableComponents())
    {
        meta->iterateEntities(&scene, [&](uint32_t idx, uint32_t gen, const void *)
        {
            entityMap.emplace(EntityKey(idx, gen), nlohmann::json{});
        });
    }

    // Build entityToIndex from the sorted map (deterministic order).
    SerializationContext ctx;
    uint32_t serialIdx = 0;
    for (const auto &entry : entityMap)
        ctx.entityToIndex.emplace(entry.first, serialIdx++);

    s_context = std::move(ctx);
    const ScopedContextReset contextReset;

    // Pass 2: serialize components (context is live so EntityToIndex works).
    for (const auto *meta : registry.SerializableComponents())
    {
        meta->iterateEntities(&scene, [&](uint32_t idx, uint32_t gen, const void *compPtr)
        {
            const uint64_t key = EntityKey(idx, gen);
            entityMap[key]["components"][meta->name] = meta->serialize(compPtr);
        });
    }

    nlohmann::json result;
    result["version"] = 1;
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
    if (version != 1)
    {
        Core::Log::Error("SceneSerializer: unsupported level file version {}", version);
        return;
    }

    if (header != nullptr)
        header->profile = j.value("profile", std::string{});

    auto &registry = Core::Reflect::ComponentRegistry::Instance();

    scene.Clear();

    s_context = SerializationContext{};
    const ScopedContextReset contextReset;

    const auto &entities = j.at("entities");

    // Pass 1: create every entity up front so indexToEntity is complete before
    // any component deserializes. Component EntityRef fields resolve through
    // IndexToEntity, and a reference may point *forward* to an entity that has
    // not been created yet (e.g. a child serialized before its parent after
    // slot reuse). A single pass would resolve those to NullEntity and silently
    // flatten the hierarchy, so all handles must exist before pass 2 runs.
    s_context->indexToEntity.reserve(entities.size());
    for (size_t i = 0; i < entities.size(); ++i)
        s_context->indexToEntity.push_back(scene.Create());

    // Pass 2: deserialize components now that every EntityRef can resolve.
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

        const ECS::Entity e = s_context->indexToEntity[i];
        for (const auto &[compName, compData] : entityJson.at("components").items())
        {
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
}

// ---------------------------------------------------------------------------
// File I/O helpers
// ---------------------------------------------------------------------------

bool SceneSerializer::SaveToFile(ECS::Scene &scene, const std::filesystem::path &path,
                                 const LevelHeader &header)
{
    std::ofstream f(path);
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