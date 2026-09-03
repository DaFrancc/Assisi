/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <algorithm>
#include <limits>
#include <span>
#include <vector>

#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Render/DrawItem.hpp>
#include <Assisi/Render/GpuMarker.hpp>
#include <Assisi/Render/Frustum.hpp>
#include <Assisi/Render/MeshCuller.hpp>
#include <Assisi/Render/ShadowCascades.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Renderer.hpp>

namespace Assisi::Runtime
{
namespace
{
/// @brief Whether @p lhs draws through a pipeline class that sorts before
/// @p rhs's, or the same class and an earlier geometry.
///
/// The whole pipeline class, not the alpha test alone: the cull mode is pipeline
/// state too, so a run crossing from single- to double-sided could not coalesce
/// however identical its geometry. The key is built from the mesh id rather than
/// its address, so the ordering is the same every frame whatever the allocator
/// did.
bool ShadowCasterOrderBefore(const Assisi::Render::ShadowCaster &lhs, const Assisi::Render::ShadowCaster &rhs)
{
    const Assisi::Render::MeshPipeline lhsClass =
        Assisi::Render::MeshPipelineFor(lhs.alphaMasked, lhs.doubleSided);
    const Assisi::Render::MeshPipeline rhsClass =
        Assisi::Render::MeshPipelineFor(rhs.alphaMasked, rhs.doubleSided);
    if (lhsClass != rhsClass)
    {
        return lhsClass < rhsClass;
    }
    return lhs.geometryKey < rhs.geometryKey;
}

/// @brief Order @p casters pipeline-class first, geometry-major within a class.
///
/// A run of identical (mesh, submesh) entries then coalesces into one instanced
/// draw in every view that keeps them. The class leads because it is the
/// pipeline: a run crossing from one class into another could not coalesce
/// whatever its geometry, so sorting on it is what keeps the runs whole.
void SortShadowCasters(std::vector<Assisi::Render::ShadowCaster> &casters)
{
    ASSISI_PROFILE_SCOPE("shadow-sort");
    std::sort(casters.begin(), casters.end(), ShadowCasterOrderBefore);
}

/// @brief The same order, produced as a permutation of @p order rather than by
/// moving @p casters.
///
/// For a caller that has a second array keyed by a caster's original position —
/// the local gather's light membership — and so needs to know where each one
/// went rather than only that they are sorted.
void SortShadowCasterOrder(const std::vector<Assisi::Render::ShadowCaster> &casters,
                           std::vector<std::uint32_t> &order)
{
    ASSISI_PROFILE_SCOPE("shadow-sort");
    std::sort(order.begin(), order.end(), [&casters](std::uint32_t lhs, std::uint32_t rhs)
              { return ShadowCasterOrderBefore(casters[lhs], casters[rhs]); });
}

/// @brief Append one caster per LOD0 submesh of @p mesh to @p out.
///
/// LOD0, matching the draw path: a shadow cast by a different silhouette than
/// the one on screen is worse than a slightly expensive one.
///
/// Shared by the sun's gather and the local lights', because a caster is the
/// same thing to both — only what decides its @p viewMask differs.
void EmitShadowCasters(const Assisi::Render::MeshBuffer &mesh, const MeshRenderer &meshRenderer,
                       const Transform &transform, const Assisi::Geometry::BoundingSphere &worldSphere,
                       std::uint32_t viewMask, std::vector<Assisi::Render::ShadowCaster> &out)
{
    const std::vector<Assisi::Geometry::SubMesh> &subMeshes = mesh.SubMeshes();
    const std::vector<Assisi::Geometry::LodRange> &lods = mesh.Lods();
    const Assisi::Geometry::LodRange lod0 =
        !lods.empty() ? lods.front() : Assisi::Geometry::LodRange{0, static_cast<uint32_t>(subMeshes.size())};

    for (uint32_t i = 0; i < lod0.SubMeshCount; ++i)
    {
        const uint32_t submeshIndex = lod0.FirstSubMesh + i;
        const Assisi::Geometry::SubMesh &subMesh = subMeshes[submeshIndex];
        // An unresolved slot casts opaquely rather than not at all: row 0 is
        // never read, because the alpha test that would read it is off.
        const Assisi::Render::Material *material =
            subMesh.MaterialSlot < meshRenderer.materials.size() ? meshRenderer.materials[subMesh.MaterialSlot]
                                                                 : nullptr;
        const bool alphaMasked = material != nullptr && material->IsAlphaMasked();
        // The same flag the mesh pass reads. A double-sided caster is a surface
        // with no interior, so the depth pass must record both of its faces; a
        // single-sided one is a closed shell whose back faces are its inside,
        // and culling them is free and correct.
        const bool doubleSided = material != nullptr && material->IsDoubleSided();
        out.push_back(Assisi::Render::ShadowCaster{
                    .geometryKey = Assisi::Render::ShadowGeometryKey(mesh.Id(), submeshIndex),
                    .vertexBuffer = mesh.VertexBuffer(),
                    .indexBuffer = mesh.IndexBuffer(),
                    .indexCount = subMesh.IndexCount,
                    .startIndexLocation = mesh.IndexBase() + subMesh.IndexOffset,
                    .baseVertexLocation = static_cast<int32_t>(mesh.VertexBase()),
                    .model = transform.worldMatrix,
                    .worldSphere = worldSphere,
                    .viewMask = viewMask,
                    .alphaMasked = alphaMasked,
                    .doubleSided = doubleSided,
                    .materialIndex = alphaMasked ? material->Id() : 0u});
    }
}

// GPU-driven cull path (stage F1). Instead of culling + emitting + sorting draws
// on the CPU, gather every mesh entity into the culler's host tables (deduping
// meshes into a descriptor table), let the compute pass frustum-cull and build the
// indirect commands + per-instance records on the GPU, then issue one
// drawIndexedIndirectCount. The CPU still iterates the ECS to build the tables
// (the cull *math* is what moved to the GPU); a dirty-tracked ECS→GPU mirror that
// removes this gather too is stage F2. @p frustum's planes drive the GPU test.
DrawStats DrawSceneGpu(const DrawSceneParams &params, const Assisi::Render::Frustum &frustum)
{
    Assisi::ECS::Scene &scene   = params.scene;
    Assisi::Render::CullTableBuilder &builder = *params.cullBuilder;

    DrawStats stats;

    // The ECS walk stage F2 exists to delete; its own profile slice is the
    // before/after measurement for the dirty-tracked mirror.
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
        ASSISI_PROFILE_GPU_PASS(params.frame.commandList, "cull-dispatch");
        params.culler->Cull(params.frame.commandList, frustum.Planes(), tables, params.frustumCulling);
    }

    Assisi::Render::MeshPass::IndirectDrawInputs inputs;
    inputs.instanceBuffer = params.culler->InstanceBuffer();
    inputs.indirectBuffer = params.culler->IndirectBuffer();
    inputs.vertexBuffer   = anyMesh->VertexBuffer();
    inputs.indexBuffer    = anyMesh->IndexBuffer();
    std::ranges::copy(params.culler->CommandCounts(), std::begin(inputs.commandCounts));

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
    ASSISI_PROFILE_GPU_PASS(params.frame.commandList, "draw-scene");

    Assisi::ECS::Scene &scene    = params.scene;
    const Assisi::Render::MeshPass &meshPass = params.meshPass;
    const glm::mat4 &view     = params.view;

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
            const float viewDistance = -(view * glm::vec4(centerWorld, 1.f)).z;    // camera looks down -Z
            const uint16_t depth = Assisi::Render::QuantizeDepthFrontToBack(viewDistance, params.nearZ, params.farZ);

            // LOD0 only for now (screen-size LOD selection is a later stage; the seam is
            // ready for it). EnsureSubMeshTables guarantees at least one LOD/submesh.
            const std::vector<Assisi::Geometry::SubMesh> &subMeshes = mesh->SubMeshes();
            const std::vector<Assisi::Geometry::LodRange> &lods      = mesh->Lods();
            const Assisi::Geometry::LodRange lod0 =
                !lods.empty() ? lods.front()
                              : Assisi::Geometry::LodRange{0, static_cast<uint32_t>(subMeshes.size())};

            for (uint32_t i = 0; i < lod0.SubMeshCount; ++i)
            {
                const uint32_t submeshIndex = lod0.FirstSubMesh + i;
                const Assisi::Geometry::SubMesh &subMesh      = subMeshes[submeshIndex];
                const Assisi::Render::Material *material =
                    subMesh.MaterialSlot < meshRenderer.materials.size() ? meshRenderer.materials[subMesh.MaterialSlot]
                                                                            : nullptr;
                if (material == nullptr)
                {
                    continue; // no material resolved for this slot — skip rather than guess
                }

                const uint64_t sortKey =
                    Assisi::Render::MakeOpaqueSortKey(material->Pipeline(), material->Id(), mesh->Id(), depth);
                items.push_back(Assisi::Render::DrawItem{.sortKey      = sortKey,
                                                         .mesh         = mesh,
                                                         .submeshIndex = submeshIndex,
                                                         .castsShadows = meshRenderer.castsShadows,
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
        submitStats = meshPass.Submit(params.frame, items);
    }

    stats.drawnItems = submitStats.instances;
    stats.batches    = submitStats.batches;
    stats.drawCalls  = submitStats.drawCalls;
    return stats;
}

void GatherShadowCasters(Assisi::ECS::Scene &scene, const glm::vec3 &lightDirection,
                         std::span<const Assisi::Geometry::BoundingSphere> viewVolumes, ShadowCasterGather &out)
{
    ASSISI_PROFILE_SCOPE("shadow-gather");

    out.casters.clear();
    out.nearAlongLight.reset();
    out.culledEntities = 0;

    float nearAlongLight = std::numeric_limits<float>::max();

    for (auto [entity, transform, meshRenderer] : scene.Query<Transform, MeshRenderer>())
    {
        const Assisi::Render::MeshBuffer *mesh = meshRenderer.meshBuffer;
        if (mesh == nullptr || !meshRenderer.castsShadows)
        {
            continue;
        }

        const Assisi::Geometry::BoundingSphere worldSphere =
            Assisi::Geometry::TransformedBoundingSphere(mesh->LocalBounds(), transform.worldMatrix);

        // Swept down-light against each view's volume, once, here. The same
        // rejection made per view is one the frustum test would have reached
        // only after walking this caster again for every view and every pipeline
        // class; the mask is what turns that product into a classification, and
        // the sweep is what makes it without cutting off the casters up-light
        // that the views deliberately keep.
        const std::uint32_t viewMask =
            Assisi::Render::ShadowCasterViewMask(worldSphere, viewVolumes, lightDirection);
        if (viewMask == 0u)
        {
            ++out.culledEntities;
            continue;
        }

        // How far up-light this caster reaches. Every cascade's near plane is
        // pulled back to the smallest of these, which is what stops geometry
        // behind the camera from being clipped out of the map it shadows into.
        nearAlongLight = std::min(nearAlongLight, glm::dot(worldSphere.center, lightDirection) - worldSphere.radius);

        EmitShadowCasters(*mesh, meshRenderer, transform, worldSphere, viewMask, out.casters);
    }

    if (out.casters.empty())
    {
        return;
    }
    out.nearAlongLight = nearAlongLight;
    SortShadowCasters(out.casters);
}

void GatherLocalShadowCasters(Assisi::ECS::Scene &scene,
                              std::span<const Assisi::Geometry::BoundingSphere> lightVolumes,
                              ShadowCasterGather &out, Assisi::Render::LocalShadowCasterIndex &index)
{
    ASSISI_PROFILE_SCOPE("local-shadow-gather");

    out.casters.clear();
    out.nearAlongLight.reset(); // meaningless for a light with a position
    out.culledEntities = 0;
    index.Clear();

    if (lightVolumes.empty())
    {
        index.start.assign(1, 0u);
        return;
    }

    // Which lights each caster reaches, accumulated while the casters are
    // gathered and inverted into the per-light rows afterwards. Inverted rather
    // than filled directly because the rows index the *sorted* caster span, and
    // the sort has not happened yet.
    std::vector<std::uint32_t> reached;
    std::vector<std::uint32_t> reachedStart;
    reachedStart.push_back(0);

    for (auto [entity, transform, meshRenderer] : scene.Query<Transform, MeshRenderer>())
    {
        const Assisi::Render::MeshBuffer *mesh = meshRenderer.meshBuffer;
        if (mesh == nullptr || !meshRenderer.castsShadows)
        {
            continue;
        }

        const Assisi::Geometry::BoundingSphere worldSphere =
            Assisi::Geometry::TransformedBoundingSphere(mesh->LocalBounds(), transform.worldMatrix);

        // A plain sphere-sphere test, and no sweep: a local light is a point
        // with a range, so what can occlude for it is what stands inside its
        // reach. The sun's gather sweeps down-light because the sun has no
        // position to be inside of.
        const std::size_t firstReach = reached.size();
        for (std::uint32_t light = 0; light < lightVolumes.size(); ++light)
        {
            const glm::vec3 separation = worldSphere.center - lightVolumes[light].center;
            const float reach = worldSphere.radius + lightVolumes[light].radius;
            if (glm::dot(separation, separation) <= reach * reach)
            {
                reached.push_back(light);
            }
        }
        if (reached.size() == firstReach)
        {
            ++out.culledEntities;
            continue;
        }

        // One row per emitted caster, not per entity: a mesh's submeshes are
        // separate casters and each needs its own row, and they all reach
        // exactly the lights the entity's sphere did.
        const std::size_t before = out.casters.size();
        EmitShadowCasters(*mesh, meshRenderer, transform, worldSphere, ~0u, out.casters);
        for (std::size_t emitted = before; emitted < out.casters.size(); ++emitted)
        {
            if (emitted != before)
            {
                reached.insert(reached.end(), reached.begin() + static_cast<std::ptrdiff_t>(firstReach),
                               reached.begin() + static_cast<std::ptrdiff_t>(reached.size()));
            }
            reachedStart.push_back(static_cast<std::uint32_t>(reached.size()));
        }
    }

    index.start.assign(lightVolumes.size() + 1u, 0u);
    if (out.casters.empty())
    {
        return;
    }

    // The sort moves casters, and the rows name them by position — so the
    // membership is carried through it rather than read after it. Sorting an
    // index and permuting alongside would be the same work with a second array
    // to keep in step.
    std::vector<std::uint32_t> order(out.casters.size());
    for (std::uint32_t i = 0; i < order.size(); ++i)
    {
        order[i] = i;
    }
    SortShadowCasterOrder(out.casters, order);

    // Count first, then fill: a row's length is known before anything is placed,
    // so the whole index is two linear passes and one allocation.
    for (const std::uint32_t caster : order)
    {
        for (std::uint32_t entry = reachedStart[caster]; entry < reachedStart[caster + 1u]; ++entry)
        {
            ++index.start[reached[entry] + 1u];
        }
    }
    for (std::size_t light = 1; light < index.start.size(); ++light)
    {
        index.start[light] += index.start[light - 1u];
    }

    std::vector<std::uint32_t> cursor(index.start.begin(), index.start.end() - 1);
    index.caster.resize(index.start.back());
    std::vector<Assisi::Render::ShadowCaster> sorted;
    sorted.reserve(out.casters.size());
    for (std::uint32_t sortedIndex = 0; sortedIndex < order.size(); ++sortedIndex)
    {
        const std::uint32_t original = order[sortedIndex];
        sorted.push_back(out.casters[original]);
        for (std::uint32_t entry = reachedStart[original]; entry < reachedStart[original + 1u]; ++entry)
        {
            index.caster[cursor[reached[entry]]++] = sortedIndex;
        }
    }
    out.casters.swap(sorted);
}

} // namespace Assisi::Runtime
