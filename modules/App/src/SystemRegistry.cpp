/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file SystemRegistry.cpp

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Logger.hpp>

#include <set>
#include <unordered_map>

namespace Assisi::App
{

// ---------------------------------------------------------------------------
// TopoSort — shared by game and render phases
// ---------------------------------------------------------------------------

template <typename Entry>
std::vector<std::size_t> SystemRegistry::TopoSort(const std::vector<Entry> &entries,
                                                   std::string_view          phaseName)
{
    const std::size_t n = entries.size();

    std::vector<std::size_t> sorted;
    sorted.reserve(n);

    if (n == 0)
        return sorted;

    // name → index
    std::unordered_map<std::string, std::size_t> nameToIndex;
    nameToIndex.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        nameToIndex.emplace(entries[i].name, i);

    // Build adjacency list and in-degree counts.
    // Edge from→to: 'from' executes before 'to'.
    std::vector<std::vector<std::size_t>> adj(n);
    std::vector<int>                      inDegree(n, 0);

    // Deduplicate edges so A.Before("B") + B.After("A") doesn't double-count.
    std::set<std::pair<std::size_t, std::size_t>> seen;

    auto addEdge = [&](std::size_t from, std::size_t to)
    {
        if (seen.insert({from, to}).second)
        {
            adj[from].push_back(to);
            ++inDegree[to];
        }
    };

    for (std::size_t i = 0; i < n; ++i)
    {
        for (const std::string &dep : entries[i].after)
        {
            const std::unordered_map<std::string, std::size_t>::const_iterator it =
                nameToIndex.find(dep);
            if (it == nameToIndex.end())
            {
                Core::Log::Error(
                    "SystemRegistry({}): '{}' declares After(\"{}\") but \"{}\" is not registered.",
                    phaseName, entries[i].name, dep, dep);
                continue;
            }
            addEdge(it->second, i);
        }

        for (const std::string &dep : entries[i].before)
        {
            const std::unordered_map<std::string, std::size_t>::const_iterator it =
                nameToIndex.find(dep);
            if (it == nameToIndex.end())
            {
                Core::Log::Error(
                    "SystemRegistry({}): '{}' declares Before(\"{}\") but \"{}\" is not registered.",
                    phaseName, entries[i].name, dep, dep);
                continue;
            }
            addEdge(i, it->second);
        }
    }

    // Kahn's algorithm.
    // Iterating by index preserves registration order within each "layer",
    // giving deterministic output without extra sorting.
    std::vector<std::size_t> ready;
    ready.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        if (inDegree[i] == 0)
            ready.push_back(i);

    std::size_t head = 0;
    while (head < ready.size())
    {
        const std::size_t curr = ready[head++];
        sorted.push_back(curr);
        for (std::size_t next : adj[curr])
            if (--inDegree[next] == 0)
                ready.push_back(next);
    }

    if (sorted.size() != n)
    {
        // A cycle is a programmer error, but silently running zero systems for
        // the phase is worse than running them in a defined-but-arbitrary order.
        // Fall back to registration order so nothing is dropped, and log loudly.
        Core::Log::Error("SystemRegistry({}): dependency cycle detected in After()/Before() "
                         "declarations — falling back to registration order.",
                         phaseName);
        sorted.clear();
        for (std::size_t i = 0; i < n; ++i)
            sorted.push_back(i);
    }

    return sorted;
}

// ---------------------------------------------------------------------------
// SystemHandle — one definition, type-erased over the context
// ---------------------------------------------------------------------------

SystemRegistry::SystemHandle &SystemRegistry::SystemHandle::After(std::string_view name)
{
    _addDependency(/*before=*/false, name);
    return *this;
}

SystemRegistry::SystemHandle &SystemRegistry::SystemHandle::Before(std::string_view name)
{
    _addDependency(/*before=*/true, name);
    return *this;
}

// ---------------------------------------------------------------------------
// Add — append to a phase and hand back a slot-bound handle
// ---------------------------------------------------------------------------

template <typename Ctx>
SystemRegistry::SystemHandle SystemRegistry::Add(Phase<Ctx>               &phase,
                                                 std::string_view          name,
                                                 std::function<void(Ctx &)> fn)
{
    const std::size_t entryIndex = phase.entries.size();
    phase.entries.push_back({std::string(name), std::move(fn), {}, {}});
    phase.dirty = true;

    // Capture the phase and slot index (not a pointer to the Entry): the entries
    // vector may reallocate before the handle is used, but indexing stays valid.
    Phase<Ctx> *phasePtr = &phase;
    return SystemHandle(
        [phasePtr, entryIndex](bool before, std::string_view depName)
        {
            typename Phase<Ctx>::Entry &entry = phasePtr->entries[entryIndex];
            (before ? entry.before : entry.after).emplace_back(depName);
            phasePtr->dirty = true;
        });
}

// ---------------------------------------------------------------------------
// RunPhase — sort-on-demand then dispatch
// ---------------------------------------------------------------------------

template <typename Ctx>
void SystemRegistry::RunPhase(Phase<Ctx> &phase, std::string_view phaseName, Ctx &ctx)
{
    if (phase.dirty)
    {
        phase.sorted = TopoSort(phase.entries, phaseName);
        phase.dirty  = false;
    }

    for (std::size_t i : phase.sorted)
        phase.entries[i].fn(ctx);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string_view SystemRegistry::PhaseName(std::size_t gamePhaseIndex)
{
    static constexpr std::string_view kNames[] = {"PreUpdate", "FixedUpdate", "Update",
                                                   "PostUpdate"};
    return kNames[gamePhaseIndex];
}

SystemRegistry::SystemHandle SystemRegistry::Register(SystemPhase                          phase,
                                                      std::string_view                     name,
                                                      std::function<void(SystemContext &)> fn)
{
    return Add(_gamePhases[Index(phase)], name, std::move(fn));
}

SystemRegistry::SystemHandle SystemRegistry::RegisterRender(std::string_view name,
                                                            std::function<void(RenderContext &)> fn)
{
    return Add(_renderPhase, name, std::move(fn));
}

void SystemRegistry::Run(SystemPhase phase, SystemContext ctx)
{
    RunPhase(_gamePhases[Index(phase)], PhaseName(Index(phase)), ctx);
}

void SystemRegistry::RunRender(RenderContext ctx)
{
    RunPhase(_renderPhase, "Render", ctx);
}

} // namespace Assisi::App
