/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/SystemCatalog.hpp>

#include <Assisi/App/World.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace Assisi::App
{

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
    std::vector<const SystemDefinition *> resolved;
    if (!Resolve(names, resolved, context))
        return false;
    ApplyResolved(world, resolved);
    return true;
}

bool SystemCatalog::Resolve(std::span<const std::string> names, std::vector<const SystemDefinition *> &out,
                            std::string_view context) const
{
    // Resolve everything first. A half-installed world runs and looks nearly
    // right, which is worse than a refused load — and the name that failed is the
    // one thing the author needs to be told.
    std::vector<const SystemDefinition *> &resolved = out;
    resolved.clear();
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
    return ok;
}

void SystemCatalog::ApplyResolved(World &world, std::span<const SystemDefinition *const> resolved) const
{
    for (const SystemDefinition *definition : resolved)
    {
        // A union, not a concatenation: two nested blueprints both naming Bounce
        // install it once, and a spawn into a world that already has it costs a
        // lookup.
        if (world.systems.Has(definition->name))
            continue;

        // The two halves differ only in how the system is registered. A render
        // system has no phase to pass and no meaning for activeWorldOnly — it
        // runs once, for the world being drawn — so those two are the whole of
        // what the branch is for. The ordering constraints apply to both, and
        // reflectgen has already refused an `after` naming nothing.
        SystemRegistry::SystemHandle handle =
            definition->isRender ? world.systems.RegisterRender(definition->name, definition->runRender)
                                 : world.systems.Register(definition->phase, definition->name, definition->run);

        for (const std::string &target : definition->after)
        {
            handle.After(target);
        }
        for (const std::string &target : definition->before)
        {
            handle.Before(target);
        }
        if (definition->activeWorldOnly && !definition->isRender)
        {
            handle.ActiveWorldOnly();
        }
    }
}

void QueueSystemInstall(World &world, std::span<const std::string> names, std::string_view context)
{
    if (names.empty())
        return;

    World::PendingSystems &pending = world.pendingSystems;

    // The first spawn to open the queue owns the error message. Arbitrary only
    // when several are equally to blame, and better than the alternative of
    // keeping a context per name for a diagnostic that names one file.
    if (pending.names.empty())
        pending.context.assign(context);

    // A union, so a hundred bullets spawned in one frame leave one name rather
    // than a hundred.
    for (const std::string &name : names)
    {
        if (std::find(pending.names.begin(), pending.names.end(), name) == pending.names.end())
            pending.names.push_back(name);
    }
}

void DrainSystemInstalls(SystemContext ctx)
{
    World &world = ctx.world;
    if (world.pendingSystems.names.empty())
        return;

    // Moved out first: an installer could in principle queue more, and appending
    // to the vector being walked is how that becomes an infinite frame.
    const World::PendingSystems batch = std::exchange(world.pendingSystems, World::PendingSystems{});
    (void)SystemCatalog::Instance().Install(world, batch.names, batch.context);

    // The one-shot phases this world has already passed, replayed for whatever
    // just arrived. Every entry that was here before is marked, so these run the
    // new systems alone — no name filtering, and a blueprint spawned into a
    // running world gets the same start its level would have given it.
    //
    // Nothing to replay for a world that has not begun: it will begin later and
    // run all of them at once.
    if (world.start == StartProgress::NotBegun)
    {
        return;
    }
    world.systems.RunOnce(SystemPhase::Begin, ctx);
    if (world.start == StartProgress::Loaded)
    {
        // A spawn into a settled world is settled by definition of the world, not
        // of the spawn: its own mesh may still be streaming. Per-instance loading
        // is a different feature, and this phase does not pretend to be it.
        world.systems.RunOnce(SystemPhase::Loaded, ctx);
    }
}

bool LevelSystemsAreDeclared(std::string_view virtualPath)
{
    const auto wanted = Runtime::SceneSerializer::ReadLevelSystems(virtualPath);
    if (!wanted)
        return false;

    // Every offender, not just the first: a file with three bad names should
    // need one run to fix, not three.
    bool ok = true;
    for (const std::string &name : *wanted)
    {
        if (SystemCatalog::Instance().Find(name) != nullptr)
            continue;
        Core::Log::Error("'{}' names system '{}', which this build does not declare.", virtualPath, name);
        ok = false;
    }
    return ok;
}

std::map<std::string, int32_t, std::less<>> BlueprintSystemCounts(const Runtime::InstanceTable &instances)
{
    std::map<std::string, int32_t, std::less<>> counts;

    // Distinct sources first: the definition cache is keyed by path, so this is
    // about not counting one blueprint once per copy of it in the level.
    std::vector<std::string_view> sources;
    for (const auto &[id, row] : instances.All())
    {
        (void)id;
        if (!row->authored)
        {
            continue;
        }
        if (std::find(sources.begin(), sources.end(), row->source) != sources.end())
        {
            continue;
        }
        sources.push_back(row->source);
    }

    for (const std::string_view source : sources)
    {
        const Runtime::BlueprintResult definition = Runtime::GetBlueprintDefinition(source);
        if (!definition)
        {
            continue; // Unreadable, and whatever placed it has already said so.
        }
        for (const std::string &name : (*definition)->systems)
        {
            ++counts[name];
        }
    }
    return counts;
}

std::map<std::string, int32_t, std::less<>> RequiredSystemCounts(const World &world)
{
    std::map<std::string, int32_t, std::less<>> counts = BlueprintSystemCounts(world.instances);
    for (const World::Screen &held : world.screenStack)
    {
        for (const std::string &name : held.systems)
        {
            ++counts[name];
        }
    }
    return counts;
}

} // namespace Assisi::App
