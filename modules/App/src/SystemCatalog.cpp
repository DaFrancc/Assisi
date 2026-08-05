/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/SystemCatalog.hpp>

#include <Assisi/App/World.hpp>
#include <Assisi/Core/Logger.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace Assisi::App
{

namespace
{

/// One world's pending install, queued from wherever the spawn happened and
/// applied at the frame's safe point.
struct PendingInstall
{
    World                   *world = nullptr;
    std::vector<std::string> names;
    std::string              context;
};

std::vector<PendingInstall> &Pending()
{
    static std::vector<PendingInstall> pending;
    return pending;
}

} // namespace

SystemCatalog &SystemCatalog::Instance()
{
    static SystemCatalog catalog;
    return catalog;
}

void SystemCatalog::Register(SystemDefinition definition)
{
    // Duplicate names are a build error in reflectgen's whole-tree pass, so
    // reaching this is either a hand-written registration or two builds of the
    // same generated unit linked together. Refuse the second either way: which
    // one wins would otherwise be link order, which is to say accident.
    if (Find(definition.name) != nullptr)
    {
        Core::Log::Error("SystemCatalog: '{}' is already declared; the second declaration is ignored.",
                         definition.name);
        return;
    }
    _definitions.push_back(std::move(definition));
}

const SystemDefinition *SystemCatalog::Find(std::string_view name) const
{
    for (const SystemDefinition &definition : _definitions)
    {
        if (definition.name == name)
            return &definition;
    }
    return nullptr;
}

bool SystemCatalog::Install(World &world, std::span<const std::string> names, std::string_view context) const
{
    // Resolve everything first. A half-installed world runs and looks nearly
    // right, which is worse than a refused load — and the name that failed is the
    // one thing the author needs to be told.
    std::vector<const SystemDefinition *> resolved;
    resolved.reserve(names.size());

    bool ok = true;
    for (const std::string &name : names)
    {
        const SystemDefinition *definition = Find(name);
        if (definition == nullptr)
        {
            std::string available;
            for (const SystemDefinition &candidate : _definitions)
                available += (available.empty() ? "" : ", ") + candidate.name;

            Core::Log::Error("SystemCatalog: '{}' names system '{}', which this build does not declare. "
                             "Declared: [{}].",
                             context, name, available);
            ok = false;
            continue;
        }
        resolved.push_back(definition);
    }
    if (!ok)
        return false;

    for (const SystemDefinition *definition : resolved)
    {
        // A union, not a concatenation: two nested blueprints both naming Bounce
        // install it once, and a spawn into a world that already has it costs a
        // lookup.
        if (world.systems.Has(definition->name))
            continue;

        if (definition->isRender)
        {
            world.systems.RegisterRender(definition->name, definition->runRender);
        }
        else
        {
            SystemRegistry::SystemHandle handle =
                world.systems.Register(definition->phase, definition->name, definition->run);
            for (const std::string &target : definition->after)
                handle.After(target);
            for (const std::string &target : definition->before)
                handle.Before(target);
            if (definition->activeWorldOnly)
                handle.ActiveWorldOnly();
        }
    }

    return true;
}

void QueueSystemInstall(World &world, std::span<const std::string> names, std::string_view context)
{
    if (names.empty())
        return;

    // Coalesced per world, so a hundred bullets spawned in one frame queue one
    // entry rather than a hundred.
    for (PendingInstall &pending : Pending())
    {
        if (pending.world != &world)
            continue;
        for (const std::string &name : names)
        {
            if (std::find(pending.names.begin(), pending.names.end(), name) == pending.names.end())
                pending.names.push_back(name);
        }
        return;
    }

    Pending().push_back(PendingInstall{
        .world = &world, .names = {names.begin(), names.end()}, .context = std::string{context}});
}

void DrainSystemInstalls()
{
    if (Pending().empty())
        return;

    // Moved out first: an installer could in principle queue more, and appending
    // to the vector being walked is how that becomes an infinite frame.
    std::vector<PendingInstall> batch;
    batch.swap(Pending());

    for (const PendingInstall &pending : batch)
    {
        if (pending.world == nullptr)
            continue;
        (void)SystemCatalog::Instance().Install(*pending.world, pending.names, pending.context);
    }
}

void CancelSystemInstalls(const World &world)
{
    std::erase_if(Pending(), [&world](const PendingInstall &pending) { return pending.world == &world; });
}

} // namespace Assisi::App
