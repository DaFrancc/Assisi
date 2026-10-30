/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/Reflect/StructRegistry.hpp>

#include <Assisi/Core/Logger.hpp>

#include <algorithm>

namespace Assisi::Core::Reflect
{

StructRegistry &StructRegistry::Instance()
{
    // Function-local, so a registration running during static initialisation
    // finds a constructed table whatever order the objects were linked in.
    static StructRegistry registry;
    return registry;
}

void StructRegistry::Register(std::string_view name, SpecBuilder spec)
{
    if (std::ranges::contains(_entries, name, &Entry::name))
    {
        Log::Error("StructRegistry: '{}' is already declared; the second struct of that name is ignored.", name);
        return;
    }
    _entries.push_back(Entry{.name = std::string{name}, .spec = spec});
}

const StructSpec *StructRegistry::Find(std::string_view name) const
{
    const std::vector<Entry>::const_iterator found = std::ranges::find(_entries, name, &Entry::name);
    return found != _entries.end() ? found->spec() : nullptr;
}

} // namespace Assisi::Core::Reflect
