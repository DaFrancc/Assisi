/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>

#include <Assisi/Core/Logger.hpp>

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
    _metas.push_back(std::move(meta));
    // A new type shifts the alphabetical ordering, so ids must be re-derived on
    // the next query. During normal startup every Register runs before any
    // query, so this finalizes exactly once.
    _finalized = false;
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
    for (ComponentId i = 0; i < _metas.size(); ++i)
    {
        _metas[i].id = i;
        _idByType.emplace(_metas[i].typeIndex, i);
        if (_metas[i].serializable)
            _serializable.push_back(&_metas[i]);
    }

    _finalized = true;
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