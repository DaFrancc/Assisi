/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <algorithm>
#include <bit>
#include <limits>
#include <span>
#include <vector>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Render/DrawItem.hpp>
#include <Assisi/Render/Frustum.hpp>
#include <Assisi/Render/GpuMarker.hpp>
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
    const Assisi::Render::MeshPipeline lhsClass = Assisi::Render::MeshPipelineFor(lhs.alphaMasked, lhs.doubleSided);
    const Assisi::Render::MeshPipeline rhsClass = Assisi::Render::MeshPipelineFor(rhs.alphaMasked, rhs.doubleSided);
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
void SortShadowCasterOrder(const std::vector<Assisi::Render::ShadowCaster> &casters, std::vector<std::uint32_t> &order)
{
    ASSISI_PROFILE_SCOPE("shadow-sort");
    std::sort(order.begin(), order.end(), [&casters](std::uint32_t lhs, std::uint32_t rhs)
              { return ShadowCasterOrderBefore(casters[lhs], casters[rhs]); });
}

/// @brief The submesh run @p mesh draws at level @p level.
///
/// No LOD table at all is the whole submesh list — what a factory primitive
/// looks like. A level past the end clamps, so a stale remembered level can
/// never index off the table.
Assisi::Geometry::LodRange LodRangeFor(const Assisi::Render::MeshBuffer &mesh, uint32_t level)
{
    const std::vector<Assisi::Geometry::LodRange> &lods = mesh.Lods();
    if (lods.empty())
    {
        return Assisi::Geometry::LodRange{0, static_cast<uint32_t>(mesh.SubMeshes().size())};
    }
    return lods[std::min<std::size_t>(level, lods.size() - 1u)];
}

/// @brief One entity's contribution to a shadow map, as it comes off one query
///        row: the geometry, its material slots, its pose, and its world bounds.
struct ShadowCasterSource
{
    const Assisi::Render::MeshBuffer &mesh;
    const MeshRenderer &meshRenderer;
    const Transform &transform;
    Assisi::Geometry::BoundingSphere worldSphere;
};

/// @brief Append one caster per submesh of @p source's level @p level to @p out.
///
/// Shared by the sun's gather and the local lights' — only what decides a
/// caster's @p viewMask and @p level differs between them.
void EmitShadowCasters(const ShadowCasterSource &source, std::uint32_t viewMask, uint32_t level,
                       Assisi::Render::ShadowCasterMotion motion,
                       std::vector<Assisi::Render::ShadowCaster> &out)
{
    const Assisi::Render::MeshBuffer &mesh = source.mesh;
    const std::vector<Assisi::Geometry::SubMesh> &subMeshes = mesh.SubMeshes();
    const Assisi::Geometry::LodRange lod = LodRangeFor(mesh, level);

    for (uint32_t i = 0; i < lod.SubMeshCount; ++i)
    {
        const uint32_t submeshIndex = lod.FirstSubMesh + i;
        const Assisi::Geometry::SubMesh &subMesh = subMeshes[submeshIndex];
        // An unresolved slot casts opaquely rather than not at all: row 0 is
        // never read, because the alpha test that would read it is off.
        const Assisi::Render::Material *material = subMesh.MaterialSlot < source.meshRenderer.materials.size()
                                                       ? source.meshRenderer.materials[subMesh.MaterialSlot]
                                                       : nullptr;
        const bool alphaMasked = material != nullptr && material->IsAlphaMasked();
        // The same flag the mesh pass reads. A double-sided caster is a surface
        // with no interior, so the depth pass must record both of its faces; a
        // single-sided one is a closed shell whose back faces are its inside,
        // and culling them is free and correct.
        const bool doubleSided = material != nullptr && material->IsDoubleSided();
        out.push_back(
            Assisi::Render::ShadowCaster{.geometryKey = Assisi::Render::ShadowGeometryKey(mesh.Id(), submeshIndex),
                                         .vertexBuffer = mesh.VertexBuffer(),
                                         .indexBuffer = mesh.IndexBuffer(),
                                         .indexCount = subMesh.IndexCount,
                                         .startIndexLocation = mesh.IndexBase() + subMesh.IndexOffset,
                                         .baseVertexLocation = static_cast<int32_t>(mesh.VertexBase()),
                                         .model = source.transform.worldMatrix,
                                         .worldSphere = source.worldSphere,
                                         .viewMask = viewMask,
                                         .alphaMasked = alphaMasked,
                                         .doubleSided = doubleSided,
                                         .motion = motion,
                                         .materialIndex = alphaMasked ? material->Id() : 0u});
    }
}

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
        const Assisi::Render::GpuPassTimerScope drawTimer{"draw-scene"};
        submitStats = params.meshPass.SubmitIndirect(params.frame, inputs);
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
        const Assisi::Render::GpuPassTimerScope drawTimer{"draw-scene"};
        submitStats = meshPass.Submit(params.frame, items);
    }

    stats.drawnItems = submitStats.instances;
    stats.batches = submitStats.batches;
    stats.drawCalls = submitStats.drawCalls;
    return stats;
}

void GatherShadowCasters(Assisi::ECS::Scene &scene, const glm::vec3 &lightDirection,
                         std::span<const Assisi::Geometry::BoundingSphere> viewVolumes, LodSelector *lodSelector,
                         ShadowCasterGather &out)
{
    ASSISI_PROFILE_SCOPE("shadow-gather");

    out.casters.clear();
    out.nearAlongLight.reset();
    out.culledEntities = 0;
    out.coarserViews = 0;

    // The selector's widths name views by the same bits these volumes do; a
    // count that disagrees would measure casters against another view's texels.
    ASSISI_ASSERT(lodSelector == nullptr || lodSelector->ShadowViewCount() == viewVolumes.size(),
                  "LodSelector::SetShadowViews must describe the views being gathered for");

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
        const std::uint32_t viewMask = Assisi::Render::ShadowCasterViewMask(worldSphere, viewVolumes, lightDirection);
        if (viewMask == 0u)
        {
            ++out.culledEntities;
            continue;
        }

        // How far up-light this caster reaches. Every cascade's near plane is
        // pulled back to the smallest of these, which is what stops geometry
        // behind the camera from being clipped out of the map it shadows into.
        nearAlongLight = std::min(nearAlongLight, glm::dot(worldSphere.center, lightDirection) - worldSphere.radius);

        // The sun redraws every cascade every frame, so nothing here is ever
        // held back from a kept layer: every caster is drawn, every time.
        const ShadowCasterSource source{*mesh, meshRenderer, transform, worldSphere};
        const uint32_t level = lodSelector != nullptr ? lodSelector->Select(entity, mesh->Lods(), worldSphere) : 0u;
        const std::uint32_t coarser =
            lodSelector != nullptr ? lodSelector->CoarserShadowViews(entity, mesh->Lods(), worldSphere, level) & viewMask
                                   : 0u;

        // At most two copies, each masked to the views that draw it, so every
        // view's list still holds the entity exactly once.
        if ((viewMask & ~coarser) != 0u)
        {
            EmitShadowCasters(source, viewMask & ~coarser, level, Assisi::Render::ShadowCasterMotion::Still,
                              out.casters);
        }
        if (coarser != 0u)
        {
            EmitShadowCasters(source, coarser, level + 1u, Assisi::Render::ShadowCasterMotion::Still, out.casters);
            out.coarserViews += static_cast<std::uint32_t>(std::popcount(coarser));
        }
    }

    if (out.casters.empty())
    {
        return;
    }
    out.nearAlongLight = nearAlongLight;
    SortShadowCasters(out.casters);
}

void GatherShadowMovers(Assisi::ECS::Scene &scene, std::span<const Assisi::ECS::Entity> changed,
                        std::vector<Assisi::Render::ShadowMover> &out)
{
    ASSISI_PROFILE_SCOPE("shadow-movers");

    out.clear();
    for (const Assisi::ECS::Entity entity : changed)
    {
        const MeshRenderer *meshRenderer = scene.Get<MeshRenderer>(entity);
        const Transform *transform = scene.Get<Transform>(entity);
        if (meshRenderer == nullptr || transform == nullptr || !meshRenderer->castsShadows ||
            meshRenderer->meshBuffer == nullptr)
        {
            continue; // it moved, but nothing it does reaches a shadow map
        }
        out.push_back(Assisi::Render::ShadowMover{
                ShadowCasterId(entity), Assisi::Geometry::TransformedBoundingSphere(meshRenderer->meshBuffer->LocalBounds(),
                                                                                    transform->worldMatrix)});
    }
}

void LocalShadowCasterGather::Reset(std::uint32_t lightCount)
{
    _casters.clear();
    _reachedLights.clear();
    _casterStart.assign(1, 0u);
    _lightCount = lightCount;
    _culledEntities = 0;

    _index.Clear();
    _index.start.assign(lightCount + 1u, 0u);
}

void LocalShadowCasterGather::Gather(Assisi::ECS::Scene &scene,
                                     std::span<const Assisi::Geometry::BoundingSphere> lightVolumes,
                                     Assisi::Render::ShadowCasterMobility &mobility, LodSelector *lodSelector)
{
    ASSISI_PROFILE_SCOPE("local-shadow-gather");

    Reset(static_cast<std::uint32_t>(lightVolumes.size()));
    if (lightVolumes.empty())
    {
        return;
    }

    // Recorded per caster rather than per light because the atlas's rows index
    // the *sorted* caster span, and the sort has not happened yet.
    std::vector<std::uint32_t> &reached = _reachedLights;

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
            ++_culledEntities;
            continue;
        }

        const std::uint64_t casterId = ShadowCasterId(entity);
        const Assisi::Render::ShadowCasterMotion motion = mobility.IsDynamic(casterId)
                                                              ? Assisi::Render::ShadowCasterMotion::Moving
                                                              : Assisi::Render::ShadowCasterMotion::Still;
        if (motion == Assisi::Render::ShadowCasterMotion::Still)
        {
            // A still caster stands where a kept layer holds it, by definition:
            // it has not been written since it was baked, or it would be moving.
            // Recorded here rather than at the bake because this is where the
            // sphere is already in hand, and a caster gathered but not baked is
            // still standing where the last bake put it.
            mobility.NoteBaked(Assisi::Render::ShadowMover{casterId, worldSphere});
        }

        // One row per emitted caster, not per entity: a mesh's submeshes are
        // separate casters and each needs its own row, and they all reach
        // exactly the lights the entity's sphere did.
        const std::size_t before = _casters.size();
        const ShadowCasterSource source{*mesh, meshRenderer, transform, worldSphere};
        const uint32_t level = lodSelector != nullptr ? lodSelector->Select(entity, mesh->Lods(), worldSphere) : 0u;
        EmitShadowCasters(source, ~0u, level, motion, _casters);
        for (std::size_t emitted = before; emitted < _casters.size(); ++emitted)
        {
            if (emitted != before)
            {
                reached.insert(reached.end(), reached.begin() + static_cast<std::ptrdiff_t>(firstReach),
                               reached.begin() + static_cast<std::ptrdiff_t>(reached.size()));
            }
            _casterStart.push_back(static_cast<std::uint32_t>(reached.size()));
        }
    }
}

void LocalShadowCasterGather::BuildIndex()
{
    ASSISI_PROFILE_SCOPE("local-shadow-index");

    _index.start.assign(_lightCount + 1u, 0u);
    if (_casters.empty())
    {
        _index.caster.clear();
        return;
    }

    // The sort moves casters, and the rows name them by position — so the
    // membership is carried through it rather than read after it. Sorting an
    // index and permuting alongside would be the same work with a second array
    // to keep in step.
    _order.resize(_casters.size());
    for (std::uint32_t i = 0; i < _order.size(); ++i)
    {
        _order[i] = i;
    }
    SortShadowCasterOrder(_casters, _order);

    // Count first, then fill: a row's length is known before anything is placed,
    // so the whole index is two linear passes and no growth.
    for (const std::uint32_t caster : _order)
    {
        for (std::uint32_t entry = _casterStart[caster]; entry < _casterStart[caster + 1u]; ++entry)
        {
            ++_index.start[_reachedLights[entry] + 1u];
        }
    }
    for (std::size_t light = 1; light < _index.start.size(); ++light)
    {
        _index.start[light] += _index.start[light - 1u];
    }

    _cursor.assign(_index.start.begin(), _index.start.end() - 1);
    _index.caster.resize(_index.start.back());
    _sorted.clear();
    _sorted.reserve(_casters.size());
    for (std::uint32_t sortedIndex = 0; sortedIndex < _order.size(); ++sortedIndex)
    {
        const std::uint32_t original = _order[sortedIndex];
        _sorted.push_back(_casters[original]);
        for (std::uint32_t entry = _casterStart[original]; entry < _casterStart[original + 1u]; ++entry)
        {
            _index.caster[_cursor[_reachedLights[entry]]++] = sortedIndex;
        }
    }
    _casters.swap(_sorted);
}

} // namespace Assisi::Runtime
