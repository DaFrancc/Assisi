/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TestWorld.hpp
/// @brief A world for screens under test: the queue a test reads, and the
/// entities the test says are alive.

#include <Assisi/Mondrian/ScreenWorld.hpp>

#include <Assisi/Core/EventQueue.hpp>

#include <algorithm>
#include <vector>

namespace Assisi::Mondrian
{

class TestWorld final : public ScreenWorld
{
  public:
    /// @p events must outlive the world.
    explicit TestWorld(Core::EventQueue &events) : _events(events) {}

    [[nodiscard]] Core::EventQueue &Events() override { return _events; }
    [[nodiscard]] bool IsAlive(EntityId entity) const override { return std::ranges::contains(alive, entity); }

    /// Every entity IsAlive answers true for.
    std::vector<EntityId> alive;

  private:
    Core::EventQueue &_events;
};

} // namespace Assisi::Mondrian
