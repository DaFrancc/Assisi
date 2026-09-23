/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SparseSet.hpp
/// @brief Cache-friendly component storage for the ECS.
///
/// Maps entity indices to components using a sparse array for O(1) lookup
/// and a dense packed array for cache-friendly iteration.
///
/// Layout:
///   sparse[entity.index]  → position in dense array (INVALID if not present)
///   dense[pos]            → component value
///   entities[pos]         → entity index that owns dense[pos]
///
/// Remove() swaps the target element with the last one and pops, keeping
/// the dense array gap-free at all times.

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

#include <Assisi/ECS/Entity.hpp>

namespace Assisi::ECS
{

template <typename T> struct SparseSet
{

    /// @brief Sentinel stored in the sparse array for slots with no component.
    static constexpr uint32_t Invalid = UINT32_MAX;

    /// @brief Adds a component for the given entity.
    ///
    /// @return Pointer to the new component, or nullptr if the add was rejected:
    ///         the entity's index slot is already occupied, either by this same
    ///         entity (duplicate) or by a different generation (a stale handle
    ///         whose slot was reused). Both mean the component was not added and
    ///         the caller can do nothing different about the distinction.
    [[nodiscard]] T *Add(Entity entity, T component = {})
    {
        /* Reject the null/sentinel index: entity.index + 1 below would overflow
           to 0, resize the sparse array to empty, and then the write past it is
           out of bounds.  Scene::Add's IsAlive gate already rejects NullEntity,
           but a direct SparseSet user has no such guard. */
        if (entity.index == InvalidEntityIndex)
            return nullptr;

        /* Any occupied slot is a rejection.  Has() alone won't catch the stale
           case — it matches the full handle, so a stale handle whose slot was
           reused by a newer generation would slip past and overwrite the live
           occupant's sparse entry, orphaning its dense slot and desyncing the
           set.  Inspect the slot directly instead. */
        if (entity.index < _sparse.size())
        {
            if (_sparse[entity.index] != Invalid)
                return nullptr;
        }
        else
        {
            _sparse.resize(entity.index + 1, Invalid);
        }

        _sparse[entity.index] = static_cast<uint32_t>(_dense.size());
        _entities.push_back(entity);
        T *added = &_dense.emplace_back(std::move(component));
        if (_tracksChanges)
            _changeTicks.push_back(0); // parallel to _dense; Scene stamps it right after Add
        BumpVersion();
        return added;
    }

    /// @brief Removes the component for the given entity.
    ///
    /// Swaps the target with the last element to keep the dense array packed,
    /// then pops the last slot.  Does nothing if the entity has no component.
    void Remove(Entity entity)
    {
        if (!Has(entity))
            return;

        const uint32_t removedPos = _sparse[entity.index];
        const uint32_t lastPos = static_cast<uint32_t>(_dense.size()) - 1;

        if (removedPos != lastPos)
        {
            _dense[removedPos] = std::move(_dense[lastPos]);
            _entities[removedPos] = _entities[lastPos];

            /* Re-point the sparse entry of the entity that was moved into the gap. */
            _sparse[_entities[removedPos].index] = removedPos;

            /* Keep the change-tick lane in lockstep with the dense array. */
            if (_tracksChanges)
                _changeTicks[removedPos] = _changeTicks[lastPos];
        }

        _sparse[entity.index] = Invalid;
        _dense.pop_back();
        _entities.pop_back();
        if (_tracksChanges)
            _changeTicks.pop_back();
        LogRemoval(entity);
        BumpVersion();
    }

    /// @brief Returns true if the entity has a component in this set.
    ///
    /// Compares the full handle (index *and* generation): a stale handle whose
    /// slot was reused by a newer entity will not match the generation stored
    /// in the packed entity array, so it correctly reports false instead of
    /// aliasing the new occupant's component.
    bool Has(Entity entity) const
    {
        if (entity.index >= _sparse.size())
            return false;
        const uint32_t pos = _sparse[entity.index];
        return pos != Invalid && _entities[pos].generation == entity.generation;
    }

    /// @brief Returns a pointer to the entity's component, or nullptr if not present.
    T *Get(Entity entity)
    {
        if (!Has(entity))
            return nullptr;
        return &_dense[_sparse[entity.index]];
    }

    /// @brief Returns a const pointer to the entity's component, or nullptr if not present.
    const T *Get(Entity entity) const
    {
        if (!Has(entity))
            return nullptr;
        return &_dense[_sparse[entity.index]];
    }

    /// @brief Returns the number of components currently stored.
    std::size_t Size() const { return _dense.size(); }

    /// @brief Returns true if no components are stored.
    bool Empty() const { return _dense.empty(); }

    /// @brief Iterators over the dense component array for cache-friendly iteration.
    std::vector<T>::iterator begin() { return _dense.begin(); }
    std::vector<T>::iterator end() { return _dense.end(); }
    std::vector<T>::const_iterator begin() const { return _dense.begin(); }
    std::vector<T>::const_iterator end() const { return _dense.end(); }

    /// @brief Removes all components, resetting the set to an empty state.
    void Clear()
    {
        _sparse.clear();
        _dense.clear();
        _entities.clear();
        _changeTicks.clear(); // no-op when untracked (already empty)
        // Every component went at once. Listing them all would be a log as long
        // as the pool was; saying "nothing before now is known" costs nothing
        // and sends every reader to check what it holds, which it has to anyway.
        _removals.clear();
        if (_removalClock != nullptr)
        {
            _removalFloor = ++*_removalClock;
        }
        BumpVersion();
    }

    /// @brief Direct access to the packed entity array (parallel to dense).
    const std::vector<Entity> &Entities() const { return _entities; }

    // ── Change detection ──────────────────────────────────────────────────────
    // Opt-in per pool (ACOMP(tracked), wired by Scene at pool creation). When
    // enabled, a parallel _changeTicks lane holds a per-component "last written"
    // tick, stamped by the owning Scene on mutable access (Scene::GetMut /
    // MarkChanged / Add). Systems compare it against the tick they last ran at to
    // process only what changed. Untracked pools keep _changeTicks empty, so an
    // untracked component costs nothing per instance.

    /// @brief Enables (or disables) the change-tick lane for this pool. Called
    /// once at pool creation; sizes _changeTicks to match any existing components.
    void SetTracksChanges(bool enabled)
    {
        _tracksChanges = enabled;
        if (enabled)
            _changeTicks.assign(_dense.size(), 0);
        else
            _changeTicks.clear();
    }

    /// @brief Whether this pool maintains change ticks.
    bool TracksChanges() const { return _tracksChanges; }

    /// @brief Records `tick` as the entity's component's last-written tick.
    /// No-op when untracked or the entity has no component here.
    void Stamp(Entity entity, uint64_t tick)
    {
        if (_tracksChanges && Has(entity))
            _changeTicks[_sparse[entity.index]] = tick;
    }

    /// @brief The entity's component's last-written tick, or 0 (never written /
    /// untracked / absent).
    uint64_t ChangeTick(Entity entity) const
    {
        if (!_tracksChanges || !Has(entity))
            return 0;
        return _changeTicks[_sparse[entity.index]];
    }

    /// @brief Appends every entity here whose component was written after
    /// `sinceTick`.
    ///
    /// The inverse question to ChangeTick's: a consumer that must react to what
    /// moved asks this rather than testing each entity it might care about, and
    /// so costs what changed rather than what exists. The scan itself is over the
    /// tick lane alone — a packed vector of integers, touching no component data
    /// — so a still frame reads one array and appends nothing.
    ///
    /// Untracked pools append nothing, which reads as "nothing changed". That is
    /// wrong in the direction that skips work, so only ACOMP(tracked) types may
    /// be asked.
    ///
    /// @p out is appended to, never cleared: several pools' changes accumulate
    /// into one set.
    void ChangedSince(uint64_t sinceTick, std::vector<Entity> &out) const
    {
        if (!_tracksChanges)
            return;
        for (std::size_t pos = 0; pos < _changeTicks.size(); ++pos)
        {
            if (_changeTicks[pos] > sinceTick)
                out.push_back(_entities[pos]);
        }
    }

    // ── Removal log ───────────────────────────────────────────────────────────
    // Which entities lost this component, and at what tick of the owning Scene's
    // clock — the same clock change ticks are stamped from, so one cursor serves
    // both questions. Kept for every pool rather than only tracked ones: it
    // costs an entry per removal and nothing per component.
    //
    // A reader that holds entity handles across frames asks what left since it
    // last looked, and checks only those. The log is bounded; when it drops
    // entries, RemovedSince says so for any reader whose cursor predates them.

    /// @brief The clock removals are stamped from. Wired by the owning Scene at
    /// pool creation; a pool with none (a SparseSet used on its own) logs
    /// nothing.
    void SetRemovalClock(uint64_t *clock) { _removalClock = clock; }

    /// @brief Appends every entity that lost this component after @p sinceTick.
    ///
    /// @return false when the log no longer reaches back to @p sinceTick — it
    /// was trimmed, or the pool was cleared — so some removals since then are
    /// not listed and the reader must check every handle it holds. What is
    /// appended is still correct either way.
    ///
    /// @p out is appended to, never cleared, as with ChangedSince.
    [[nodiscard]] bool RemovedSince(uint64_t sinceTick, std::vector<Entity> &out) const
    {
        // Ticks only rise, so the log is sorted and the tail is the answer.
        std::size_t first = _removals.size();
        while (first > 0 && _removals[first - 1].tick > sinceTick)
        {
            --first;
        }
        for (std::size_t index = first; index < _removals.size(); ++index)
        {
            out.push_back(_removals[index].entity);
        }
        return sinceTick >= _removalFloor;
    }

#ifndef NDEBUG
    /// @brief Debug-only counter bumped on every structural change (Add / Remove
    /// / Clear). A Query iterator snapshots this at construction and re-checks it
    /// as it advances, turning "mutated the pool mid-iteration" — which silently
    /// reallocates the dense/entity arrays out from under the iterator — into a
    /// loud assert instead of memory corruption. Compiled out entirely in release.
    [[nodiscard]] uint32_t StructureVersion() const { return _structureVersion; }
#endif

private:
    /// The most removals a pool remembers. Past it the older half is dropped at
    /// once, so trimming costs a constant amount per removal on average, and a
    /// reader that polls every frame is never behind by anything like this many.
    static constexpr std::size_t kRemovalLogCapacity = 4096;

    struct Removal
    {
        uint64_t tick = 0;
        Entity entity;
    };

    void LogRemoval(Entity entity)
    {
        if (_removalClock == nullptr)
        {
            return;
        }
        if (_removals.size() >= kRemovalLogCapacity)
        {
            const std::size_t dropped = kRemovalLogCapacity / 2;
            _removalFloor = _removals[dropped - 1].tick;
            _removals.erase(_removals.begin(), _removals.begin() + static_cast<std::ptrdiff_t>(dropped));
        }
        _removals.push_back(Removal{.tick = ++*_removalClock, .entity = entity});
    }

    /// Bumps the structural-change counter. A no-op (and no member) in release
    /// builds, so the three call sites cost nothing once NDEBUG is defined.
    void BumpVersion()
    {
#ifndef NDEBUG
        ++_structureVersion;
#endif
    }

    std::vector<uint32_t> _sparse;      ///< Indexed by entity index → dense position.
    std::vector<T> _dense;              ///< Packed component values.
    std::vector<Entity> _entities;      ///< Entity that owns each dense slot.
    std::vector<uint64_t> _changeTicks; ///< Parallel to _dense; per-component last-written tick. Empty when untracked.
    std::vector<Removal> _removals;     ///< Removal log, oldest first; see RemovedSince.
    uint64_t *_removalClock = nullptr;  ///< The owning Scene's change tick; null logs nothing.
    uint64_t _removalFloor = 0;         ///< Removals at or before this tick may be missing from the log.
    bool _tracksChanges = false;        ///< Whether the _changeTicks lane is maintained (ACOMP(tracked)).
#ifndef NDEBUG
    uint32_t _structureVersion = 0; ///< See StructureVersion(); debug-only tripwire state.
#endif
};

} // namespace Assisi::ECS
