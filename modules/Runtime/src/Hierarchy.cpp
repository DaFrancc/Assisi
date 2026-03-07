#include <Assisi/Runtime/Hierarchy.hpp>

#include <Assisi/Runtime/Components.hpp>

#include <functional>
#include <unordered_map>

namespace Assisi::Runtime
{

void PropagateTransforms(ECS::Scene &scene)
{
    std::unordered_map<uint64_t, glm::mat4> cache;

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

        const auto *p         = scene.Get<ParentComponent>(e);
        const glm::mat4 world =
            (p && p->parent != ECS::NullEntity) ? worldMatrix(p->parent) * local : local;

        cache.emplace(key, world);
        return world;
    };

    for (auto [entity, transform] : scene.Query<TransformComponent>())
        transform.worldMatrix = worldMatrix(entity);
}

} // namespace Assisi::Runtime