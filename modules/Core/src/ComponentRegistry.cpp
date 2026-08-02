/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ReplicableLimits.hpp>

#include <algorithm>
#include <utility>

namespace Assisi::Core::Reflect
{

ComponentRegistry &ComponentRegistry::Instance()
{
    static ComponentRegistry instance;
    return instance;
}

void ComponentRegistry::Register(ComponentMeta meta)
{
    // Round-6 M4(a). Once an id has been issued the registry is immutable, and a
    // late Register is refused rather than honoured. Two independent reasons, both
    // fatal:
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
    // Appending with a fresh id instead of re-sorting would fix (1) and not (2),
    // so refusing is the only option that keeps both invariants. Every legitimate
    // registration comes from a static initializer and therefore runs before any
    // query; arriving late means a dynamically-loaded module (unsupported) or a
    // genuine bug, so this is loud in debug and a dropped component in release —
    // a component that is absent fails visibly at Find(), whereas renumbering
    // silently mis-maps every id in the process.
    if (_finalized)
    {
        // Log first, assert second: the assert does not return — it aborts (or, under
        // the test handler, throws) — so anything after it is unreachable in debug.
        // The assert message is a string literal and cannot name the component, so
        // this is the only line that says *which* one, and it has to run first to be
        // seen at all.
        Core::Log::Error("ComponentRegistry: refusing late registration of '{}' — the registry is already "
                         "finalized and its ids are in use. The component will not be reflected.",
                         meta.name);
        ASSISI_ASSERT(false, "ComponentRegistry::Register after an id was issued — ids are positions in the "
                             "name-sorted list, so registering now would renumber ids that callers have "
                             "already cached and saved scenes already store");
        return;
    }

    _metas.push_back(std::move(meta));
}

void ComponentRegistry::EnsureFinalized() const
{
    if (_finalized)
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
    for (ComponentId i = 0; i < _metas.size(); ++i)
    {
        _metas[i].id = i;
        _idByType.emplace(_metas[i].typeIndex, i);
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

    // The aggregator-vs-reality check. reflectgen counts ACOMP(replicable) types
    // across the tree at build time and sizes ComponentMask from it, so this can
    // only trip if a module's headers escaped that scan — the single way the
    // count can be wrong. Failing loudly here beats writing exclusion bits past
    // the end of the mask, which would be silent and would corrupt an unrelated
    // component's policy.
    if (_replicable.size() > kReplicableComponentCount)
    {
        Core::Log::Error("ComponentRegistry: {} replicable components registered but ComponentMask was sized "
                         "for {}. A module's headers are missing from the reflectgen scan — every reflected "
                         "header must be passed to assisi_reflect().",
                         _replicable.size(), kReplicableComponentCount);
    }

    _finalized = true;
}

std::span<const ComponentMeta *const> ComponentRegistry::ReplicableComponents() const
{
    EnsureFinalized();
    return _replicable;
}

std::size_t ComponentRegistry::ReplicableOrdinalOf(ComponentId id) const
{
    EnsureFinalized();
    return id < _replicableOrdinal.size() ? _replicableOrdinal[id] : kInvalidOrdinal;
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
    return id < _metas.size() ? &_metas[id] : nullptr;
}

ComponentId ComponentIdOfType(std::type_index type)
{
    return ComponentRegistry::Instance().IdOf(type);
}

} // namespace Assisi::Core::Reflect