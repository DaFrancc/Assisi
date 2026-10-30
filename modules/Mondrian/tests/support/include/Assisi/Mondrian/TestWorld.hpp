/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TestWorld.hpp
/// @brief A world for screens under test: the queue a test reads, the entities
/// the test says are alive, and the components it says they have.

#include <Assisi/Mondrian/ScreenWorld.hpp>

#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/Reflect/ComponentMeta.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Mondrian
{

class TestWorld final : public ScreenWorld
{
  public:
    /// One component an entity has, held by the test.
    struct Held
    {
        std::string component;
        const void *data = nullptr;
        EntityId entity;
    };

    /// @p events must outlive the world.
    explicit TestWorld(Core::EventQueue &events) : _events(events) {}

    [[nodiscard]] Core::EventQueue &Events() override { return _events; }
    [[nodiscard]] bool IsAlive(EntityId entity) const override { return std::ranges::contains(alive, entity); }
    [[nodiscard]] const Core::Reflect::ComponentMeta *FindComponent(std::string_view name) const override
    {
        const std::vector<Core::Reflect::ComponentMeta>::const_iterator found =
            std::ranges::find(components, name, &Core::Reflect::ComponentMeta::name);
        return found != components.end() ? &*found : nullptr;
    }
    [[nodiscard]] const void *Read(const Core::Reflect::ComponentMeta &component, EntityId entity) const override
    {
        const std::vector<Held>::const_iterator found = std::ranges::find_if(
            held, [&](const Held &one) { return one.entity == entity && one.component == component.name; });
        return found != held.end() ? found->data : nullptr;
    }

    /// The components bindings are looked up among. Filled before anything
    /// binds, since a binding keeps a pointer into it.
    std::vector<Core::Reflect::ComponentMeta> components;
    /// Every entity IsAlive answers true for.
    std::vector<EntityId> alive;
    /// What Read finds.
    std::vector<Held> held;

  private:
    Core::EventQueue &_events;
};

} // namespace Assisi::Mondrian
