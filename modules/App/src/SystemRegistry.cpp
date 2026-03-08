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
        for (const auto &dep : entries[i].after)
        {
            auto it = nameToIndex.find(dep);
            if (it == nameToIndex.end())
            {
                Core::Log::Error(
                    "SystemRegistry({}): '{}' declares After(\"{}\") but \"{}\" is not registered.",
                    phaseName, entries[i].name, dep, dep);
                continue;
            }
            addEdge(it->second, i);
        }

        for (const auto &dep : entries[i].before)
        {
            auto it = nameToIndex.find(dep);
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
        Core::Log::Fatal(
            "SystemRegistry({}): cycle detected. Check After()/Before() declarations.", phaseName);
        sorted.clear();
    }

    return sorted;
}

// Explicit instantiations — keeps the template definition out of the header.
template std::vector<std::size_t>
SystemRegistry::TopoSort<SystemRegistry::GameEntry>(const std::vector<GameEntry> &,
                                                     std::string_view);
template std::vector<std::size_t>
SystemRegistry::TopoSort<SystemRegistry::RenderEntry>(const std::vector<RenderEntry> &,
                                                       std::string_view);

// ---------------------------------------------------------------------------
// SystemHandle
// ---------------------------------------------------------------------------

SystemRegistry::SystemHandle &SystemRegistry::SystemHandle::After(std::string_view name)
{
    if (_isRender)
    {
        _registry->_renderEntries[_entryIndex].after.emplace_back(name);
        _registry->_renderDirty = true;
    }
    else
    {
        _registry->_entries[_phaseIndex][_entryIndex].after.emplace_back(name);
        _registry->_dirty[_phaseIndex] = true;
    }
    return *this;
}

SystemRegistry::SystemHandle &SystemRegistry::SystemHandle::Before(std::string_view name)
{
    if (_isRender)
    {
        _registry->_renderEntries[_entryIndex].before.emplace_back(name);
        _registry->_renderDirty = true;
    }
    else
    {
        _registry->_entries[_phaseIndex][_entryIndex].before.emplace_back(name);
        _registry->_dirty[_phaseIndex] = true;
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Register
// ---------------------------------------------------------------------------

SystemRegistry::SystemHandle SystemRegistry::Register(SystemPhase                          phase,
                                                       std::string_view                     name,
                                                       std::function<void(SystemContext &)> fn)
{
    const std::size_t pi = Index(phase);
    const std::size_t ei = _entries[pi].size();
    _entries[pi].push_back({std::string(name), std::move(fn), {}, {}});
    _dirty[pi] = true;
    return SystemHandle(this, /*isRender=*/false, pi, ei);
}

SystemRegistry::SystemHandle SystemRegistry::Register(SystemPhase                          phase,
                                                       std::string_view                     name,
                                                       std::function<void(RenderContext &)> fn)
{
    const std::size_t ei = _renderEntries.size();
    _renderEntries.push_back({std::string(name), std::move(fn), {}, {}});
    _renderDirty = true;
    return SystemHandle(this, /*isRender=*/true, /*phaseIndex=*/0, ei);
}

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------

void SystemRegistry::Run(SystemPhase phase, SystemContext ctx)
{
    const std::size_t pi = Index(phase);
    if (_dirty[pi])
        SortPhase(pi);

    for (std::size_t i : _sorted[pi])
        _entries[pi][i].fn(ctx);
}

void SystemRegistry::Run(SystemPhase, RenderContext ctx)
{
    if (_renderDirty)
        SortRender();

    for (std::size_t i : _renderSorted)
        _renderEntries[i].fn(ctx);
}

// ---------------------------------------------------------------------------
// Sort
// ---------------------------------------------------------------------------

void SystemRegistry::SortPhase(std::size_t pi)
{
    static constexpr std::string_view kNames[] = {"PreUpdate", "FixedUpdate", "Update",
                                                   "PostUpdate"};
    _sorted[pi]  = TopoSort(_entries[pi], kNames[pi]);
    _dirty[pi]   = false;
}

void SystemRegistry::SortRender()
{
    _renderSorted = TopoSort(_renderEntries, "Render");
    _renderDirty  = false;
}

} // namespace Assisi::App