/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file SystemRegistry.cpp

#include <Assisi/App/SystemRegistry.hpp>

// SystemContext holds World by reference (forward-declared in the header, to
// keep World.hpp's own include of this one acyclic); the activation gate reads
// through it, so the definition is needed here.
#include <Assisi/App/World.hpp>
#include <Assisi/Chiara/Profile.hpp>
#include <Assisi/Core/Logger.hpp>

#include <cstdint>
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
    std::vector<int32_t>                  inDegree(n, 0);

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

SystemRegistry::SystemHandle &SystemRegistry::SystemHandle::ActiveWorldOnly()
{
    if (!_setActiveOnly)
    {
        // Render systems run for the world being drawn and nothing else, so the
        // constraint is already implied — a call here means the author expected a
        // gate that does not exist in this phase.
        Core::Log::Error("SystemRegistry: ActiveWorldOnly() is meaningless on a render system "
                         "and was ignored.");
        return *this;
    }
    _setActiveOnly();
    return *this;
}

void SystemRegistry::SystemHandle::Require(Core::Reflect::ComponentId id)
{
    if (id == Core::Reflect::kInvalidComponentId)
    {
        // An unreflected type has no pool and would gate the system off forever.
        // Fail loud: silently never running is the worst outcome here.
        Core::Log::Error("SystemRegistry: RequireAny() names a type with no ComponentId — it is "
                         "not registered with the reflection system (ACOMP). Ignoring it, so the "
                         "system stays eligible.");
        return;
    }
    _addRequirement(id);
}

// ---------------------------------------------------------------------------
// Add — append to a phase and hand back a slot-bound handle
// ---------------------------------------------------------------------------

template <typename Ctx>
SystemRegistry::SystemHandle SystemRegistry::Add(Phase<Ctx>               &phase,
                                                 std::string_view          name,
                                                 std::function<void(Ctx &)> fn,
                                                 bool                      supportsActiveOnly)
{
    // Duplicate names make After()/Before() ambiguous: TopoSort's nameToIndex
    // keeps only the first entry per name, so every edge targeting this name binds
    // to that first registration and the later system becomes unreachable by
    // dependency. Log loudly rather than corrupt the graph silently. The system
    // still runs (in registration order) — nothing is dropped, matching the
    // cycle-fallback philosophy above.
    for (const typename Phase<Ctx>::Entry &existing : phase.entries)
    {
        if (existing.name == name)
        {
            Core::Log::Error("SystemRegistry: a system named \"{}\" is already registered in this "
                             "phase; After()/Before() referring to \"{}\" will bind only to the "
                             "first one. Give the systems distinct names.",
                             name, name);
            break;
        }
    }

    const std::size_t entryIndex = phase.entries.size();
    phase.entries.push_back({std::string(name), std::move(fn), {}, {}, /*activeOnly=*/false, {},
                             Chiara::InternString(name)});
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
        },
        supportsActiveOnly ? SystemHandle::SetActiveOnly(
                                 [phasePtr, entryIndex]
                                 { phasePtr->entries[entryIndex].activeOnly = true; })
                           : SystemHandle::SetActiveOnly{},
        [phasePtr, entryIndex](Core::Reflect::ComponentId id)
        { phasePtr->entries[entryIndex].requireAny.push_back(id); });
}

// ---------------------------------------------------------------------------
// RunPhase — sort-on-demand then dispatch
// ---------------------------------------------------------------------------

template <typename Ctx>
void SystemRegistry::RunPhase(Phase<Ctx> &phase, std::string_view phaseName, const char *profileName, Ctx &ctx,
                              bool skipActiveOnly, const ECS::Scene &gateScene)
{
    // This is the chokepoint that makes instrumentation feel automatic: a scope
    // here and one per entry means every system ever written is profiled with no
    // further work, which is how engines with "magic" coverage actually get it —
    // dense framework chokepoints, not per-function reflection.
    ASSISI_PROFILE_SCOPE(profileName);

    if (phase.dirty)
    {
        phase.sorted = TopoSort(phase.entries, phaseName);
        phase.dirty  = false;
    }

    // Whether a gated system has anything to work on. Cheap enough to pay every
    // frame for every system: each id is an index into the scene's pool array,
    // so a system whose components are absent costs a load and a compare rather
    // than a call. That is what makes it affordable for a profile to install
    // systems a given world may never need.
    const auto eligible = [&gateScene](const typename Phase<Ctx>::Entry &entry)
    {
        if (entry.requireAny.empty())
            return true;
        for (const Core::Reflect::ComponentId id : entry.requireAny)
        {
            if (gateScene.ComponentCount(id) > 0)
                return true;
        }
        return false;
    };

    for (std::size_t i : phase.sorted)
    {
        const typename Phase<Ctx>::Entry &entry = phase.entries[i];
        if (skipActiveOnly && entry.activeOnly)
            continue;
        if (!eligible(entry))
            continue;

        ASSISI_PROFILE_SCOPE(entry.chiaraName);
        entry.fn(ctx);
    }
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

const char *SystemRegistry::PhaseProfileName(std::size_t gamePhaseIndex)
{
    static constexpr const char *kNames[] = {"PreUpdate", "FixedUpdate", "Update", "PostUpdate"};
    return kNames[gamePhaseIndex];
}

SystemRegistry::SystemHandle SystemRegistry::Register(SystemPhase                          phase,
                                                      std::string_view                     name,
                                                      std::function<void(SystemContext &)> fn)
{
    return Add(_gamePhases[Index(phase)], name, std::move(fn), /*supportsActiveOnly=*/true);
}

SystemRegistry::SystemHandle SystemRegistry::RegisterRender(std::string_view name,
                                                            std::function<void(RenderContext &)> fn)
{
    return Add(_renderPhase, name, std::move(fn), /*supportsActiveOnly=*/false);
}

void SystemRegistry::Run(SystemPhase phase, SystemContext ctx)
{
    RunPhase(_gamePhases[Index(phase)], PhaseName(Index(phase)), PhaseProfileName(Index(phase)), ctx,
             /*skipActiveOnly=*/!ctx.isActiveWorld, ctx.world.scene);
}

void SystemRegistry::RunRender(RenderContext ctx)
{
    RunPhase(_renderPhase, "Render", "Render", ctx, /*skipActiveOnly=*/false, ctx.scene);
}

void SystemRegistry::Clear()
{
    for (Phase<SystemContext> &phase : _gamePhases)
    {
        phase.entries.clear();
        phase.sorted.clear();
        phase.dirty = false; // nothing to sort
    }
    _renderPhase.entries.clear();
    _renderPhase.sorted.clear();
    _renderPhase.dirty = false;
}

} // namespace Assisi::App
