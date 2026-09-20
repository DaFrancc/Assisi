/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file EventCatalog.hpp
/// @brief Every event type this build declares, by the name a file uses to push one.
///
/// C++ names an event by its type, which the compiler checks. A file cannot: a
/// screen written in markup says `on_click="Game::ResumeClicked"` and something
/// has to turn that string into a push. This is that something, and it exists
/// for the files — code that has the type pushes it directly and never comes
/// here.
///
/// An entry holds the push rather than the type, so a caller binds one to a
/// button without knowing what it is pushing. That is what lets the UI stay
/// ignorant of game types while still firing them.
///
/// The name is the fully-qualified type name, derived rather than declared. The
/// opposite of a system name, and the difference is who catches a rename: a
/// system name is an invented string, so deriving it would let a C++ rename
/// silently rename content that fails at load. Every file naming an event is
/// cooked against this catalog, so a rename fails the cook pointing at the file
/// that still says the old name.
///
/// Populated by static initializers in the generated OBJECT libraries, which
/// cmake/AssisiReflect.cmake pulls fully into the final link precisely so a
/// registration nobody references still runs — the same path `ACOMP` and
/// `ASYSTEM` already ride.

#include <Assisi/Core/EventQueue.hpp>

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Core
{

/// @brief One event type, as its declaration described it.
struct EventDefinition
{
    /// The fully-qualified type name, which is what a file writes.
    std::string name;

    /// Pushes a default-constructed event of that type. Generated code holds
    /// the type; nothing here does.
    std::function<void(EventQueue &)> push;
};

/// @brief The table of declared event types.
///
/// Instances are constructible, not only the process-wide one: a caller that
/// resolves names — a screen loader, a test — takes a `const EventCatalog &`, so
/// it can be handed a catalog holding exactly the entries a case is about
/// instead of whatever the link happened to register.
class EventCatalog
{
  public:
    /// @brief The process-wide table the generated registrations fill.
    static EventCatalog &Instance();

    /// @brief Adds a definition. Called by generated code, and by a test
    /// building a catalog of its own.
    void Register(EventDefinition definition);

    /// @brief The definition @p name refers to, or nullptr.
    [[nodiscard]] const EventDefinition *Find(std::string_view name) const;

    /// @brief Every declared event, in registration order. For diagnostics
    /// naming what *is* available when a file names something that is not.
    [[nodiscard]] std::span<const EventDefinition> All() const { return _definitions; }

  private:
    std::vector<EventDefinition> _definitions;
};

} // namespace Assisi::Core
