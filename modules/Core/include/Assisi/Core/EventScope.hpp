/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file EventScope.hpp
/// @brief Which queue an event type belongs in: its world's, or the one the
/// whole program shares.
///
/// Most events are about one world — an entity was hit, a sound should play —
/// and an entity id means nothing outside the world it lives in, so they stay
/// in that world's queue. A few belong to the whole program, such as quitting
/// or the editor's selection. Those are declared `AEVENT(universal)`, and the
/// registration reflectgen generates for one records it here before main runs.
///
/// A type nobody declared is a world event, which covers every event a test
/// defines for itself without reflecting it.

#include <cstdint>
#include <typeindex>
#include <typeinfo>

namespace Assisi::Core
{

enum class EventScope : uint8_t
{
    World,     ///< read only by the systems of the world it was pushed in
    Universal, ///< read by every world's systems, and by the application
    Count_
};

/// @brief Records that events of @p type belong to the whole program. Called by
/// the registration generated for an `AEVENT(universal)`, and by a test
/// declaring a type of its own.
void DeclareUniversal(std::type_index type);

/// @brief The scope events of @p type belong to: Universal if declared so,
/// World otherwise.
[[nodiscard]] EventScope ScopeOf(std::type_index type);

template <typename E> [[nodiscard]] EventScope ScopeOf()
{
    return ScopeOf(std::type_index(typeid(E)));
}

} // namespace Assisi::Core
