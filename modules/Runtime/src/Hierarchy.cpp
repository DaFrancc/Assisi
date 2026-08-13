/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/Hierarchy.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace Assisi::Runtime
{

namespace
{
glm::mat4 LocalMatrix(const Transform &t)
{
    return glm::translate(glm::mat4(1.f), t.position) * glm::mat4_cast(t.rotation) *
           glm::scale(glm::mat4(1.f), t.scale);
}

// Per-entity scratch for one propagation pass, indexed by Entity::index. Replaces
// the old unordered_map (memoisation) + unordered_set (cycle stack) with a single
// dense, reused array — no per-entity hashing. `passId` stamps which pass last
// touched a slot (so the array need never be cleared between passes); `resolving`
// marks entities currently on the recursion stack for cycle detection.
struct PassState
{
    uint32_t passId       = 0;
    bool     worldChanged = false;
    bool     resolving    = false;
};
} // namespace

uint64_t PropagateTransforms(ECS::Scene &scene, uint64_t lastTick)
{
    // Reused across calls; grows to the largest entity index seen. thread_local so
    // propagation driven from another thread can't interleave with this one.
    thread_local std::vector<PassState> passState;
    thread_local uint32_t               passCounter = 0;
    const uint32_t                      pass = ++passCounter;

    // Entities (index+generation packed) whose parent cycle was already
    // reported. Propagation runs every frame, so without this a single bad
    // .alvl loops one Error line forever. Bounded by the number of distinct
    // cyclic entities ever seen — diagnostic-only state, never read back.
    thread_local std::unordered_set<uint64_t> reportedCycles;

    auto slot = [&](uint32_t index) -> PassState &
    {
        if (index >= passState.size())
            passState.resize(index + 1);
        return passState[index];
    };

    // Resolves e's worldMatrix, recomputing it only when e's local TRS changed
    // (per the ECS change tick, since `lastTick`) or an ancestor changed; returns
    // whether it changed this pass so a child can decide if it must recompute too.
    // Recursive via C++23 deducing this (self), so parents resolve before children.
    auto resolve = [&](this const auto &self, ECS::Entity e) -> bool
    {
        Transform *t = scene.Get<Transform>(e);
        if (t == nullptr)
            return false; // a parent without a Transform contributes identity

        // Note: `slot()` may resize `passState` (invalidating references) during
        // the parent recursion below, so never hold a PassState& across a self()
        // call — re-fetch by index each time.
        if (slot(e.index).passId == pass)
            return slot(e.index).worldChanged; // already resolved this pass (shared parent)

        slot(e.index).passId    = pass;
        slot(e.index).resolving = true; // on the resolve stack, for cycle detection

        // Recompute when the local TRS changed OR the parent link changed (attach /
        // reparent). Parent is ACOMP(tracked) precisely so this second case is caught;
        // without it, attaching a parent after a pass leaves a stale worldMatrix until
        // something else moves the child. (Detach via Remove<Parent> doesn't stamp, so
        // a site that detaches should stamp the child's Transform — no such site today.)
        const bool localChanged =
            scene.Changed<Transform>(e, lastTick) || scene.Changed<Parent>(e, lastTick);

        bool             parentChanged = false;
        const glm::mat4 *parentWorld   = nullptr;
        if (const Parent *p = scene.Get<Parent>(e); p != nullptr && p->parent != ECS::NullEntity)
        {
            Transform *parentTransform = scene.Get<Transform>(p->parent);
            const bool parentOnStack =
                parentTransform != nullptr && slot(p->parent.index).passId == pass && slot(p->parent.index).resolving;
            if (parentOnStack)
            {
                /* A hand-edited .alvl can contain a parent loop (A->B->A). The
                   parent is already on this resolve stack, so recursing would not
                   terminate — break the cycle by treating this node as a root. */
                const uint64_t packed = (static_cast<uint64_t>(e.generation) << 32u) | e.index;
                if (reportedCycles.insert(packed).second)
                {
                    Core::Log::Error(
                        "PropagateTransforms: parent cycle at entity index {} (gen {}); treating as root",
                        e.index, e.generation);
                }
            }
            else if (parentTransform != nullptr)
            {
                parentChanged = self(p->parent);
                parentWorld   = &parentTransform->worldMatrix; // finalised by the recursion; pool ptr stays valid
            }
        }

        const bool worldChanged = localChanged || parentChanged;
        if (worldChanged)
        {
            const glm::mat4 local = LocalMatrix(*t);
            // Written through the plain (non-stamping) Get, and it must stay that
            // way: worldMatrix is derived output, so stamping here would re-mark
            // this Transform changed and every resolved entity would look dirty
            // again on the next pass — a self-retriggering loop that turns the
            // dirty-skip into a no-op. This is the one place where writing a
            // tracked component without stamping is correct, and it is safe because
            // worldMatrix is not a reflected AFIELD: it is never serialized and
            // never replicated, so no consumer outside this file needs the signal.
            // Peers recompute it from the local TRS with their own propagation pass.
            t->worldMatrix = parentWorld != nullptr ? (*parentWorld * local) : local;
        }

        slot(e.index).worldChanged = worldChanged;
        slot(e.index).resolving    = false;
        return worldChanged;
    };

    // Deliberately the plain Query, and it must stay one. This loop only enumerates
    // the Transform holders (resolve() re-fetches by entity); switching it to
    // QueryMut would stamp every Transform in the scene every frame, which defeats
    // the dirty-skip below — the next pass would find everything changed — and would
    // hand network delta replication a full Transform set per tick. See the write in
    // resolve() for why the worldMatrix store is likewise non-stamping.
    for (auto [entity, transform] : scene.Query<Transform>())
        resolve(entity);

    // The tick a caller should pass as `lastTick` next frame: everything written
    // up to now has been accounted for.
    return scene.CurrentChangeTick();
}

std::vector<ECS::Entity> GatherSubtree(ECS::Scene &scene, ECS::Entity root)
{
    std::vector<ECS::Entity> result{root};

    // Breadth-first: for each collected entity, sweep for entities whose Parent
    // points at it. `result` grows as we go and the index walk visits each new
    // entry, so a whole subtree of any depth is collected. No child index exists,
    // so this scans — acceptable at subtree-edit scale.
    for (std::size_t i = 0; i < result.size(); ++i)
    {
        const ECS::Entity current = result[i];
        scene.ForEachEntity(
            [&](ECS::Entity e)
            {
                const Parent *parent = scene.Get<Parent>(e);
                if (parent == nullptr || parent->parent != current)
                    return;
                if (std::find(result.begin(), result.end(), e) == result.end())
                    result.push_back(e);
            });
    }
    return result;
}

ECS::Transform WorldTransformOf(const ECS::Scene &scene, ECS::Entity entity)
{
    const Transform *local = scene.Get<Transform>(entity);
    if (local == nullptr)
        return {};

    ECS::Transform world = *local;

    // Bounded rather than walked to the root: a cycle in Parent is a corrupt
    // scene, and the answer to a corrupt chain is a wrong pose, not a hang. The
    // bound is generous enough that no real hierarchy reaches it.
    constexpr uint32_t kMaxDepth = 256;
    ECS::Entity        current   = entity;
    for (uint32_t depth = 0; depth < kMaxDepth; ++depth)
    {
        const Parent *parent = scene.Get<Parent>(current);
        if (parent == nullptr || parent->parent == ECS::NullEntity)
            break;

        const Transform *parentLocal = scene.Get<Transform>(parent->parent);
        if (parentLocal == nullptr)
            break; // a parent with no pose defines no space; the chain ends here

        world   = ComposeTransform(*parentLocal, world);
        current = parent->parent;
    }
    return world;
}

} // namespace Assisi::Runtime
