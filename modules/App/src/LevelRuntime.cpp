/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/LevelRuntime.hpp>

#include <Assisi/Core/AssetIdJson.hpp>
#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/Runtime/AssetResolve.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <optional>
#include <string>

namespace Assisi::App
{

void InstallAssetResolvers(Render::AssetCache &cache, const Core::AssetDatabase &database)
{
    // Serialization's path hint (asset-database D2): saved GUID references carry
    // a readable last-known path regenerated from the database.
    Core::SetAssetIdHintResolver([&database](const Core::AssetId &id)
                                 { return database.PathFor(id).value_or(std::string{}); });

    // The cache: id↔path so mesh/material/texture resolution and glTF import
    // speak GUIDs. Reserved built-ins resolve without the database.
    cache.SetAssetResolvers(
        [&database](const Core::AssetId &id) -> Core::AssetPath
        {
            const std::optional<std::string> path = database.PathFor(id);
            return path ? Core::AssetPath{std::string_view{*path}} : Core::AssetPath{};
        },
        [&database](std::string_view virtualPath) -> Core::AssetId
        { return database.IdFor(virtualPath).value_or(Core::AssetId{}); });
}

void RebindSceneAssetsAndPhysics(ECS::Scene &scene, Render::AssetCache &cache, const Core::AssetDatabase &database,
                                 Physics::PhysicsWorld &physics)
{
    Runtime::ResolveSceneAssets(scene, cache, database);
    physics.RebuildSceneBodies(scene);
}

bool LoadLevel(ECS::Scene &scene, std::string_view virtualPath, Render::AssetCache &cache,
               const Core::AssetDatabase &database, Physics::PhysicsWorld &physics,
               Runtime::SceneRenderer &sceneRenderer, AssetCacheReset reset,
               Runtime::LevelHeader *header)
{
    if (!Runtime::SceneSerializer::LoadFromFile(scene, virtualPath, /*onProgress=*/{}, header))
        return false;

    if (reset == AssetCacheReset::ClearFirst)
    {
        // New asset set, and nothing else resident: drop the old cache and evict
        // the mesh pass's binding sets (they key on raw texture pointers we're
        // about to free) before re-resolving. With another world alive this would
        // dangle its resolved pointers, which is why it is a caller's choice.
        cache.Clear();
        sceneRenderer.InvalidateAssetBindings();
    }

    RebindSceneAssetsAndPhysics(scene, cache, database, physics);
    return true;
}

void UpgradeStreamingAssets(ECS::Scene &scene, Render::AssetCache &cache, const Core::AssetDatabase &database,
                            bool &wereLoading)
{
    // Re-resolve while loads are pending, and for one frame after the last one
    // finishes so the final result is picked up.
    const bool loadsPending = cache.HasPendingLoads();
    if (loadsPending || wereLoading)
    {
        Runtime::ResolveSceneAssets(scene, cache, database);
        wereLoading = loadsPending;
    }
}

} // namespace Assisi::App
