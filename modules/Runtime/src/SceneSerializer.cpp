/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>

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
    if (!s_context || index >= s_context->indexToEntity.size())
        return ECS::NullEntity;

    return s_context->indexToEntity[index];
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

nlohmann::json SceneSerializer::Save(ECS::Scene &scene)
{
    auto &registry = Core::Reflect::ComponentRegistry::Instance();

    // Pass 1: collect all entity keys into a sorted map so serial indices
    // match the final array order. No serialization yet.
    std::map<uint64_t, nlohmann::json> entityMap;

    for (const auto &meta : registry.All())
    {
        if (!meta.iterateEntities)
            continue;

        meta.iterateEntities(&scene, [&](uint32_t idx, uint32_t gen, const void *)
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
    for (const auto &meta : registry.All())
    {
        if (!meta.iterateEntities)
            continue;

        meta.iterateEntities(&scene, [&](uint32_t idx, uint32_t gen, const void *compPtr)
        {
            const uint64_t key = EntityKey(idx, gen);
            entityMap[key]["components"][meta.name] = meta.serialize(compPtr);
        });
    }

    nlohmann::json result;
    result["version"]  = 1;
    result["entities"] = nlohmann::json::array();

    for (auto &[key, entityJson] : entityMap)
        result["entities"].push_back(std::move(entityJson));

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