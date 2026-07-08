/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/Hierarchy.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace Assisi::Runtime
{

void PropagateTransforms(ECS::Scene &scene)
{
    std::unordered_map<uint64_t, glm::mat4> cache;
    std::unordered_set<uint64_t> onChain; // entities on the current recursion stack

    auto entityKey = [](ECS::Entity e) -> uint64_t
    {
        return (static_cast<uint64_t>(e.generation) << 32) | e.index;
    };

    auto localMatrix = [](const TransformComponent &t) -> glm::mat4
    {
        return glm::translate(glm::mat4(1.f), t.position) * glm::mat4_cast(t.rotation) *
               glm::scale(glm::mat4(1.f), t.scale);
    };

    std::function<glm::mat4(ECS::Entity)> worldMatrix = [&](ECS::Entity e) -> glm::mat4
    {
        const uint64_t key = entityKey(e);
        if (const auto it = cache.find(key); it != cache.end())
            return it->second;

        const auto *t         = scene.Get<TransformComponent>(e);
        const glm::mat4 local = t ? localMatrix(*t) : glm::mat4(1.f);

        glm::mat4 world = local;
        if (const auto *p = scene.Get<ParentComponent>(e); p && p->parent != ECS::NullEntity)
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
                world = worldMatrix(p->parent) * local;
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