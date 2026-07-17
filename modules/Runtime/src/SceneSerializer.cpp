/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>

#include <cmath>
#include <fstream>
#include <map>
#include <optional>
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

std::optional<uint32_t> SceneSerializer::EntityToIndex(ECS::Entity entity)
{
    // Raw-entity context wins: identity mapping, the slot index itself is the key.
    if (s_rawContextScene != nullptr)
        return entity.index;

    if (!s_context)
        return std::nullopt;

    const uint64_t key = EntityKey(entity.index, entity.generation);
    const auto it = s_context->entityToIndex.find(key);
    if (it == s_context->entityToIndex.end())
        return std::nullopt;

    return it->second;
}

ECS::Entity SceneSerializer::IndexToEntity(uint32_t index)
{
    // Raw-entity context wins: the "index" is a raw slot; resolve the live handle
    // there (exact because the paired restore revived it at its original slot).
    if (s_rawContextScene != nullptr)
        return s_rawContextScene->EntityAt(index);

    if (!s_context || index >= s_context->indexToEntity.size())
        return ECS::NullEntity;

    return s_context->indexToEntity[index];
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

nlohmann::json SceneSerializer::Save(ECS::Scene &scene)
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
    result["version"]  = 1;
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

void SceneSerializer::Load(ECS::Scene &scene, const nlohmann::json &j)
{
    const int version = j.value("version", 0);
    if (version != 1)
    {
        Core::Log::Error("SceneSerializer: unsupported level file version {}", version);
        return;
    }

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
    for (size_t i = 0; i < entities.size(); ++i)
    {
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

bool SceneSerializer::SaveToFile(ECS::Scene &scene, const std::filesystem::path &path)
{
    std::ofstream f(path);
    if (!f.is_open())
    {
        Core::Log::Error("SceneSerializer: cannot open '{}' for writing", path.string());
        return false;
    }
    f << Save(scene).dump(2);
    if (!f.good())
    {
        Core::Log::Error("SceneSerializer: write failed for '{}'", path.string());
        return false;
    }
    return true;
}

bool SceneSerializer::LoadFromFile(ECS::Scene &scene, std::string_view assetPath)
{
    const auto text = Core::AssetSystem::ReadText(assetPath);
    if (!text)
    {
        Core::Log::Error("SceneSerializer: cannot read asset '{}'", assetPath);
        return false;
    }

    try
    {
        Load(scene, nlohmann::json::parse(*text));
        return true;
    }
    catch (const nlohmann::json::exception &ex)
    {
        // A parse error leaves the scene untouched; a throw partway through
        // Load (a malformed component field) leaves it half-populated. Clear
        // it either way so a failed load yields an empty scene, never a
        // corrupt one. (ScopedContextReset in Load already freed s_context.)
        Core::Log::Error("SceneSerializer: JSON error in '{}': {}", assetPath, ex.what());
        scene.Clear();
        return false;
    }
}

} // namespace Assisi::Runtime