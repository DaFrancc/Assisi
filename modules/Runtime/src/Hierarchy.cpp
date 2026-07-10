/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/Hierarchy.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <unordered_map>
#include <unordered_set>

namespace Assisi::Runtime
{

void PropagateTransforms(ECS::Scene &scene)
{
    using TransformCache = std::unordered_map<uint64_t, glm::mat4>;

    // Reused across calls rather than reallocated. PropagateTransforms runs at
    // least twice per frame (game + camera scenes) and is a hot CPU path; clear()
    // retains the allocated buckets, so after warmup no per-call heap allocation
    // happens. thread_local keeps it correct if propagation is ever driven from
    // more than one thread without adding a lock — the calls are otherwise
    // sequential on the main thread and never re-entrant across each other.
    thread_local TransformCache               cache;
    thread_local std::unordered_set<uint64_t> onChain; // entities on the current recursion stack
    cache.clear();
    onChain.clear();

    auto entityKey = [](ECS::Entity e) -> uint64_t
    {
        return (static_cast<uint64_t>(e.generation) << 32) | e.index;
    };

    auto localMatrix = [](const TransformComponent &t) -> glm::mat4
    {
        return glm::translate(glm::mat4(1.f), t.position) * glm::mat4_cast(t.rotation) *
               glm::scale(glm::mat4(1.f), t.scale);
    };

    // Recursive via C++23 deducing this (self) — resolves parents without a
    // heap-allocated std::function closure.
    auto worldMatrix = [&](this const auto &self, ECS::Entity e) -> glm::mat4
    {
        const uint64_t key = entityKey(e);
        if (TransformCache::const_iterator it = cache.find(key); it != cache.end())
            return it->second;

        const TransformComponent *t = scene.Get<TransformComponent>(e);
        const glm::mat4 local = t ? localMatrix(*t) : glm::mat4(1.f);

        glm::mat4 world = local;
        if (const ParentComponent *p = scene.Get<ParentComponent>(e); p && p->parent != ECS::NullEntity)
        {
            /* Cycle guard: a hand-edited .alvl can contain a parent loop
               (A->B->A). If this entity's parent is already on the chain we are
               resolving, recursing would run until the stack dies — break the
               cycle by treating this node as a root instead. */
            if (onChain.contains(entityKey(p->parent)))
                Core::Log::Error(
                    "PropagateTransforms: parent cycle at entity index {} (gen {}); treating as root",
                    e.index, e.generation);
            else
            {
                onChain.insert(key);
                world = self(p->parent) * local;
                onChain.erase(key);
            }
        }

        cache.emplace(key, world);
        return world;
    };

    for (auto [entity, transform] : scene.Query<TransformComponent>())
        transform.worldMatrix = worldMatrix(entity);
}

} // namespace Assisi::Runtime