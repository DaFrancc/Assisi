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

    s_context.reset();

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

    for (const auto &entityJson : j.at("entities"))
    {
        const ECS::Entity e = scene.Create();
        s_context->indexToEntity.push_back(e);

        if (!entityJson.contains("components"))
            continue;

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

    s_context.reset();
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
    return f.good();
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
        Core::Log::Error("SceneSerializer: JSON error in '{}': {}", assetPath, ex.what());
        return false;
    }
}

} // namespace Assisi::Runtime