/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/EventScope.hpp>

#include <unordered_set>

namespace Assisi::Core
{

namespace
{

std::unordered_set<std::type_index> &UniversalTypes()
{
    // Function-local, so a registration running during static initialisation
    // finds a constructed set whatever order the objects were linked in.
    static std::unordered_set<std::type_index> types;
    return types;
}

} // namespace

void DeclareUniversal(std::type_index type)
{
    UniversalTypes().insert(type);
}

EventScope ScopeOf(std::type_index type)
{
    return UniversalTypes().contains(type) ? EventScope::Universal : EventScope::World;
}

} // namespace Assisi::Core
