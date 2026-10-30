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

#include <Assisi/Core/EventRoute.hpp>

#include <concepts>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Assisi::Core
{

/// @brief What a file's control hands the event it pushes: whether it is on,
/// where it rests, or which step it is on.
using EventValue = std::variant<bool, float, int32_t>;

/// @brief What an event's field called `value` can take.
enum class EventValueKind : uint8_t
{
    None,  ///< it has no such field, so it is pushed as it is
    Bool,
    Float,
    Int32,
    Other, ///< it has one, of a type no control carries
    Count_
};

/// @brief The kind a `value` field of type @p T is.
template <typename T> [[nodiscard]] consteval EventValueKind EventValueKindOf()
{
    if constexpr (std::same_as<T, bool>)
    {
        return EventValueKind::Bool;
    }
    else if constexpr (std::same_as<T, float>)
    {
        return EventValueKind::Float;
    }
    else if constexpr (std::same_as<T, int32_t>)
    {
        return EventValueKind::Int32;
    }
    else
    {
        return EventValueKind::Other;
    }
}

/// @brief One event type, as its declaration described it.
struct EventDefinition
{
    /// The fully-qualified type name, which is what a file writes.
    std::string name;

    /// Pushes a default-constructed event of that type into the queue its
    /// scope names. Generated code holds the type; nothing here does.
    std::function<void(const EventRoute &)> push;

    /// The same with its `value` field set. Null unless valueKind is one a
    /// control carries.
    std::function<void(const EventRoute &, const EventValue &)> pushValue;

    EventValueKind valueKind = EventValueKind::None;
};

/// @brief The definition of the event type @p E, called @p name.
template <typename E> [[nodiscard]] EventDefinition DefineEvent(std::string name)
{
    EventDefinition definition{.name = std::move(name),
                               .push = [](const EventRoute &events) { events.Push(E{}); },
                               .pushValue = {},
                               .valueKind = EventValueKind::None};
    if constexpr (requires { E::value; })
    {
        using Value = std::remove_cv_t<decltype(E::value)>;
        definition.valueKind = EventValueKindOf<Value>();
        if constexpr (EventValueKindOf<Value>() != EventValueKind::Other)
        {
            definition.pushValue = [](const EventRoute &events, const EventValue &value)
                                   {
                                       E event{};
                                       if (const Value *const carried = std::get_if<Value>(&value))
                                       {
                                           event.value = *carried;
                                       }
                                       events.Push(event);
                                   };
        }
    }
    return definition;
}

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
