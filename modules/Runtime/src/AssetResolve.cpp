/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/AssetResolve.hpp>

#include <cstddef>
#include <cstdint>

namespace Assisi::Runtime
{

void ResolveMeshRendererAssets(MeshRenderer &meshRenderer, Render::AssetCache &cache,
                               const Core::AssetDatabase &database)
{
    meshRenderer.meshBuffer = cache.ResolveMesh(meshRenderer.mesh);

    // One resolved Material per mesh slot: the override when the slot has a
    // non-nil entry, otherwise the mesh's default for that slot — the material
    // GUID the glTF manifest recorded at import, read from the database rather
    // than re-derived live. A primitive mesh has no slot table, so this leaves
    // `materials` empty and the draw path uses the cache's fallback.
    const std::size_t slotCount =
        meshRenderer.meshBuffer != nullptr ? meshRenderer.meshBuffer->Materials().size() : 0;
    meshRenderer.materials.clear();
    meshRenderer.materials.reserve(slotCount);
    for (std::size_t slot = 0; slot < slotCount; ++slot)
    {
        const bool hasOverride =
            slot < meshRenderer.materialOverrides.size() && !meshRenderer.materialOverrides[slot].IsNil();
        const Core::AssetId materialId = hasOverride
                                             ? meshRenderer.materialOverrides[slot]
                                             : database.SlotMaterial(meshRenderer.mesh, static_cast<uint32_t>(slot));
        // ResolveMaterial(nil) yields the fallback, so an unexploded mesh or an
        // out-of-range slot still renders.
        meshRenderer.materials.push_back(cache.ResolveMaterial(materialId));
    }
}

void ResolveSceneAssets(ECS::Scene &scene, Render::AssetCache &cache, const Core::AssetDatabase &database)
{
    for (auto [entity, meshRenderer] : scene.Query<MeshRenderer>())
        ResolveMeshRendererAssets(meshRenderer, cache, database);
}

void ClearSceneAssetBindings(ECS::Scene &scene)
{
    for (auto [entity, meshRenderer] : scene.Query<MeshRenderer>())
    {
        meshRenderer.meshBuffer = nullptr;
        meshRenderer.materials.clear();
    }
}

} // namespace Assisi::Runtime
