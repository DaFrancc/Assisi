/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ScreenWorld.hpp
/// @brief What a screen reaches of the world that owns it.
///
/// A screen is owned by one world and destroyed with it, so the world it reads
/// is always there while the screen is. The UI knows that world only through
/// this interface: the core never names the engine's entities or scenes, which
/// is what keeps it testable headless and usable outside a game. It reaches a
/// component through reflection, which needs neither.

#include <Assisi/Mondrian/Binding.hpp>

#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/Reflect/ComponentMeta.hpp>
#include <Assisi/Core/Reflect/StructMeta.hpp>

#include <cstdint>
#include <string_view>

namespace Assisi::Mondrian
{

/// @brief The index and generation an entity id holds while it names nothing.
inline constexpr uint32_t kNoEntityIndex = UINT32_MAX;

/// @brief An entity of the world that owns a screen: the slot it sits in and
/// which life of that slot it is, so an id kept past the entity's death never
/// names whatever takes the slot next.
struct EntityId
{
    uint32_t index = kNoEntityIndex;
    uint32_t generation = kNoEntityIndex;

    [[nodiscard]] friend constexpr bool operator==(const EntityId &, const EntityId &) = default;

    /// @brief Whether this names an entity at all.
    [[nodiscard]] explicit constexpr operator bool() const
    {
        return index != kNoEntityIndex || generation != kNoEntityIndex;
    }
};

class ScreenWorld
{
  public:
    ScreenWorld() = default;
    virtual ~ScreenWorld() = default;

    ScreenWorld(const ScreenWorld &) = delete;
    ScreenWorld &operator=(const ScreenWorld &) = delete;
    ScreenWorld(ScreenWorld &&) = delete;
    ScreenWorld &operator=(ScreenWorld &&) = delete;

    /// @brief The queue a screen pushes this world's events into.
    [[nodiscard]] virtual Core::EventQueue &Events() = 0;

    /// @brief Whether @p entity is still in this world: false once it has been
    /// destroyed, even when another entity has taken its slot since.
    [[nodiscard]] virtual bool IsAlive(EntityId entity) const = 0;

    /// @brief The component this world's entities can have that is called
    /// @p name, or null when none is. Valid as long as the world is.
    [[nodiscard]] virtual const Core::Reflect::ComponentMeta *FindComponent(std::string_view name) const = 0;

    /// @brief The struct called @p name that a registered source can be, or
    /// null when none is. Valid as long as the world is.
    [[nodiscard]] virtual const Core::Reflect::StructSpec *FindStruct(std::string_view name) const = 0;

    /// @brief @p entity's @p component, or null when it has none. Valid until
    /// the world next runs a system.
    [[nodiscard]] virtual const void *Read(const Core::Reflect::ComponentMeta &component, EntityId entity) const = 0;

    /// @brief The same to write to, which counts as changing it, exactly as
    /// fetching it for writing in game code does.
    [[nodiscard]] virtual void *Write(const Core::Reflect::ComponentMeta &component, EntityId entity) = 0;

    /// @brief What a binding naming @p name looks at.
    [[nodiscard]] BindingOwner FindOwner(std::string_view name) const
    {
        return {.component = FindComponent(name), .structure = FindStruct(name)};
    }
};

} // namespace Assisi::Mondrian
