/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/EventCatalog.hpp>

#include <Assisi/Core/Logger.hpp>

#include <utility>

namespace Assisi::Core
{

EventCatalog &EventCatalog::Instance()
{
    // Function-local, so a registration running during static initialisation
    // finds a constructed table whatever order the objects were linked in.
    static EventCatalog catalog;
    return catalog;
}

void EventCatalog::Register(EventDefinition definition)
{
    // Duplicate names are a build error in reflectgen's whole-tree pass, so
    // reaching this means two builds of the same generated unit were linked
    // together. Refuse the second either way: which one wins would otherwise be
    // link order, which is to say accident.
    if (Find(definition.name) != nullptr)
    {
        Log::Error("EventCatalog: '{}' is already declared; the second declaration is ignored.", definition.name);
        return;
    }
    _definitions.push_back(std::move(definition));
}

const EventDefinition *EventCatalog::Find(std::string_view name) const
{
    for (const EventDefinition &definition : _definitions)
    {
        if (definition.name == name)
        {
            return &definition;
        }
    }
    return nullptr;
}

} // namespace Assisi::Core
