/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <algorithm>
#include <vector>

#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Render/DrawItem.hpp>
#include <Assisi/Render/Frustum.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Renderer.hpp>

namespace Assisi::Runtime
{

DrawStats DrawScene(const DrawSceneParams &params)
{
    Assisi::ECS::Scene            &scene    = params.scene;
    const Assisi::Render::MeshPass &meshPass = params.meshPass;
    const glm::mat4                &view     = params.view;

    const glm::mat4 viewProjection = params.projection * view;

    // Cull each mesh's world-space bounding sphere against the view frustum before
    // emitting its submeshes, so off-screen geometry costs a matrix-times-point and
    // six dot products instead of any draw work.
    const Assisi::Render::Frustum frustum = Assisi::Render::Frustum::FromViewProjection(viewProjection);

    DrawStats                          stats;
    std::vector<Assisi::Render::DrawItem> items;

    for (auto [entity, transform, meshRenderer] : scene.Query<Transform, MeshRenderer>())
    {
        const Assisi::Render::MeshBuffer *mesh = meshRenderer.meshBuffer;
        if (mesh == nullptr)
        {
            continue;
        }

        if (params.frustumCulling)
        {
            // Two-level whole-mesh cull: the cheap sphere reject (one point
            // transform + a scalar) throws out most off-screen meshes, then the
            // tighter AABB refine catches ones the sphere's isotropic radius kept
            // but the box excludes. Both are conservative — nothing visible is
            // ever culled; the AABB just wastes fewer draws on flat/elongated meshes.
            const Assisi::Geometry::BoundingSphere worldSphere =
                Assisi::Geometry::TransformedBoundingSphere(mesh->LocalBounds(), transform.worldMatrix);
            if (!frustum.IntersectsSphere(worldSphere))
            {
                ++stats.culledMeshes;
                continue;
            }
            const Assisi::Geometry::Aabb worldAabb =
                Assisi::Geometry::TransformedAabb(mesh->LocalAabb(), transform.worldMatrix);
            if (!frustum.IntersectsAabb(worldAabb))
            {
                ++stats.culledMeshes;
                continue;
            }
        }

        // One depth for the whole mesh (its center's view-space distance). All its
        // submeshes share it — they sort together by mesh anyway; the depth field
        // only orders distinct meshes front-to-back within a material run.
        const glm::vec3 centerWorld =
            glm::vec3(transform.worldMatrix * glm::vec4(mesh->LocalBounds().center, 1.f));
        const float    viewDistance = -(view * glm::vec4(centerWorld, 1.f)).z; // camera looks down -Z
        const uint16_t depth = Assisi::Render::QuantizeDepthFrontToBack(viewDistance, params.nearZ, params.farZ);

        // LOD0 only for now (screen-size LOD selection is a later stage; the seam is
        // ready for it). EnsureSubMeshTables guarantees at least one LOD/submesh.
        const std::vector<Assisi::Geometry::SubMesh>  &subMeshes = mesh->SubMeshes();
        const std::vector<Assisi::Geometry::LodRange> &lods      = mesh->Lods();
        const Assisi::Geometry::LodRange               lod0 =
            !lods.empty() ? lods.front()
                          : Assisi::Geometry::LodRange{0, static_cast<uint32_t>(subMeshes.size())};

        for (uint32_t i = 0; i < lod0.SubMeshCount; ++i)
        {
            const uint32_t                     submeshIndex = lod0.FirstSubMesh + i;
            const Assisi::Geometry::SubMesh   &subMesh      = subMeshes[submeshIndex];
            const Assisi::Render::Material    *material =
                subMesh.MaterialSlot < meshRenderer.materials.size() ? meshRenderer.materials[subMesh.MaterialSlot]
                                                                        : nullptr;
            if (material == nullptr)
            {
                continue; // no material resolved for this slot — skip rather than guess
            }

            const uint64_t sortKey =
                Assisi::Render::MakeOpaqueSortKey(0, material->Id(), mesh->Id(), depth);
            items.push_back(Assisi::Render::DrawItem{.sortKey      = sortKey,
                                                     .mesh         = mesh,
                                                     .submeshIndex = submeshIndex,
                                                     .material     = material,
                                                     .model        = transform.worldMatrix});
        }
    }

    // Material/mesh-major, front-to-back within a run. Off = query order (A/B: the
    // image is identical, the bind counts differ). std::sort is not stable, but the
    // sort key is a total order over what matters, so stability is irrelevant.
    if (params.sortDraws)
    {
        std::sort(items.begin(), items.end(),
                  [](const Assisi::Render::DrawItem &lhs, const Assisi::Render::DrawItem &rhs)
                  { return lhs.sortKey < rhs.sortKey; });
    }

    const Assisi::Render::MeshPass::SubmitStats submitStats = meshPass.Submit(params.frame, items);

    stats.drawnItems = submitStats.instances;
    stats.batches    = submitStats.batches;
    stats.drawCalls  = submitStats.drawCalls;
    return stats;
}

} // namespace Assisi::Runtime
