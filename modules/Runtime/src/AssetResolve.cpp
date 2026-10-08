/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/AssetResolve.hpp>

#include <Assisi/Runtime/SkinnedMeshPose.hpp>

#include <cstddef>
#include <cstdint>

namespace Assisi::Runtime
{

void ResolveMeshRendererAssets(MeshRenderer &meshRenderer, Render::AssetCache &cache)
{
    meshRenderer.meshBuffer = cache.ResolveMesh(meshRenderer.mesh);

    // One resolved Material per mesh slot: the override when that slot has a
    // non-nil entry, otherwise the default the mesh loaded with. A primitive mesh
    // has no slot table, so `materials` stays empty and the draw path uses the
    // cache's fallback.
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
                                             : cache.SlotMaterial(meshRenderer.mesh, static_cast<uint32_t>(slot));
        // ResolveMaterial(nil) yields the fallback, so a slot with no recorded
        // material still renders.
        meshRenderer.materials.push_back(cache.ResolveMaterial(materialId));
    }
}

void ResolveSceneAssets(ECS::Scene &scene, Render::AssetCache &cache)
{
    for (auto [entity, meshRenderer] : scene.Query<Mut<MeshRenderer>>())
        ResolveMeshRendererAssets(meshRenderer, cache);

    // Bound here as well as before each evaluation, so a freshly loaded skinned
    // mesh holds its rest pose before anything reads it.
    for (auto [entity, skinned, meshRenderer] : scene.Query<Mut<SkinnedMesh>, MeshRenderer>())
    {
        (void)BindSkinnedMesh(skinned, meshRenderer);
    }
}

void ClearSceneAssetBindings(ECS::Scene &scene)
{
    for (auto [entity, meshRenderer] : scene.Query<Mut<MeshRenderer>>())
    {
        meshRenderer.meshBuffer = nullptr;
        meshRenderer.materials.clear();
    }
    for (auto [entity, skinned] : scene.Query<Mut<SkinnedMesh>>())
    {
        UnbindSkinnedMesh(skinned);
    }
}

} // namespace Assisi::Runtime
