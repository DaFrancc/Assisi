/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ShadowCasterGather.hpp
/// @brief What goes into the shadow maps: the casters each frame draws into
/// the sun's cascades and the local-light atlas, and the movers both of them
/// are invalidated against.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/LocalShadowCache.hpp>
#include <Assisi/Render/LocalShadowPass.hpp>
#include <Assisi/Render/ShadowCascades.hpp>
#include <Assisi/Render/ShadowDepthRenderer.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/LodSelection.hpp>

namespace Assisi::Runtime
{

/// @brief The identity the shadow system remembers a caster by, across the
/// sun's cascades and the local atlas alike.
///
/// The entity's index and generation together, so a slot reused by a new entity
/// is a different caster rather than the old one having teleported. A caster
/// has to be recognised across frames in which it was not gathered at all,
/// which is why this is a durable handle rather than a position in a span.
[[nodiscard]] constexpr std::uint64_t ShadowCasterId(Assisi::ECS::Entity entity)
{
    return (static_cast<std::uint64_t>(entity.generation) << 32) | entity.index;
}

/// @brief The entity handle a ShadowCasterId was made from. A handle, not a
/// promise: the entity may have been destroyed since, so check it with
/// Scene::IsAlive before use.
[[nodiscard]] constexpr Assisi::ECS::Entity ShadowCasterEntity(std::uint64_t casterId)
{
    return Assisi::ECS::Entity{.index = static_cast<std::uint32_t>(casterId),
                               .generation = static_cast<std::uint32_t>(casterId >> 32u)};
}

/// @brief One frame's sun shadow casters.
///
/// Reused across frames by the caller — the vector's capacity survives a
/// re-gather, so a steady-state scene allocates nothing here.
struct ShadowCasterGather
{
    /// Sorted by pipeline class and by geometry within a class, which is what
    /// lets consecutive entries coalesce into one instanced draw in the shadow
    /// pass: a run crossing from one class into another could not coalesce
    /// whatever its geometry.
    std::vector<Assisi::Render::ShadowCaster> casters;

    /// Entities dropped for reaching no view's volume at all. Reported so the
    /// shadow distance's effect is a number rather than an impression: walking
    /// content past it should move this and nothing else.
    std::uint32_t culledEntities = 0;

    /// Entity-view pairs drawn one level coarser than the camera draws that
    /// entity. Zero with shadow LOD off, and in a scene with no LOD chains.
    std::uint32_t coarserViews = 0;
};

/// @brief Collects the shadow-casting submeshes the sun's views draw this frame.
///
/// A caster is any resolved mesh whose MeshRenderer has `castsShadows` set.
/// Each submesh's material is consulted for one thing only: whether it
/// alpha-tests, and which table row the test reads. A submesh with no material
/// resolved still casts, opaquely — a shadow is a property of the geometry, and
/// dropping it would be a worse answer than a solid one.
///
/// Nothing is frustum-culled here — that is per view, and the frustum is the
/// view's own. What happens here is the classification every view would
/// otherwise repeat: each view is one sphere containing its whole ortho box,
/// and a caster's sweep down-light against each of them is the mask the draw
/// list walks. A caster that reaches none of them belongs in no list at all,
/// and is dropped rather than gathered. That is where the sun's shadow distance
/// is spent.
///
/// Each view draws a caster at the level its own texels ask for, held to the
/// camera's level or one coarser (see LodSelector::CoarserShadowViews, whose
/// views must be these, in this order): an entity the views disagree about is
/// emitted once per level, each copy masked to the views that draw it. Null
/// selects LOD0 for everything.
///
/// The views are of two kinds (see Render::ShadowPass::Render): still layers
/// being rebaked, which take the casters that are not moving, and read slices
/// the movers are drawn over, which take the ones that are. Which a caster is
/// comes from the shared Render::ShadowCasterMobility, so the sun and the
/// local lights always agree about which layer holds it. A frame rebaking no
/// still layer walks the movers alone rather than the scene.
class SunShadowCasterGather
{
public:
    /// @brief This frame's views: @p stillViews still layers first, then the
    /// read slices movers are drawn into, one volume each, in the order they
    /// are drawn. A caster's view mask is numbered the same way.
    void SetViews(std::span<const Assisi::Geometry::BoundingSphere> volumes, std::uint32_t stillViews,
                  const glm::vec3 &lightDirection);

    /// @brief Gather for the views SetViews named, into Result(). Still
    /// casters gathered for a still layer are told to @p mobility as baked:
    /// that is the pose the layer now holds them at.
    void Gather(Assisi::ECS::Scene &scene, Assisi::Render::ShadowCasterMobility &mobility, LodSelector *lodSelector);

    /// @brief Nothing gathered: what a frame drawing nothing hands the pass.
    void Clear();

    [[nodiscard]] const ShadowCasterGather &Result() const { return _result; }

private:
    void AddCaster(Assisi::ECS::Entity entity, const Transform &transform, const MeshRenderer &meshRenderer,
                   Assisi::Render::ShadowCasterMobility &mobility, LodSelector *lodSelector);

    ShadowCasterGather _result;
    /// Room for every cascade twice: once as a still layer, once as a read slice.
    std::array<Assisi::Geometry::BoundingSphere, Assisi::Render::kMaxSunShadowViews> _volumes{};
    glm::vec3 _lightDirection{0.f, -1.f, 0.f};
    std::uint32_t _viewCount = 0;
    std::uint32_t _stillViews = 0;
};

/// @brief One frame's local-light shadow casters, the lights each one reaches,
/// and the per-light rows the atlas reads.
///
/// Owns its buffers and is refilled rather than rebuilt: a scene changes little
/// between frames, so a steady state reuses the capacity instead of freeing and
/// regrowing it.
class LocalShadowCasterGather
{
public:
    /// @brief Every shadow-casting submesh in the scene, and which of
    /// @p lightVolumes each one reaches. A caster reaching none is dropped.
    ///
    /// A plain sphere-sphere test, and no sweep: a local light is a point with a
    /// range, so what can occlude for it is what stands inside it. The sun's
    /// gather sweeps down-light because the sun has no position to be inside of.
    ///
    /// @p mobility decides which layer of a tile each caster belongs to, and is
    /// told where every still caster gathered for a rebake stands — the pose
    /// the still layer holds it at, and what a later demotion has to invalidate.
    /// @p lodSelector picks each caster's level, as the sun's gather and the draw
    /// path do; null gathers everything at LOD0.
    ///
    /// @p stillRequests, index-parallel to @p lightVolumes, says which lights
    /// draw still casters this frame (LocalShadowPass::StillCasterRequests).
    /// The others' rows hold only moving casters, and a frame where every
    /// light rests walks only the movers instead of the scene. Empty, or any
    /// other length, means every light wants everything.
    void Gather(Assisi::ECS::Scene &scene, std::span<const Assisi::Geometry::BoundingSphere> lightVolumes,
                std::span<const std::uint8_t> stillRequests, Assisi::Render::ShadowCasterMobility &mobility,
                LodSelector *lodSelector);

    /// @brief Invert the membership into the per-light rows, and sort the casters
    /// those rows name.
    ///
    /// The sort lives here because the rows name casters by position in the
    /// sorted span; splitting the two would mean carrying a permutation between
    /// them. Pipeline class first and geometry-major, so consecutive entries
    /// coalesce.
    void BuildIndex();

    /// @brief No casters, and an empty row for each of @p lightCount lights.
    ///
    /// A frame that skips the gather still needs every row: the composite walks
    /// one per served face whether or not it finds anything in it.
    void Reset(std::uint32_t lightCount);

    /// In gather order until BuildIndex sorts them.
    [[nodiscard]] std::span<const Assisi::Render::ShadowCaster> Casters() const { return _casters; }
    [[nodiscard]] const Assisi::Render::LocalShadowCasterIndex &Index() const { return _index; }
    [[nodiscard]] std::uint32_t CulledEntities() const { return _culledEntities; }

private:
    /// Test one entity against this gather's lights and emit its casters with
    /// the lights they reach.
    void AddCaster(Assisi::ECS::Entity entity, const Transform &transform, const MeshRenderer &meshRenderer,
                   Assisi::Render::ShadowCasterMobility &mobility, LodSelector *lodSelector);

    std::vector<Assisi::Render::ShadowCaster> _casters;

    // The gather in progress's lights, and which of them draw still casters
    // (empty: all of them). Views of the caller's spans, valid during Gather.
    std::span<const Assisi::Geometry::BoundingSphere> _lightVolumes;
    std::span<const std::uint8_t> _stillRequests;

    // Light indices, concatenated: `_casterStart[i]` to `_casterStart[i + 1]` is
    // caster i's row. `_index`'s rows are this, inverted.
    std::vector<std::uint32_t> _reachedLights;
    std::vector<std::uint32_t> _casterStart;

    Assisi::Render::LocalShadowCasterIndex _index;

    // Scratch, kept between frames for the capacity.
    std::vector<std::uint32_t> _order;
    std::vector<std::uint32_t> _cursor;
    std::vector<Assisi::Render::ShadowCaster> _sorted;

    std::uint32_t _lightCount = 0;
    std::uint32_t _culledEntities = 0;
};

/// @brief The shadow casters among @p changed, with where they now stand.
///
/// The invalidation input, and the reason it is cheap: @p changed comes from the
/// Transform pool's change-tick lane, so it names what moved rather than what
/// exists, and a frame in which nothing moved produces nothing here.
void GatherShadowMovers(Assisi::ECS::Scene &scene, std::span<const Assisi::ECS::Entity> changed,
                        std::vector<Assisi::Render::ShadowMover> &out);

} // namespace Assisi::Runtime
