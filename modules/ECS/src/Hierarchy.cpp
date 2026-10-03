/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/ECS/Hierarchy.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/TransformPose.hpp>

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace Assisi::ECS
{

namespace
{
glm::mat4 LocalMatrix(const Transform &t)
{
    return glm::translate(glm::mat4(1.f), t.position) * glm::mat4_cast(t.rotation) *
           glm::scale(glm::mat4(1.f), t.scale);
}

// Per-entity scratch for one propagation pass, indexed by Entity::index. One dense
// reused array rather than a map + set, so nothing is hashed per entity. `passId`
// stamps which pass last touched a slot (so the array need never be cleared between
// passes); `resolving` marks entities on the recursion stack for cycle detection.
struct PassState
{
    uint32_t passId = 0;
    bool worldChanged = false;
    bool resolving = false;
};

/// One PropagateTransforms call: what it reads, what it writes, and the
/// per-entity scratch it keeps.
struct Propagation
{
    Scene &scene;
    std::vector<glm::mat4> &world;
    std::vector<PassState> &passState;

    /// Entities (index and generation packed) whose parent cycle was already
    /// reported, so a bad level logs it once rather than every frame.
    std::unordered_set<uint64_t> &reportedCycles;

    uint64_t lastTick;
    uint32_t pass;

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

    /// Resolves @p e's world matrix, recomputing it only when its local TRS or
    /// its parent link changed since `lastTick`, or an ancestor's world matrix
    /// changed this pass. Parents are resolved first, by recursion.
    ///
    /// @return whether @p e's world matrix changed this pass, so a child knows
    /// it must recompute too.
    bool Resolve(Entity e)
    {
        const Transform *t = scene.Get<Transform>(e);
        if (t == nullptr)
        {
            return false; // a parent without a Transform contributes identity
        }
        if (Slot(e.index).passId == pass)
        {
            return Slot(e.index).worldChanged; // already resolved this pass (shared parent)
        }
        Slot(e.index).passId = pass;
        Slot(e.index).resolving = true;

        // The parent link counts as well as the TRS: Parent is ACOMP(tracked) so
        // an attach or a reparent is seen. A detach through Remove<Parent> does
        // not stamp; whatever detaches should stamp the child's Transform.
        const bool localChanged = scene.Changed<Transform>(e, lastTick) || scene.Changed<Parent>(e, lastTick);

        bool parentChanged = false;
        const glm::mat4 *parentWorld = ResolveParent(e, parentChanged);

        const bool worldChanged = localChanged || parentChanged;
        if (worldChanged)
        {
            // The lane is derived output, not part of the component, so writing
            // it stamps nothing and nothing is serialized or replicated.
            const glm::mat4 local = LocalMatrix(*t);
            world[scene.DenseIndexOf<Transform>(e)] = parentWorld != nullptr ? (*parentWorld * local) : local;
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
        if (p == nullptr || p->parent == NullEntity || !scene.Has<Transform>(p->parent))
        {
            return nullptr;
        }

        // A hand-edited level can hold a parent loop (A->B->A). The parent is
        // already on the resolve stack, so recursing would not terminate: this
        // entity is treated as a root instead.
        if (Slot(p->parent.index).passId == pass && Slot(p->parent.index).resolving)
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
        // Finalised by the recursion; the lane does not move during a pass.
        return &world[scene.DenseIndexOf<Transform>(p->parent)];
    }
};
} // namespace

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
    thread_local std::unordered_set<uint64_t> reportedCycles;
    thread_local uint32_t passCounter = 0;

    Propagation propagation{.scene = scene,
                            .world = lanes->world,
                            .passState = passState,
                            .reportedCycles = reportedCycles,
                            .lastTick = lastTick,
                            .pass = ++passCounter};

    // Deliberately the plain Query, and it must stay one: it only enumerates the
    // Transform holders (Resolve re-fetches by entity), and QueryMut would stamp
    // every Transform every frame — defeating the dirty-skip and handing network
    // delta replication a full Transform set per tick.
    for (auto [entity, transform] : scene.Query<Transform>())
    {
        (void)transform;
        (void)propagation.Resolve(entity);
    }

    // The tick a caller should pass as `lastTick` next frame: everything written
    // up to now has been accounted for.
    return scene.CurrentChangeTick();
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

const glm::mat4 *WorldMatrix(const Scene &scene, Entity entity)
{
    const SparseSetLanes<Transform> *lanes = scene.Lanes<Transform>();
    const uint32_t index = scene.DenseIndexOf<Transform>(entity);
    if (lanes == nullptr || index == SparseSet<Transform>::Invalid)
    {
        return nullptr;
    }
    return &lanes->world[index];
}

const glm::mat4 *ParentWorldMatrix(const Scene &scene, Entity entity)
{
    const Parent *parent = scene.Get<Parent>(entity);
    if (parent == nullptr || parent->parent == NullEntity)
    {
        return nullptr;
    }
    return WorldMatrix(scene, parent->parent);
}

} // namespace Assisi::ECS
