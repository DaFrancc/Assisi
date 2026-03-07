#include <Assisi/Runtime/SceneSerializer.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>

#include <fstream>
#include <map>

namespace Assisi::Runtime
{

nlohmann::json SceneSerializer::Save(ECS::Scene &scene)
{
    auto &registry = Core::Reflect::ComponentRegistry::Instance();

    // Build a map from entity key → component JSON objects.
    // Using std::map keeps entities in a deterministic (index-sorted) order.
    std::map<uint64_t, nlohmann::json> entityMap;

    for (const auto &meta : registry.All())
    {
        if (!meta.iterateEntities)
            continue;

        meta.iterateEntities(&scene, [&](uint32_t idx, uint32_t gen, const void *compPtr)
        {
            const uint64_t key = (static_cast<uint64_t>(gen) << 32) | idx;
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

    for (const auto &entityJson : j.at("entities"))
    {
        const ECS::Entity e = scene.Create();

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
}

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