/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ReplicableLimits.hpp>

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
/// Guards the one-time finalization below. A file static rather than a member so
/// <mutex> stays out of a header most of the engine includes; the registry is a
/// singleton, and a second instance in a test sharing this lock is merely
/// coarser, never wrong.
std::mutex g_finalizeMutex;

using Assisi::Core::Reflect::ComponentId;
using Assisi::Core::Reflect::ComponentMeta;
using Assisi::Core::Reflect::kInvalidComponentId;

/// The id of the component named @p name in the name-sorted @p metas, or
/// kInvalidComponentId. Find() cannot be used while finalizing: it finalizes.
ComponentId IdInSorted(const std::vector<ComponentMeta> &metas, std::string_view name)
{
    std::vector<ComponentMeta>::const_iterator it =
        std::lower_bound(metas.begin(), metas.end(), name,
                         [](const ComponentMeta &meta, std::string_view wanted) { return meta.name < wanted; });
    if (it == metas.end() || it->name != name)
    {
        return kInvalidComponentId;
    }
    return it->id;
}

/// Resolves @p names to ids. A name this program does not register is logged and
/// skipped: the build-wide check already refused names that exist nowhere, so
/// one missing here is a component from a module this program does not link.
std::vector<ComponentId> ResolveNames(const std::vector<ComponentMeta> &metas, const ComponentMeta &owner,
                                      const std::vector<std::string> &names, std::string_view rule)
{
    std::vector<ComponentId> ids;
    for (const std::string &name : names)
    {
        const ComponentId id = IdInSorted(metas, name);
        if (id == kInvalidComponentId)
        {
            Assisi::Core::Log::Error("ComponentRegistry: '{}' {} '{}', which this program does not register - "
                                     "the rule is dropped.",
                                     owner.name, rule, name);
            continue;
        }
        ids.push_back(id);
    }
    return ids;
}

void SortUnique(std::vector<ComponentId> &ids)
{
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}

/// Fills every meta's required (transitively), excluded (from either side) and
/// requiredBy lists from the names reflectgen recorded. Ids must be assigned.
void ResolveRules(std::vector<ComponentMeta> &metas)
{
    std::vector<std::vector<ComponentId>> direct(metas.size());
    for (std::size_t i = 0; i < metas.size(); ++i)
    {
        direct[i] = ResolveNames(metas, metas[i], metas[i].requiredNames, "requires");
        for (const ComponentId excluded : ResolveNames(metas, metas[i], metas[i].excludedNames, "excludes"))
        {
            // .value: array index into metas.
            metas[i].excluded.push_back(excluded);
            metas[excluded.value].excluded.push_back(metas[i].id);
        }
    }

    // Walked rather than recursed, and guarded by `seen`, so a cycle the build
    // check somehow missed ends the walk instead of the stack.
    for (std::size_t i = 0; i < metas.size(); ++i)
    {
        std::vector<bool> seen(metas.size(), false);
        seen[i] = true;
        std::vector<ComponentId> pending = direct[i];
        while (!pending.empty())
        {
            const ComponentId next = pending.back();
            pending.pop_back();
            // .value: array index into seen/direct.
            if (seen[next.value])
            {
                continue;
            }
            seen[next.value] = true;
            metas[i].required.push_back(next);
            pending.insert(pending.end(), direct[next.value].begin(), direct[next.value].end());
        }
    }

    for (ComponentMeta &meta : metas)
    {
        SortUnique(meta.required);
        SortUnique(meta.excluded);
        for (const ComponentId requirement : meta.required)
        {
            // .value: array index into metas.
            metas[requirement.value].requiredBy.push_back(meta.id);
        }
    }
}
} // namespace

namespace Assisi::Core::Reflect
{

ComponentRegistry &ComponentRegistry::Instance()
{
    static ComponentRegistry instance;
    return instance;
}

void ComponentRegistry::Register(ComponentMeta meta)
{
    // Once an id has been issued the registry is immutable, and a late Register is
    // refused rather than honoured. Two independent reasons:
    //
    //   1. Ids are positions in the name-sorted list — that ordering is what makes
    //      them reproducible across builds and machines, since static-init order
    //      across translation units is unspecified. Adding a name re-sorts, so an
    //      alphabetically-earlier late arrival shifts ids that callers already
    //      cached (ComponentIdOf<T> memoises in a function-local static) and that
    //      saved scenes already store.
    //   2. ById()/All() hand out pointers *into* _metas, and _serializable holds
    //      them too. Growing the vector would dangle every one of them.
    //
    // Appending with a fresh id instead of re-sorting fixes (1) and not (2), so
    // refusing is the only option that keeps both. Legitimate registrations come
    // from static initializers and run before any query; a late one means a
    // dynamically-loaded module (unsupported) or a bug. Loud in debug, a dropped
    // component in release — an absent component fails visibly at Find(), whereas
    // renumbering silently mis-maps every id in the process.
    if (_finalized.load(std::memory_order_acquire))
    {
        // Log first, assert second: the assert does not return — it aborts (or, under
        // the test handler, throws) — so anything after it is unreachable in debug.
        // The assert message is a string literal and cannot name the component, so
        // this is the only line that says *which* one, and it has to run first to be
        // seen at all.
        Core::Log::Error("ComponentRegistry: refusing late registration of '{}' - the registry is already "
                         "finalized and its ids are in use. The component will not be reflected.",
                         meta.name);
        ASSISI_ASSERT(false, "ComponentRegistry::Register after an id was issued - ids are positions in the "
                      "name-sorted list, so registering now would renumber ids that callers have "
                      "already cached and saved scenes already store");
        return;
    }

    _metas.push_back(std::move(meta));
}

void ComponentRegistry::EnsureFinalized() const
{
    // Acquire, and it is the whole point: a thread that sees the flag set must
    // also see every table the finalizing thread built below — the sort, the id
    // assignment, and the _serializable / _replicable vectors. The first ask can
    // come from a worker (async travel deserializes off the main thread and walks
    // this registry there) while the main thread asks for the same thing, so a
    // plain bool here is a data race.
    if (_finalized.load(std::memory_order_acquire))
        return;

    const std::lock_guard lock(g_finalizeMutex);

    // Re-checked under the lock: the loser of the race would otherwise sort and
    // re-id a table the winner has already handed pointers into.
    if (_finalized.load(std::memory_order_relaxed))
        return;

    std::sort(_metas.begin(), _metas.end(),
              [](const ComponentMeta &a, const ComponentMeta &b) { return a.name < b.name; });

    // Reject duplicate component names. Two components sharing a name collide in
    // saved files (Load's Find picks one) and produce type-confused pool casts, so
    // keep the first of each name and drop the rest — loudly, since a duplicate is a
    // build-time mistake (usually the same ACOMP struct reflected twice). Dropping
    // here (after the sort groups equal names) keeps ids dense over the survivors.
    const auto lastUnique = std::unique(
        _metas.begin(), _metas.end(),
        [](const ComponentMeta &kept, const ComponentMeta &dup)
        {
            if (kept.name == dup.name)
            {
                Core::Log::Error("ComponentRegistry: duplicate component name '{}' - keeping the first "
                                 "registration and dropping the duplicate.",
                                 dup.name);
                return true;
            }
            return false;
        });
    _metas.erase(lastUnique, _metas.end());

    _idByType.clear();
    _idByType.reserve(_metas.size());
    _serializable.clear();
    _replicable.clear();
    _replicableOrdinal.assign(_metas.size(), kInvalidOrdinal);
    // Raw counter, not a ComponentId — this is the loop that walks the
    // name-sorted table and hands the position itself out as the id, the one
    // place a plain count becomes an identity. Every other use of `i` below is
    // an array index (into _metas/_replicableOrdinal), not an id operation.
    for (std::uint32_t i = 0; i < _metas.size(); ++i)
    {
        _metas[i].id = ComponentId{i}; // the one place a raw counter becomes an id
        _idByType.emplace(_metas[i].typeIndex, ComponentId{i});
        if (_metas[i].serializable)
            _serializable.push_back(&_metas[i]);
        // Ordinal is position among the replicable types, in the same ascending
        // id order — so the sequence and the index agree by construction.
        if (_metas[i].replicable)
        {
            _replicableOrdinal[i] = _replicable.size();
            _replicable.push_back(&_metas[i]);
        }
    }

    ResolveRules(_metas);

    // The aggregator-vs-reality check. reflectgen counts ACOMP(replicable) types
    // across the tree at build time and sizes ComponentMask from it, so this can
    // only trip if a module's headers escaped that scan — the single way the
    // count can be wrong. Failing loudly here beats writing exclusion bits past
    // the end of the mask, which would be silent and would corrupt an unrelated
    // component's policy.
    if (_replicable.size() > kReplicableComponentCount)
    {
        Core::Log::Error("ComponentRegistry: {} replicable components registered but ComponentMask was sized "
                         "for {}. A module's headers are missing from the reflectgen scan - every reflected "
                         "header must be passed to assisi_reflect().",
                         _replicable.size(), kReplicableComponentCount);
    }

    // Release, pairing with the acquire above: everything written in this
    // function happens-before any thread that observes the flag set.
    _finalized.store(true, std::memory_order_release);
}

std::span<const ComponentMeta *const> ComponentRegistry::ReplicableComponents() const
{
    EnsureFinalized();
    return _replicable;
}

std::size_t ComponentRegistry::ReplicableOrdinalOf(ComponentId id) const
{
    EnsureFinalized();
    // .value: array index into _replicableOrdinal.
    return id.value < _replicableOrdinal.size() ? _replicableOrdinal[id.value] : kInvalidOrdinal;
}

const ComponentMeta *ComponentRegistry::Find(std::string_view name) const
{
    EnsureFinalized();
    // _metas is sorted by name, so a binary search is exact and cheap.
    auto it = std::lower_bound(_metas.begin(), _metas.end(), name,
                               [](const ComponentMeta &m, std::string_view n) { return m.name < n; });
    if (it != _metas.end() && it->name == name)
        return &*it;
    return nullptr;
}

std::span<const ComponentMeta> ComponentRegistry::All() const
{
    EnsureFinalized();
    return _metas;
}

std::span<const ComponentMeta *const> ComponentRegistry::SerializableComponents() const
{
    EnsureFinalized();
    return _serializable;
}

std::size_t ComponentRegistry::Count() const
{
    return _metas.size();
}

ComponentId ComponentRegistry::IdOf(std::type_index type) const
{
    EnsureFinalized();
    auto it = _idByType.find(type);
    return it != _idByType.end() ? it->second : kInvalidComponentId;
}

ComponentId ComponentRegistry::IdOf(std::string_view name) const
{
    const ComponentMeta *meta = Find(name);
    return meta ? meta->id : kInvalidComponentId;
}

const ComponentMeta *ComponentRegistry::ById(ComponentId id) const
{
    EnsureFinalized();
    // .value: array index into _metas.
    return id.value < _metas.size() ? &_metas[id.value] : nullptr;
}

ComponentId ComponentIdOfType(std::type_index type)
{
    return ComponentRegistry::Instance().IdOf(type);
}

} // namespace Assisi::Core::Reflect