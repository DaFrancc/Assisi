/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/ECS/Scene.hpp>

#include <Assisi/Core/Logger.hpp>

#include <cstddef>
#include <string_view>
#include <vector>

namespace Assisi::ECS
{

namespace
{
using Core::Reflect::ComponentId;
using Core::Reflect::ComponentMeta;
using Core::Reflect::ComponentRegistry;
using Core::Reflect::kInvalidComponentId;

/// The registered name of @p id, for log lines.
std::string_view NameOf(ComponentId id)
{
    const ComponentMeta *meta = ComponentRegistry::Instance().ById(id);
    return meta == nullptr ? std::string_view{"<unregistered>"} : std::string_view{meta->name};
}
} // namespace

bool Scene::HasById(Entity entity, ComponentId id) const
{
    // .value: array index into _pools.
    return id.value < _pools.size() && _pools[id.value].pool != nullptr &&
           _pools[id.value].has(_pools[id.value].pool, entity);
}

std::optional<ComponentConflict> Scene::ConflictOf(Entity entity, ComponentId id) const
{
    const ComponentRegistry &registry = ComponentRegistry::Instance();
    const ComponentMeta *meta = registry.ById(id);
    if (meta == nullptr)
    {
        return std::nullopt;
    }

    // The component itself first, then each requirement the add would bring.
    if (const std::optional<ComponentConflict> own = IncomingConflict(entity, *meta); own.has_value())
    {
        return own;
    }
    for (const ComponentId requirement : meta->required)
    {
        if (const std::optional<ComponentConflict> pulled = IncomingConflict(entity, *registry.ById(requirement));
            pulled.has_value())
        {
            return pulled;
        }
    }
    return std::nullopt;
}

std::optional<ComponentConflict> Scene::IncomingConflict(Entity entity, const ComponentMeta &meta) const
{
    // One the entity already has was checked when it arrived.
    if (HasById(entity, meta.id))
    {
        return std::nullopt;
    }
    for (const ComponentId excluded : meta.excluded)
    {
        if (HasById(entity, excluded))
        {
            return ComponentConflict{.wanted = meta.id, .present = excluded};
        }
    }
    return std::nullopt;
}

ComponentId Scene::RequirerOf(Entity entity, ComponentId id) const
{
    const ComponentMeta *meta = ComponentRegistry::Instance().ById(id);
    if (meta == nullptr)
    {
        return kInvalidComponentId;
    }
    for (const ComponentId requirer : meta->requiredBy)
    {
        if (HasById(entity, requirer))
        {
            return requirer;
        }
    }
    return kInvalidComponentId;
}

RemoveResult Scene::RemoveById(Entity entity, ComponentId id)
{
    if (!HasById(entity, id))
    {
        return RemoveResult::Absent;
    }
    if (const ComponentId requirer = RequirerOf(entity, id); requirer != kInvalidComponentId)
    {
        Core::Log::Error("Scene: cannot remove '{}' from entity {} (gen {}) - '{}' requires it.", NameOf(id),
                         entity.index, entity.generation, NameOf(requirer));
        return RemoveResult::Required;
    }
    // .value: array index into _pools.
    _pools[id.value].remove(_pools[id.value].pool, entity);
    return RemoveResult::Removed;
}

bool Scene::RemoveManyById(Entity entity, std::span<const ComponentId> ids)
{
    // Each pass removes every member nothing left on the entity requires, which
    // frees the members those required. A pass that removes nothing means the
    // rest are held by a component outside the set.
    std::vector<ComponentId> pending(ids.begin(), ids.end());
    bool progressed = true;
    while (progressed)
    {
        progressed = false;
        for (std::size_t i = 0; i < pending.size();)
        {
            const ComponentId id = pending[i];
            if (HasById(entity, id) && RequirerOf(entity, id) != kInvalidComponentId)
            {
                ++i;
                continue;
            }
            if (HasById(entity, id))
            {
                // .value: array index into _pools.
                _pools[id.value].remove(_pools[id.value].pool, entity);
            }
            pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(i));
            progressed = true;
        }
    }
    for (const ComponentId id : pending)
    {
        Core::Log::Error("Scene: cannot remove '{}' from entity {} (gen {}) - '{}' requires it.", NameOf(id),
                         entity.index, entity.generation, NameOf(RequirerOf(entity, id)));
    }
    return pending.empty();
}

void Scene::AddRequirements(Entity entity, ComponentId id)
{
    const ComponentRegistry &registry = ComponentRegistry::Instance();
    const ComponentMeta *meta = registry.ById(id);
    if (meta == nullptr)
    {
        return;
    }
    for (const ComponentId requirement : meta->required)
    {
        if (!HasById(entity, requirement))
        {
            registry.ById(requirement)->addDefault(this, entity.index, entity.generation);
        }
    }
}

void Scene::ReportConflict(Entity entity, ComponentId id, ComponentConflict conflict) const
{
    if (conflict.wanted == id)
    {
        Core::Log::Error("Scene: cannot add '{}' to entity {} (gen {}) - it and '{}' exclude each other.",
                         NameOf(id), entity.index, entity.generation, NameOf(conflict.present));
    }
    else
    {
        Core::Log::Error("Scene: cannot add '{}' to entity {} (gen {}) - it requires '{}', which cannot share "
                         "an entity with '{}'.",
                         NameOf(id), entity.index, entity.generation, NameOf(conflict.wanted),
                         NameOf(conflict.present));
    }
    ASSISI_ASSERT(false, "Scene::Add of a component a component on the entity excludes");
}

} // namespace Assisi::ECS
