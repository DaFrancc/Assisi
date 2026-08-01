/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <algorithm>
#include <span>
#include <vector>

#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Render/DrawItem.hpp>
#include <Assisi/Render/GpuMarker.hpp>
#include <Assisi/Render/Frustum.hpp>
#include <Assisi/Render/MeshCuller.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Renderer.hpp>

namespace Assisi::Runtime
{
namespace
{
// GPU-driven cull path (stage F1). Instead of culling + emitting + sorting draws
// on the CPU, gather every mesh entity into the culler's host tables (deduping
// meshes into a descriptor table), let the compute pass frustum-cull and build the
// indirect commands + per-instance records on the GPU, then issue one
// drawIndexedIndirectCount. The CPU still iterates the ECS to build the tables
// (the cull *math* is what moved to the GPU); a dirty-tracked ECS→GPU mirror that
// removes this gather too is stage F2. @p frustum's planes drive the GPU test.
DrawStats DrawSceneGpu(const DrawSceneParams &params, const Assisi::Render::Frustum &frustum)
{
    Assisi::ECS::Scene              &scene   = params.scene;
    Assisi::Render::CullTableBuilder &builder = *params.cullBuilder;

    DrawStats stats;

    // The ECS walk that stage F2 exists to delete. Keeping it under its own name
    // is the point: when the dirty-tracked mirror lands, this slice is the
    // before/after measurement.
    const Assisi::Render::MeshBuffer *anyMesh = nullptr;
    {
        ASSISI_PROFILE_SCOPE("cull-gather");
        builder.Reset();
        for (auto [entity, transform, meshRenderer] : scene.Query<Transform, MeshRenderer>())
        {
            const Assisi::Render::MeshBuffer *mesh = meshRenderer.meshBuffer;
            if (mesh == nullptr)
            {
                continue;
            }
            anyMesh = mesh; // any mesh identifies the shared arena's vertex/index buffers (single arena, F1)
            builder.AddInstance(mesh, transform.worldMatrix,
                                std::span<const Assisi::Render::Material *const>(meshRenderer.materials.data(),
                                                                                meshRenderer.materials.size()));
        }
    }

    {
        ASSISI_PROFILE_SCOPE("cull-finalize");
        builder.Finalize(); // build the per-batch draw templates from the gathered tables
    }

    const Assisi::Render::CullTables &tables = builder.Tables();
    if (tables.Empty() || anyMesh == nullptr)
    {
        return stats; // nothing to draw
    }

    {
        // Recording the compute dispatch, not running it — the cull's real cost is
        // GPU-side. A CPU spike here means buffer uploads, not culling.
        ASSISI_PROFILE_GPU_SCOPE(params.frame.commandList, "cull-dispatch");
        params.culler->Cull(params.frame.commandList, frustum.Planes(), tables, params.frustumCulling);
    }

    Assisi::Render::MeshPass::IndirectDrawInputs inputs;
    inputs.instanceBuffer = params.culler->InstanceBuffer();
    inputs.indirectBuffer = params.culler->IndirectBuffer();
    inputs.commandCount   = params.culler->IndirectCommandCount();
    inputs.vertexBuffer   = anyMesh->VertexBuffer();
    inputs.indexBuffer    = anyMesh->IndexBuffer();

    Assisi::Render::MeshPass::SubmitStats submitStats;
    {
        ASSISI_PROFILE_GPU_SCOPE(params.frame.commandList, "submit-indirect");
        submitStats = params.meshPass.SubmitIndirect(params.frame, inputs);
    }

    // Survivor tallies read back from the GPU (a few frames stale). F2a coalesces
    // identical (mesh,submesh) instances into one instanced draw, so `batches` is
    // the live batch count (falls well below drawnItems) — stage E's win, GPU-side.
    // culledMeshes counts culled submesh-instances (candidates − survivors).
    const uint32_t survivors  = params.culler->SurvivorInstanceCount();
    const uint32_t candidates = params.culler->CandidateInstanceCount();
    stats.drawnItems   = survivors;
    stats.culledMeshes = candidates > survivors ? candidates - survivors : 0;
    stats.batches      = params.culler->SurvivorBatchCount();
    stats.drawCalls    = submitStats.drawCalls;
    return stats;
}
} // namespace

DrawStats DrawScene(const DrawSceneParams &params)
{
    ASSISI_PROFILE_GPU_SCOPE(params.frame.commandList, "draw-scene");

    Assisi::ECS::Scene            &scene    = params.scene;
    const Assisi::Render::MeshPass &meshPass = params.meshPass;
    const glm::mat4                &view     = params.view;

    const glm::mat4 viewProjection = params.projection * view;

    // Cull each mesh's world-space bounding sphere against the view frustum before
    // emitting its submeshes, so off-screen geometry costs a matrix-times-point and
    // six dot products instead of any draw work.
    const Assisi::Render::Frustum frustum = Assisi::Render::Frustum::FromViewProjection(viewProjection);

    // GPU-driven path (stage F1): the compute cull replaces this CPU extract/sort.
    // Falls back to the CPU path if the culler/builder aren't wired or ready.
    if (params.gpuCulling && params.culler != nullptr && params.cullBuilder != nullptr && params.culler->IsValid())
    {
        return DrawSceneGpu(params, frustum);
    }

    DrawStats                          stats;
    std::vector<Assisi::Render::DrawItem> items;

    // The CPU cull + emit. Read against render/culled-meshes: this walks every
    // mesh entity, and only the survivors reach `draw-sort`.
    {
        ASSISI_PROFILE_SCOPE("draw-extract");
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
    }

    // Material/mesh-major, front-to-back within a run. Off = query order (A/B: the
    // image is identical, the bind counts differ). std::sort is not stable, but the
    // sort key is a total order over what matters, so stability is irrelevant.
    if (params.sortDraws)
    {
        // Scoped rather than folded into draw-extract precisely because it is the
        // half the `sortDraws` toggle turns off — the A/B is a slice appearing or
        // not, with no arithmetic.
        ASSISI_PROFILE_SCOPE("draw-sort");
        std::sort(items.begin(), items.end(),
                  [](const Assisi::Render::DrawItem &lhs, const Assisi::Render::DrawItem &rhs)
                  { return lhs.sortKey < rhs.sortKey; });
    }

    Assisi::Render::MeshPass::SubmitStats submitStats;
    {
        ASSISI_PROFILE_GPU_SCOPE(params.frame.commandList, "draw-submit");
        submitStats = meshPass.Submit(params.frame, items);
    }

    stats.drawnItems = submitStats.instances;
    stats.batches    = submitStats.batches;
    stats.drawCalls  = submitStats.drawCalls;
    return stats;
}

} // namespace Assisi::Runtime
