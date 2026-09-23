/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Runtime/Renderer.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Render/DrawItem.hpp>
#include <Assisi/Render/Frustum.hpp>
#include <Assisi/Render/GpuMarker.hpp>
#include <Assisi/Render/MeshCuller.hpp>
#include <Assisi/Runtime/Components.hpp>

namespace Assisi::Runtime
{
Assisi::Geometry::LodRange LodRangeFor(const Assisi::Render::MeshBuffer &mesh, uint32_t level)
{
    const std::vector<Assisi::Geometry::LodRange> &lods = mesh.Lods();
    if (lods.empty())
    {
        return Assisi::Geometry::LodRange{0, static_cast<uint32_t>(mesh.SubMeshes().size())};
    }
    return lods[std::min<std::size_t>(level, lods.size() - 1u)];
}

namespace
{
// GPU-driven cull path. Instead of culling + emitting + sorting draws on the CPU,
// gather every mesh entity into the culler's host tables (deduping meshes into a
// descriptor table), let the compute pass frustum-cull, select each survivor's
// LOD level and build the indirect commands + per-instance records on the GPU,
// then issue one drawIndexedIndirect. The CPU still iterates the ECS to build the
// tables (the cull and selection *math* is what moved to the GPU). @p frustum's
// planes drive the GPU test.
//
// The CPU names a level wherever one is named — a viewport force, the pin on one
// instance, selection off — and a frame with no view to measure in is LOD0, as it
// is on the CPU path. Everything else the pass measures.
DrawStats DrawSceneGpu(const DrawSceneParams &params, const Assisi::Render::Frustum &frustum)
{
    Assisi::ECS::Scene &scene = params.scene;
    Assisi::Render::CullTableBuilder &builder = *params.cullBuilder;
    const LodSelector *lodSelector = params.lodSelector;

    DrawStats stats;

    // No selector at all is LOD0, what the path drew before selection existed.
    const bool canMeasure = lodSelector != nullptr && lodSelector->View().tanHalfFovY > 0.f;

    // The ECS walk the dirty-tracked object mirror exists to delete; its own
    // profile slice is the before/after measurement for it.
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
            anyMesh = mesh; // any mesh identifies the shared arena's vertex/index buffers (single arena)
            const int32_t named = lodSelector != nullptr ? lodSelector->NamedLevelFor(entity) : 0;
            const uint32_t level = named >= 0     ? static_cast<uint32_t>(named)
                                   : canMeasure ? Assisi::Render::kMeasureLod
                                                : 0u;
            builder.AddInstance(mesh, transform.worldMatrix,
                                std::span<const Assisi::Render::Material *const>(meshRenderer.materials.data(),
                                                                                 meshRenderer.materials.size()),
                                level);
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
        ASSISI_PROFILE_GPU_PASS(params.frame.commandList, "cull-dispatch");
        Assisi::Render::CullView view{.frustumPlanes = frustum.Planes()};
        if (lodSelector != nullptr)
        {
            view.cameraPosition = lodSelector->View().cameraPosition;
            view.tanHalfFovY = lodSelector->View().tanHalfFovY;
            view.lodBias = lodSelector->Settings().bias;
        }
        params.culler->Cull(params.frame.commandList, view, tables, params.frustumCulling);
    }

    Assisi::Render::MeshPass::IndirectDrawInputs inputs;
    inputs.instanceBuffer = params.culler->InstanceBuffer();
    inputs.indirectBuffer = params.culler->IndirectBuffer();
    inputs.vertexBuffer = anyMesh->VertexBuffer();
    inputs.indexBuffer = anyMesh->IndexBuffer();
    std::ranges::copy(params.culler->CommandCounts(), std::begin(inputs.commandCounts));

    Assisi::Render::MeshPass::SubmitStats submitStats;
    {
        ASSISI_PROFILE_GPU_SCOPE(params.frame.commandList, "submit-indirect");
        const Assisi::Render::GpuPassTimerScope drawTimer{DrawSceneTimerName(params.stage)};
        submitStats = params.meshPass.SubmitIndirect(params.frame, inputs, params.stage);
    }

    // Survivor tallies read back from the GPU (a few frames stale). Identical
    // (mesh,submesh) instances coalesce into one instanced draw, so `batches` is
    // the live batch count (falls well below drawnItems).
    stats.drawnItems = params.culler->SurvivorInstanceCount();
    stats.culledMeshes = params.culler->CulledObjectCount();
    stats.batches = params.culler->SurvivorBatchCount();
    stats.drawCalls = submitStats.drawCalls;
    static_assert(Assisi::Render::kCullLodBuckets == kMaxReportedLods, "the GPU tally fills DrawStats::lodInstances");
    std::ranges::copy(params.culler->SurvivorLodCounts(), stats.lodInstances.begin());
    return stats;
}
} // namespace

DrawStats DrawScene(const DrawSceneParams &params)
{
    // A label and a CPU scope, not a GPU timer: the GPU path times its cull
    // dispatch and its draw separately, and pass timers cannot nest. Each path
    // opens the `draw-scene` timer around its draw alone, which on the CPU path
    // is everything this function records.
    ASSISI_PROFILE_GPU_SCOPE(params.frame.commandList, "draw-scene");

    Assisi::ECS::Scene &scene = params.scene;
    const Assisi::Render::MeshPass &meshPass = params.meshPass;
    const glm::mat4 &view = params.view;

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

    DrawStats stats;
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

            const Assisi::Geometry::BoundingSphere worldSphere =
                Assisi::Geometry::TransformedBoundingSphere(mesh->LocalBounds(), transform.worldMatrix);

            if (params.frustumCulling)
            {
                // Two-level whole-mesh cull: the cheap sphere reject (one point
                // transform + a scalar) throws out most off-screen meshes, then the
                // tighter AABB refine catches ones the sphere's isotropic radius kept
                // but the box excludes. Both are conservative — nothing visible is
                // ever culled; the AABB just wastes fewer draws on flat/elongated meshes.
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
            const glm::vec3 centerWorld = glm::vec3(transform.worldMatrix * glm::vec4(mesh->LocalBounds().center, 1.f));
            const float viewDistance = -(view * glm::vec4(centerWorld, 1.f)).z; // camera looks down -Z
            const uint16_t depth = Assisi::Render::QuantizeDepthFrontToBack(viewDistance, params.nearZ, params.farZ);

            // The same call the shadow gathers make for this instance, so the
            // shadow is cast by the silhouette on screen.
            const uint32_t level =
                params.lodSelector != nullptr
                    ? params.lodSelector->Select(entity, mesh->Lods(), worldSphere)
                    : 0u;

            const std::vector<Assisi::Geometry::SubMesh> &subMeshes = mesh->SubMeshes();
            const Assisi::Geometry::LodRange lod = LodRangeFor(*mesh, level);
            const std::size_t itemsBefore = items.size();

            for (uint32_t i = 0; i < lod.SubMeshCount; ++i)
            {
                const uint32_t submeshIndex = lod.FirstSubMesh + i;
                const Assisi::Geometry::SubMesh &subMesh = subMeshes[submeshIndex];
                const Assisi::Render::Material *material = subMesh.MaterialSlot < meshRenderer.materials.size()
                                                               ? meshRenderer.materials[subMesh.MaterialSlot]
                                                               : nullptr;
                if (material == nullptr)
                {
                    continue; // no material resolved for this slot — skip rather than guess
                }

                const uint64_t sortKey =
                    Assisi::Render::MakeOpaqueSortKey(material->Pipeline(), material->Id(), mesh->Id(), depth);
                items.push_back(Assisi::Render::DrawItem{.sortKey = sortKey,
                                                         .mesh = mesh,
                                                         .submeshIndex = submeshIndex,
                                                         .castsShadows = meshRenderer.castsShadows,
                                                         .material = material,
                                                         .model = transform.worldMatrix});
            }

            // After the loop: an instance whose every slot went unresolved drew
            // nothing, at no level.
            if (items.size() > itemsBefore)
            {
                ++stats.lodInstances[std::min<std::size_t>(level, kMaxReportedLods - 1u)];
            }
        }
    }

    // Material/mesh-major, front-to-back within a run. Off = query order (A/B: the
    // image is identical, the bind counts differ). std::sort is not stable, but the
    // sort key is a total order over what matters, so stability is irrelevant.
    if (params.sortDraws)
    {
        // Its own slice, not folded into draw-extract: `sortDraws` turns exactly
        // this half off, so the A/B reads as a slice appearing or not.
        ASSISI_PROFILE_SCOPE("draw-sort");
        std::sort(items.begin(), items.end(),
                  [](const Assisi::Render::DrawItem &lhs, const Assisi::Render::DrawItem &rhs)
                  { return lhs.sortKey < rhs.sortKey; });
    }

    Assisi::Render::MeshPass::SubmitStats submitStats;
    {
        ASSISI_PROFILE_GPU_SCOPE(params.frame.commandList, "draw-submit");
        const Assisi::Render::GpuPassTimerScope drawTimer{DrawSceneTimerName(params.stage)};
        submitStats = meshPass.Submit(params.frame, items, params.stage);
    }

    stats.drawnItems = submitStats.instances;
    stats.batches = submitStats.batches;
    stats.drawCalls = submitStats.drawCalls;
    return stats;
}

} // namespace Assisi::Runtime
