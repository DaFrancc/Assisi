/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/ECS/Hierarchy.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/RenderOffset.hpp>
#include <Assisi/ECS/TransformPose.hpp>
#include <Assisi/ECS/WorldMatrix.hpp>

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <vector>

#if defined(__FMA__)
#include <immintrin.h>
#endif

namespace Assisi::ECS
{

namespace
{

/// translate(position) * rotate(rotation) * scale(scale), written out directly
/// rather than as three matrix products.
glm::mat4 ComposeMatrix(const glm::quat &rotation, const glm::vec3 &position, const glm::vec3 &scale)
{
    const float x = rotation.x;
    const float y = rotation.y;
    const float z = rotation.z;
    const float w = rotation.w;
    const float xx = x * x;
    const float yy = y * y;
    const float zz = z * z;
    const float xy = x * y;
    const float xz = x * z;
    const float yz = y * z;
    const float wx = w * x;
    const float wy = w * y;
    const float wz = w * z;

    glm::mat4 m(1.f);
    m[0] = glm::vec4(1.f - 2.f * (yy + zz), 2.f * (xy + wz), 2.f * (xz - wy), 0.f) * scale.x;
    m[1] = glm::vec4(2.f * (xy - wz), 1.f - 2.f * (xx + zz), 2.f * (yz + wx), 0.f) * scale.y;
    m[2] = glm::vec4(2.f * (xz + wy), 2.f * (yz - wx), 1.f - 2.f * (xx + yy), 0.f) * scale.z;
    m[3] = glm::vec4(position, 1.f);
    return m;
}

/// @p parent * @p local. With FMA each result column is one multiply and three
/// fused multiply-adds over the parent's columns, instead of sixteen scalar
/// products.
glm::mat4 MultiplyMatrices(const glm::mat4 &parent, const glm::mat4 &local)
{
#if defined(__FMA__)
    const __m128 p0 = _mm_loadu_ps(&parent[0][0]);
    const __m128 p1 = _mm_loadu_ps(&parent[1][0]);
    const __m128 p2 = _mm_loadu_ps(&parent[2][0]);
    const __m128 p3 = _mm_loadu_ps(&parent[3][0]);
    glm::mat4 out;
    for (glm::length_t column = 0; column < 4; ++column)
    {
        const glm::vec4 &l = local[column];
        __m128 sum = _mm_mul_ps(p3, _mm_set1_ps(l.w));
        sum = _mm_fmadd_ps(p2, _mm_set1_ps(l.z), sum);
        sum = _mm_fmadd_ps(p1, _mm_set1_ps(l.y), sum);
        sum = _mm_fmadd_ps(p0, _mm_set1_ps(l.x), sum);
        _mm_storeu_ps(&out[column][0], sum);
    }
    return out;
#else
    return parent * local;
#endif
}

/// Blends two rotations a fraction @p alpha of the way along the short arc,
/// linearly and then renormalized. Over one fixed step the angle is small, so
/// this follows slerp's path within rounding and needs no trigonometry.
glm::quat BlendRotation(const glm::quat &from, glm::quat to, float alpha)
{
    if (glm::dot(from, to) < 0.f)
    {
        to = -to;
    }
    return glm::normalize(from * (1.f - alpha) + to * alpha);
}

/// Turns @p world by @p offset about its own position, then moves it.
void ApplyRenderOffset(glm::mat4 &world, const RenderOffset &offset)
{
    const glm::mat3 turn = glm::mat3_cast(offset.rotation);
    for (glm::length_t column = 0; column < 3; ++column)
    {
        world[column] = glm::vec4(turn * glm::vec3(world[column]), 0.f);
    }
    world[3] += glm::vec4(offset.position, 0.f);
}

/// Per-entity scratch for one propagation pass, indexed by Entity::index. One
/// reused array rather than maps and sets, so nothing is hashed per entity. The
/// pass ids say which pass last touched a slot, so the array is never cleared.
struct PassState
{
    uint32_t dirtyPass = 0;    ///< Pass the entity was listed as dirty in.
    uint32_t resolvedPass = 0; ///< Pass the entity was resolved in.
    bool worldChanged = false; ///< Its world matrix was recomputed in resolvedPass.
    bool resolving = false;    ///< On the resolve stack, for cycle detection.
};

} // namespace

/// One PropagateTransforms call: what it reads, what it writes, and the
/// per-entity scratch it keeps.
struct Propagation
{
    Scene &scene;
    SparseSetLanes<Transform> &lanes;
    std::vector<PassState> &passState;

    /// Entities that may need a new world matrix, collected before any is
    /// resolved.
    std::vector<Entity> &dirty;

    /// Entities (index and generation packed) whose parent cycle was already
    /// reported, so a bad level logs it once rather than every frame.
    std::unordered_set<uint64_t> &reportedCycles;

    uint32_t pass;
    uint32_t resolved = 0;

    /// @p index's scratch, grown on demand. Growing invalidates earlier
    /// references, so never hold one across a call to Resolve.
    PassState &Slot(uint32_t index)
    {
        if (index >= passState.size())
        {
            passState.resize(index + 1);
        }
        return passState[index];
    }

    void MarkDirty(Entity e)
    {
        if (Slot(e.index).dirtyPass != pass)
        {
            Slot(e.index).dirtyPass = pass;
            dirty.push_back(e);
        }
    }

    /// Lists every entity whose drawn pose may differ from the matrix it holds.
    void Collect(uint64_t lastTick)
    {
        if (lanes._redrawAll)
        {
            lanes._redrawAll = false;
            for (auto [entity, transform] : scene.Query<Transform>())
            {
                (void)transform;
                MarkDirty(entity);
            }
            return;
        }

        if (scene.CurrentChangeTick() != lastTick)
        {
            std::vector<Entity> changed;
            scene.ChangedSince<Transform>(lastTick, changed);
            scene.ChangedSince<Parent>(lastTick, changed);
            // Removing Parent or RenderOffset stamps nothing; the removal log is
            // what says it happened. A log that no longer reaches back far enough
            // redraws everything.
            std::vector<Entity> unposed;
            const bool complete = scene.RemovedSince<Parent>(lastTick, changed) &&
                                  scene.RemovedSince<RenderOffset>(lastTick, changed) &&
                                  scene.RemovedSince<Transform>(lastTick, unposed);
            if (!complete)
            {
                lanes._redrawAll = true;
                Collect(lastTick);
                return;
            }
            for (const Entity entity : changed)
            {
                MarkDirty(entity);
            }
            DropUnposedMatrices(unposed);
        }

        for (const Entity entity : lanes._ended)
        {
            MarkDirty(entity);
        }
        if (lanes._alpha != lanes._drawnAlpha || lanes._step != lanes._drawnStep)
        {
            for (const SparseSetLanes<Transform>::BlendEntry &entry : lanes._current)
            {
                MarkDirty(entry.entity);
            }
        }
        for (auto [entity, offset] : scene.Query<RenderOffset>())
        {
            (void)offset;
            MarkDirty(entity);
        }
    }

    /// Removes the WorldMatrix of each of @p unposed that no longer has a
    /// Transform, so nothing draws it where it last stood.
    void DropUnposedMatrices(const std::vector<Entity> &unposed)
    {
        for (const Entity entity : unposed)
        {
            if (scene.IsAlive(entity) && !scene.Has<Transform>(entity) && scene.Has<WorldMatrix>(entity))
            {
                (void)scene.Remove<WorldMatrix>(entity);
            }
        }
    }

    /// The local matrix @p transform, at dense slot @p slot, is drawn with:
    /// blended between the last two fixed steps if it moved in the latest one.
    glm::mat4 DrawnLocalMatrix(uint32_t slot, const Transform &transform) const
    {
        const SparseSetLanes<Transform>::BlendMark &mark = lanes._marks[slot];
        if (mark.step != lanes._step)
        {
            return ComposeMatrix(transform.rotation, transform.position, transform.scale);
        }

        // Each field the step did not change is taken exactly, so an entity at
        // rest is drawn bit for bit where it is. Written again since the step,
        // the step's motion is blended and the later write applied in full.
        const SparseSetLanes<Transform>::BlendEntry &entry = lanes._current[mark.entry];
        const float alpha = lanes._alpha;
        const glm::vec3 &prevPosition = entry.prevPosition;
        const glm::vec3 &prevScale = entry.prevScale;
        const glm::vec3 endPosition = entry.hasEnd ? entry.endPosition : transform.position;
        const glm::vec3 endScale = entry.hasEnd ? entry.endScale : transform.scale;
        const glm::quat endRotation = entry.hasEnd ? entry.endRotation : transform.rotation;

        glm::vec3 position = transform.position;
        if (prevPosition != endPosition)
        {
            position = glm::mix(prevPosition, endPosition, alpha) + (transform.position - endPosition);
        }
        glm::vec3 scale = transform.scale;
        if (prevScale != endScale)
        {
            scale = glm::mix(prevScale, endScale, alpha) + (transform.scale - endScale);
        }
        glm::quat rotation = transform.rotation;
        if (entry.prevRotation != endRotation)
        {
            rotation = BlendRotation(entry.prevRotation, endRotation, alpha);
            if (entry.hasEnd)
            {
                rotation = glm::normalize(transform.rotation * glm::conjugate(endRotation) * rotation);
            }
        }
        return ComposeMatrix(rotation, position, scale);
    }

    /// Resolves @p e's world matrix, recomputing it when @p e is dirty or its
    /// parent's matrix changed this pass. Parents are resolved first, by
    /// recursion.
    ///
    /// @return whether @p e's world matrix changed this pass, so a child knows
    /// it must recompute too.
    bool Resolve(Entity e)
    {
        const uint32_t slot = scene.DenseIndexOf<Transform>(e);
        WorldMatrix *world = scene.Get<WorldMatrix>(e);
        if (slot == SparseSet<Transform>::Invalid || world == nullptr)
        {
            return false; // gone, or a parent without a Transform: identity
        }
        if (Slot(e.index).resolvedPass == pass)
        {
            return Slot(e.index).worldChanged; // already resolved this pass (shared parent)
        }
        Slot(e.index).resolvedPass = pass;
        Slot(e.index).resolving = true;

        bool parentChanged = false;
        const glm::mat4 *parentWorld = ResolveParent(e, parentChanged);

        const bool worldChanged = Slot(e.index).dirtyPass == pass || parentChanged;
        if (worldChanged)
        {
            const glm::mat4 local = DrawnLocalMatrix(slot, *scene.Get<Transform>(e));
            world->matrix = parentWorld != nullptr ? MultiplyMatrices(*parentWorld, local) : local;
            if (const RenderOffset *offset = scene.Get<RenderOffset>(e); offset != nullptr)
            {
                ApplyRenderOffset(world->matrix, *offset);
            }
            ++resolved;
        }

        Slot(e.index).worldChanged = worldChanged;
        Slot(e.index).resolving = false;
        return worldChanged;
    }

    /// Resolves @p e's parent and returns its world matrix, or null when @p e is
    /// a root. Sets @p parentChanged when that matrix changed this pass.
    const glm::mat4 *ResolveParent(Entity e, bool &parentChanged)
    {
        const Parent *p = scene.Get<Parent>(e);
        if (p == nullptr || p->parent == NullEntity || !scene.Has<Transform>(p->parent) ||
            !scene.Has<WorldMatrix>(p->parent))
        {
            return nullptr;
        }

        // A hand-edited level can hold a parent loop (A->B->A). The parent is
        // already on the resolve stack, so recursing would not terminate: this
        // entity is treated as a root instead.
        if (Slot(p->parent.index).resolvedPass == pass && Slot(p->parent.index).resolving)
        {
            const uint64_t packed = (static_cast<uint64_t>(e.generation) << 32u) | e.index;
            if (reportedCycles.insert(packed).second)
            {
                Core::Log::Error("PropagateTransforms: parent cycle at entity index {} (gen {}); treating as root",
                                 e.index, e.generation);
            }
            return nullptr;
        }

        parentChanged = Resolve(p->parent);
        // Finalised by the recursion; no pool changes size during a pass.
        return &scene.Get<WorldMatrix>(p->parent)->matrix;
    }

    /// Resolves every child of an entity whose world matrix changed this pass,
    /// and their children in turn. There is no child index, so the Parent pool
    /// is scanned once per level of hierarchy that changed.
    void ResolveChildren()
    {
        bool progress = resolved > 0;
        while (progress)
        {
            progress = false;
            for (auto [child, parent] : scene.Query<Parent>())
            {
                if (parent.parent == NullEntity || Slot(child.index).resolvedPass == pass)
                {
                    continue;
                }
                const PassState &up = Slot(parent.parent.index);
                if (up.resolvedPass == pass && up.worldChanged && !up.resolving)
                {
                    (void)Resolve(child);
                    progress = true;
                }
            }
        }
    }
};

uint64_t PropagateTransforms(Scene &scene, uint64_t lastTick)
{
    SparseSetLanes<Transform> *lanes = scene.Lanes<Transform>();
    if (lanes == nullptr)
    {
        return scene.CurrentChangeTick();
    }

    // Reused across calls and grown to the largest entity index seen.
    // thread_local so propagation driven from another thread can't interleave
    // with this one.
    thread_local std::vector<PassState> passState;
    thread_local std::vector<Entity> dirty;
    thread_local std::unordered_set<uint64_t> reportedCycles;
    thread_local uint32_t passCounter = 0;

    dirty.clear();
    Propagation propagation{.scene = scene,
                            .lanes = *lanes,
                            .passState = passState,
                            .dirty = dirty,
                            .reportedCycles = reportedCycles,
                            .pass = ++passCounter};
    propagation.Collect(lastTick);
    for (std::size_t i = 0; i < dirty.size(); ++i)
    {
        (void)propagation.Resolve(dirty[i]);
    }
    propagation.ResolveChildren();

    lanes->_ended.clear();
    lanes->_drawnAlpha = lanes->_alpha;
    lanes->_drawnStep = lanes->_step;
    lanes->_lastResolved = propagation.resolved;

    // The tick a caller should pass as `lastTick` next frame: everything written
    // up to now has been accounted for.
    return scene.CurrentChangeTick();
}

uint32_t LastPropagationResolved(const Scene &scene)
{
    const SparseSetLanes<Transform> *lanes = scene.Lanes<Transform>();
    return lanes != nullptr ? lanes->_lastResolved : 0u;
}

FixedStepScope::FixedStepScope(Scene &scene) : _scene(scene)
{
    if (SparseSetLanes<Transform> *lanes = _scene.Lanes<Transform>(); lanes != nullptr)
    {
        lanes->BeginFixedStep();
    }
}

FixedStepScope::~FixedStepScope()
{
    if (SparseSetLanes<Transform> *lanes = _scene.Lanes<Transform>(); lanes != nullptr)
    {
        lanes->EndFixedStep();
    }
}

void SetBlendAlpha(Scene &scene, float alpha)
{
    if (SparseSetLanes<Transform> *lanes = scene.Lanes<Transform>(); lanes != nullptr)
    {
        lanes->_alpha = alpha;
    }
}

void SettleTransforms(Scene &scene)
{
    if (SparseSetLanes<Transform> *lanes = scene.Lanes<Transform>(); lanes != nullptr)
    {
        lanes->Advance();
    }
}

void SnapTransform(Scene &scene, Entity entity)
{
    SparseSetLanes<Transform> *lanes = scene.Lanes<Transform>();
    const uint32_t slot = scene.DenseIndexOf<Transform>(entity);
    if (lanes == nullptr || slot == SparseSet<Transform>::Invalid)
    {
        return;
    }
    const SparseSetLanes<Transform>::BlendMark &mark = lanes->_marks[slot];
    if (mark.step != lanes->_step)
    {
        return; // not blending
    }
    // The pose just written becomes where the blend starts: drawn there now,
    // and a later write this step blends from it.
    SparseSetLanes<Transform>::BlendEntry &entry = lanes->_current[mark.entry];
    const Transform &current = *scene.Get<Transform>(entity);
    entry.prevRotation = current.rotation;
    entry.prevPosition = current.position;
    entry.prevScale = current.scale;
    entry.hasEnd = false;
}

std::vector<Entity> GatherSubtree(Scene &scene, Entity root)
{
    std::vector<Entity> result{root};

    // Breadth-first: for each collected entity, sweep for entities whose Parent
    // points at it. `result` grows as we go and the index walk visits each new
    // entry, so a whole subtree of any depth is collected. No child index exists,
    // so this scans — acceptable at subtree-edit scale.
    for (std::size_t i = 0; i < result.size(); ++i)
    {
        const Entity current = result[i];
        scene.ForEachEntity(
            [&](Entity e)
            {
                const Parent *parent = scene.Get<Parent>(e);
                if (parent == nullptr || parent->parent != current)
                {
                    return;
                }
                if (std::find(result.begin(), result.end(), e) == result.end())
                {
                    result.push_back(e);
                }
            });
    }
    return result;
}

Transform WorldTransformOf(const Scene &scene, Entity entity)
{
    const Transform *local = scene.Get<Transform>(entity);
    if (local == nullptr)
    {
        return {};
    }

    Transform world = *local;

    // Bounded rather than walked to the root: a cycle in Parent is a corrupt
    // scene, and the answer to a corrupt chain is a wrong pose, not a hang. The
    // bound is generous enough that no real hierarchy reaches it.
    constexpr uint32_t kMaxDepth = 256;
    Entity current = entity;
    for (uint32_t depth = 0; depth < kMaxDepth; ++depth)
    {
        const Parent *parent = scene.Get<Parent>(current);
        if (parent == nullptr || parent->parent == NullEntity)
        {
            break;
        }

        const Transform *parentLocal = scene.Get<Transform>(parent->parent);
        if (parentLocal == nullptr)
        {
            break; // a parent with no pose defines no space; the chain ends here
        }

        world = ComposeTransform(*parentLocal, world);
        current = parent->parent;
    }
    return world;
}

const glm::mat4 *ParentWorldMatrix(const Scene &scene, Entity entity)
{
    const Parent *parent = scene.Get<Parent>(entity);
    if (parent == nullptr || parent->parent == NullEntity)
    {
        return nullptr;
    }
    const WorldMatrix *world = scene.Get<WorldMatrix>(parent->parent);
    return world != nullptr ? &world->matrix : nullptr;
}

} // namespace Assisi::ECS
