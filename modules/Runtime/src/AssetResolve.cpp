/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/AssetResolve.hpp>

#include <Assisi/Runtime/SkinnedMeshPose.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Assisi::Runtime
{

namespace
{
/// The mesh @p meshRenderer names, with its materials written to @p materials.
const Render::MeshBuffer *ResolvedAssets(const MeshRenderer &meshRenderer, Render::AssetCache &cache,
                                         std::vector<const Render::Material *> &materials)
{
    const Render::MeshBuffer *mesh = cache.ResolveMesh(meshRenderer.mesh);

    // One resolved Material per mesh slot: the override when that slot has a
    // non-nil entry, otherwise the default the mesh loaded with. A primitive mesh
    // has no slot table, so `materials` stays empty and the draw path uses the
    // cache's fallback.
    const std::size_t slotCount = mesh != nullptr ? mesh->Materials().size() : 0;
    materials.clear();
    materials.reserve(slotCount);
    for (std::size_t slot = 0; slot < slotCount; ++slot)
    {
        const bool hasOverride =
            slot < meshRenderer.materialOverrides.size() && !meshRenderer.materialOverrides[slot].IsNil();
        const Core::AssetId materialId = hasOverride
                                             ? meshRenderer.materialOverrides[slot]
                                             : cache.SlotMaterial(meshRenderer.mesh, static_cast<uint32_t>(slot));
        // ResolveMaterial(nil) yields the fallback, so a slot with no recorded
        // material still renders.
        materials.push_back(cache.ResolveMaterial(materialId));
    }
    return mesh;
}
} // namespace

void ResolveMeshRendererAssets(MeshRenderer &meshRenderer, Render::AssetCache &cache)
{
    meshRenderer.meshBuffer = ResolvedAssets(meshRenderer, cache, meshRenderer.materials);
}

void ResolveSceneAssets(ECS::Scene &scene, Render::AssetCache &cache)
{
    // Read first and written only where the answer changed: this runs every
    // frame while anything streams, and a write marks the MeshRenderer changed,
    // which makes its shadow redraw.
    std::vector<const Render::Material *> materials;
    std::vector<ECS::Entity> changed;
    for (auto [entity, meshRenderer] : scene.Query<MeshRenderer>())
    {
        const Render::MeshBuffer *mesh = ResolvedAssets(meshRenderer, cache, materials);
        if (mesh != meshRenderer.meshBuffer || materials != meshRenderer.materials)
        {
            changed.push_back(entity);
        }
    }
    for (const ECS::Entity entity : changed)
    {
        ResolveMeshRendererAssets(*scene.GetMut<MeshRenderer>(entity), cache);
    }

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
