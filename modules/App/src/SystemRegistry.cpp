/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file SystemRegistry.cpp

#include <Assisi/App/SystemRegistry.hpp>

// SystemContext holds World by reference (forward-declared in the header, to
// keep World.hpp's own include of this one acyclic); the activation gate reads
// through it, so the definition is needed here.
#include <Assisi/App/World.hpp>
#include <Assisi/Chiara/Profile.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>

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
                                                  std::string_view phaseName)
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

    // An ordering target this world does not hold is silently no edge.
    //
    // It means the named system exists but this world did not install it, which
    // is ordinary: a level picks its systems one at a time, and "run after the
    // patrol system, if there is one" is a reasonable thing for a system to say.
    // Install that system later and the phase is re-sorted with the edge in
    // place, so the constraint is honoured from then on rather than lost.
    //
    // The other reading — a target no system anywhere declares — cannot reach
    // here: reflectgen refuses it across the whole tree before anything is
    // compiled, which is the only scope where "anywhere" can be asked.
    for (std::size_t i = 0; i < n; ++i)
    {
        for (const std::string &dep : entries[i].after)
        {
            const std::unordered_map<std::string, std::size_t>::const_iterator it =
                nameToIndex.find(dep);
            if (it != nameToIndex.end())
            {
                addEdge(it->second, i);
            }
        }

        for (const std::string &dep : entries[i].before)
        {
            const std::unordered_map<std::string, std::size_t>::const_iterator it =
                nameToIndex.find(dep);
            if (it != nameToIndex.end())
            {
                addEdge(i, it->second);
            }
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
    _addDependency(/*before=*/ false, name);
    return *this;
}

SystemRegistry::SystemHandle &SystemRegistry::SystemHandle::Before(std::string_view name)
{
    _addDependency(/*before=*/ true, name);
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
SystemRegistry::SystemHandle SystemRegistry::Add(Phase<Ctx> &phase,
                                                 std::string_view name,
                                                 std::function<void(Ctx &)> fn,
                                                 bool supportsActiveOnly)
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
    phase.entries.push_back({.name       = std::string(name),
                             .fn         = std::move(fn),
                             .after      = {},
                             .before     = {},
                             .activeOnly = false,
                             .enabled    = true,
                             .ran        = false,
                             .requireAny = {},
                             .chiaraName = Chiara::InternString(name)});
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

namespace
{
// The scene a phase's activation gates are asked about. Both context types carry
// one; they just reach it differently, and an overload here is what lets
// RunPhase take the context alone rather than the context and its own scene.
const ECS::Scene &GateScene(const SystemContext &ctx) { return ctx.world.scene; }
const ECS::Scene &GateScene(const RenderContext &ctx) { return ctx.scene; }

/// Whether @p entry runs this dispatch: not muted, not gated out by the world
/// role, and holding at least one of the components it requires.
///
/// One predicate for both the repeating phases and the one-shot ones. They must
/// agree about what "would run" means: a one-shot marks every entry it reaches
/// as having had its moment, and an entry the two disagreed about would either
/// be marked without running or run twice.
template <typename Entry>
bool ShouldRun(const Entry &entry, bool skipActiveOnly, const ECS::Scene &gateScene)
{
    if (!entry.enabled)
    {
        return false;
    }
    if (skipActiveOnly && entry.activeOnly)
    {
        return false;
    }
    if (entry.requireAny.empty())
    {
        return true;
    }
    // Each id indexes the scene's pool array, so a system whose components are
    // absent costs a load and a compare rather than a call. That is what makes it
    // affordable for a level to name systems a given world may never need.
    for (const Core::Reflect::ComponentId id : entry.requireAny)
    {
        if (gateScene.ComponentCount(id) > 0)
        {
            return true;
        }
    }
    return false;
}
} // namespace

template <typename Ctx>
void SystemRegistry::RunPhase(Phase<Ctx> &phase, std::string_view phaseName, const char *profileName, Ctx &ctx,
                              bool skipActiveOnly)
{
    const ECS::Scene &gateScene = GateScene(ctx);

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

    for (std::size_t i : phase.sorted)
    {
        const typename Phase<Ctx>::Entry &entry = phase.entries[i];
        if (!ShouldRun(entry, skipActiveOnly, gateScene))
        {
            continue;
        }

        ASSISI_PROFILE_SCOPE(entry.chiaraName);
        entry.fn(ctx);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Both tables are indexed by SystemPhase, so they have to be in its order and as
// long as it is — the asserts are what turn adding an enumerator and forgetting a
// name into a build error rather than a phase that profiles as another one.
namespace
{
constexpr const char *kPhaseNames[] = {"Begin",       "Loaded",     "PreUpdate", "FixedUpdate",
                                       "PostFixedUpdate", "Update", "PostUpdate"};
static_assert(std::size(kPhaseNames) == static_cast<std::size_t>(SystemPhase::Count),
              "Every SystemPhase needs a name, in the enum's order.");
} // namespace

std::string_view SystemRegistry::PhaseName(std::size_t gamePhaseIndex)
{
    return kPhaseNames[gamePhaseIndex];
}

const char *SystemRegistry::PhaseProfileName(std::size_t gamePhaseIndex)
{
    return kPhaseNames[gamePhaseIndex];
}

SystemRegistry::SystemHandle SystemRegistry::Register(SystemPhase phase,
                                                      std::string_view name,
                                                      std::function<void(SystemContext &)> fn)
{
    return Add(_gamePhases[Index(phase)], name, std::move(fn), /*supportsActiveOnly=*/ true);
}

SystemRegistry::SystemHandle SystemRegistry::RegisterRender(std::string_view name,
                                                            std::function<void(RenderContext &)> fn)
{
    return Add(_renderPhase, name, std::move(fn), /*supportsActiveOnly=*/ false);
}

void SystemRegistry::Run(SystemPhase phase, SystemContext ctx)
{
    // Pool occupancy, once per world per frame. PreUpdate is the first per-frame
    // phase, so it is where a world's frame begins — emitting from every phase
    // would quadruple the samples without adding anything. The one-shot phases
    // run through RunOnce and emit none of this: they are not a frame.
    //
    // A runtime loop over ComponentRegistry rather than generated code: the
    // registry already enumerates every ACOMP with a stable name, so this stays
    // correct as components are added and needs no codegen to do it. Names are
    // interned once into a static table, since a counter name must outlive the
    // capture and interning takes a lock.
    if (phase == SystemPhase::PreUpdate)
    {
        ASSISI_PROFILE_COUNTER("ecs/entity-count", static_cast<double>(ctx.world.scene.AliveCount()));

        const std::span<const Core::Reflect::ComponentMeta> metas =
            Core::Reflect::ComponentRegistry::Instance().All();
        static const std::vector<const char *> kCounterNames = [&metas]
                                                               {
                                                                   std::vector<const char *> names;
                                                                   names.reserve(metas.size());
                                                                   for (const Core::Reflect::ComponentMeta &meta : metas)
                                                                   {
                                                                       names.push_back(Chiara::InternString("ecs/components/" + meta.name));
                                                                   }
                                                                   return names;
                                                               }();

        // Guard rather than assume: a component registered after the first frame
        // would leave the cached table short, and a mismatched index here would
        // label one pool with another's name.
        const std::size_t counted = std::min(kCounterNames.size(), metas.size());
        for (std::size_t i = 0; i < counted; ++i)
        {
            ASSISI_PROFILE_COUNTER(kCounterNames[i],
                                   static_cast<double>(ctx.world.scene.ComponentCount(metas[i].id)));
        }
    }

    RunPhase(_gamePhases[Index(phase)], PhaseName(Index(phase)), PhaseProfileName(Index(phase)), ctx,
             /*skipActiveOnly=*/ !ctx.isActiveWorld);
}

void SystemRegistry::RunOnce(SystemPhase phase, SystemContext ctx)
{
    Phase<SystemContext> &oncePhase = _gamePhases[Index(phase)];

    // Sorted here as well as in RunPhase: a one-shot's After()/Before() decide
    // which of two Begin systems sees the world first, and an unsorted walk would
    // settle that by registration order.
    if (oncePhase.dirty)
    {
        oncePhase.sorted = TopoSort(oncePhase.entries, PhaseName(Index(phase)));
        oncePhase.dirty  = false;
    }

    ASSISI_PROFILE_SCOPE(PhaseProfileName(Index(phase)));

    const ECS::Scene &gateScene = ctx.world.scene;
    for (const std::size_t i : oncePhase.sorted)
    {
        Phase<SystemContext>::Entry &entry = oncePhase.entries[i];
        if (entry.ran)
        {
            continue;
        }

        // Marked before the eligibility question, not after it: an entry that
        // declines its moment has still had it. Marking only the ones that ran
        // would leave a muted or gated system waiting to fire at whatever
        // unrelated drain next runs this phase.
        entry.ran = true;
        if (!ShouldRun(entry, /*skipActiveOnly=*/ !ctx.isActiveWorld, gateScene))
        {
            continue;
        }

        ASSISI_PROFILE_SCOPE(entry.chiaraName);
        entry.fn(ctx);
    }
}

void SystemRegistry::ClearOnceMarks()
{
    // Only the one-shot phases carry a mark; the repeating ones never read it,
    // so walking them would be work with nothing to undo.
    for (const SystemPhase phase : {SystemPhase::Begin, SystemPhase::Loaded})
    {
        for (Phase<SystemContext>::Entry &entry : _gamePhases[Index(phase)].entries)
        {
            entry.ran = false;
        }
    }
}

void SystemRegistry::RunRender(RenderContext ctx)
{
    RunPhase(_renderPhase, "Render", "Render", ctx, /*skipActiveOnly=*/ false);
}

bool SystemRegistry::Has(std::string_view name) const
{
    for (const Phase<SystemContext> &phase : _gamePhases)
    {
        for (const auto &entry : phase.entries)
        {
            if (entry.name == name)
                return true;
        }
    }
    for (const auto &entry : _renderPhase.entries)
    {
        if (entry.name == name)
            return true;
    }
    return false;
}

void SystemRegistry::SetEnabled(std::string_view name, bool enabled)
{
    // Every phase, not the first hit: Add() warns about a duplicate name but does
    // not refuse it, so a name can reach more than one entry and muting half of
    // them would be the confusing outcome.
    for (Phase<SystemContext> &phase : _gamePhases)
    {
        for (auto &entry : phase.entries)
        {
            if (entry.name == name)
                entry.enabled = enabled;
        }
    }
    for (auto &entry : _renderPhase.entries)
    {
        if (entry.name == name)
            entry.enabled = enabled;
    }
}

bool SystemRegistry::IsEnabled(std::string_view name) const
{
    for (const Phase<SystemContext> &phase : _gamePhases)
    {
        for (const auto &entry : phase.entries)
        {
            if (entry.name == name && !entry.enabled)
                return false;
        }
    }
    for (const auto &entry : _renderPhase.entries)
    {
        if (entry.name == name && !entry.enabled)
            return false;
    }
    return true;
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
