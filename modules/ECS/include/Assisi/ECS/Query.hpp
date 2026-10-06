/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Query.hpp
/// @brief Multi-component query view for iterating entities that match a component signature.
///
/// Returned by Scene::Query<Args...>(). Iterates the smallest matching pool and
/// skips entities absent from the others (or present in an excluded pool),
/// yielding (Entity, components...) as a structured binding.
///
/// Each argument names one component: plain, `Mut<T>`, or `Without<T>`. A
/// plain component yields `const T&`. A `Mut<T>` yields `T&` and marks the
/// component changed for every entity the loop reaches, so an ACOMP(tracked)
/// write is never missed and a write through a plain element does not compile.
/// A `Without<T>` yields nothing and skips entities holding a T.
///
/// @code
///   for (auto [e, vel, pos] : scene.Query<Velocity, Mut<Position>>())
///       pos.x += vel.x;
///   // entities with Position but not Frozen:
///   for (auto [e, pos] : scene.Query<Mut<Position>, Without<Frozen>>())
///       pos.x += 1.0f;
/// @endcode
///
/// Mut and Without are also declared at global scope, so code in any
/// namespace writes them unqualified.
///
/// A loop that changes only some of the entities it visits reads through a
/// plain element and writes those few through Scene::GetMut, so only they are
/// marked.

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <vector>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/ECS/SparseSet.hpp>

namespace Assisi::ECS
{

struct Scene; // sole constructor of QueryView — see friend declaration below

/// @brief Query argument rejecting entities holding a T.
///
/// `scene.Query<A, Without<B>>()` yields entities with A but not B; each
/// excluded component is its own Without. An excluded component whose pool has
/// never been created excludes nobody — there are no holders to reject — so a
/// missing excluded pool is simply a no-op filter.
template <typename T> struct Without
{
};

/// @brief Query element asking for T writable: the query yields `T&` and marks
/// T changed for each entity it reaches. Only ever named in a query's element
/// list; nothing constructs one.
template <typename T> struct Mut
{
};

/// @brief What a query element names and yields: a plain T is read-only,
/// Mut<T> writes.
template <typename E> struct QueryElement
{
    using Component = E;
    using Yield = const E &;
    static constexpr bool Writes = false;
};

template <typename T> struct QueryElement<Mut<T>>
{
    using Component = T;
    using Yield = T &;
    static constexpr bool Writes = true;
};

template <typename E> using ComponentOf = typename QueryElement<E>::Component;

/// @brief True when any of Es is a Mut element. A Without is not one.
template <typename... Es> inline constexpr bool AnyWrites = (false || ... || QueryElement<Es>::Writes);

/// @brief How many of Ts are T.
template <typename T, typename... Ts>
inline constexpr std::size_t CountOf = (std::size_t{0} + ... + (std::is_same_v<T, Ts> ? std::size_t{1} : std::size_t{0}));

/// @brief What one Query argument adds to the yielded elements and to the
/// excluded components: a component adds itself as an element, a Without adds
/// its component as an exclusion.
template <typename A> struct QueryArgument
{
    using Elements = std::tuple<A>;
    using Excluded = std::tuple<>;
};

template <typename T> struct QueryArgument<Without<T>>
{
    using Elements = std::tuple<>;
    using Excluded = std::tuple<T>;
};

/// @brief The yielded elements of Query<Args...>, in argument order.
template <typename... Args>
using ElementsOf = decltype(std::tuple_cat(std::declval<typename QueryArgument<Args>::Elements>()...));

/// @brief The excluded components of Query<Args...>.
template <typename... Args>
using ExcludedOf = decltype(std::tuple_cat(std::declval<typename QueryArgument<Args>::Excluded>()...));

/// @brief Whether a split signature is one a query can run: each component
/// yielded once however it is wrapped, and no Mut among the exclusions, which
/// are never yielded and so never written. Naming a component twice would
/// give the iterator two pools of one type to tell apart by type.
template <typename Elements, typename Excluded> inline constexpr bool ValidQuerySignature = false;

template <typename... Es, typename... Xs>
inline constexpr bool ValidQuerySignature<std::tuple<Es...>, std::tuple<Xs...>> =
    ((CountOf<ComponentOf<Es>, ComponentOf<Es>...> == 1) && ...) && !AnyWrites<Xs...>;

/// @brief Arguments a Query accepts.
template <typename... Args>
concept QueryArguments = ValidQuerySignature<ElementsOf<Args...>, ExcludedOf<Args...>>;

/// @brief Arguments a Query on a const Scene accepts: no Mut among them.
template <typename... Args>
concept ReadOnlyQueryArguments = QueryArguments<Args...> && !AnyWrites<Args...>;

/// @brief Placeholder for the scene change-tick back-pointer in a view that
/// writes nothing.
///
/// Stored via [[no_unique_address]], this empty type costs the iterator zero
/// bytes, so a read-only query is no wider for the sake of ones that write.
struct NoChangeTick
{
};

/// @brief The change-tick back-pointer a view needs: one only if it writes.
template <bool Writes> using ChangeTickPtr = std::conditional_t<Writes, uint64_t *, NoChangeTick>;

/// @brief Lazy view over entities matching a component signature.
///
/// Parameterised on a tuple of query elements (plain T or Mut<T>) and a tuple
/// of excluded component types. Excluded types only gate membership and are
/// never yielded.
///
/// The view keeps its pool pointers private: they are the structural handles that
/// could add or remove components behind Scene's liveness gate. Only Scene
/// constructs a view (via `Scene::Query`), so there is no public path to a
/// mutable pool pointer.
template <typename Elements, typename Excluded> struct QueryView;

template <typename... Es, typename... Xs> struct QueryView<std::tuple<Es...>, std::tuple<Xs...>>
{
    static constexpr bool Writes = AnyWrites<Es...>;

    struct Sentinel
    {
    };

    struct Iterator
    {
        std::tuple<Entity, typename QueryElement<Es>::Yield...> operator*() const
        {
            CheckNotInvalidated();
            const Entity entity = (*_entities)[_pos];
            // Braced initialisation evaluates left to right, so the stamps
            // land in element order.
            return std::tuple<Entity, typename QueryElement<Es>::Yield...>{entity, Fetch<Es>(entity)...};
        }

        Iterator &operator++()
        {
            CheckNotInvalidated();
            ++_pos;
            SkipInvalid();
            return *this;
        }

        // Deliberately no invalidation check here: _entities points at a stable
        // member vector of the pool, so reading its size is always safe. The
        // trade-off is that a structural change made right before the loop's
        // exit test can end the loop instead of asserting — a missed detection,
        // not UB; operator*/operator++ catch it on any continued use.
        bool operator==(Sentinel) const { return _pos >= _entities->size(); }
        bool operator!=(Sentinel s) const { return !(*this == s); }

private:
        friend QueryView; // only its enclosing view constructs iterators

        Iterator(const std::vector<Entity> *entities, std::size_t pos,
                 std::tuple<SparseSet<ComponentOf<Es>> *...> required, std::tuple<const SparseSet<Xs> *...> excluded,
                 ChangeTickPtr<Writes> changeTick)
            : _entities(entities), _pos(pos), _required(required), _excluded(excluded), _changeTick(changeTick)
        {
            _versionSnapshot = CurrentStructureVersion();
            SkipInvalid();
        }

        /// The component element E names on @p entity, stamped first when E
        /// is Mut — before the caller writes, as Scene::GetMut does, so a
        /// stamp hook sees the value from before the write.
        ///
        /// Stamping is gated on SparseSet::TracksChanges() before a tick is
        /// allocated: an untracked component costs one bool load and burns no
        /// tick, which would otherwise inflate every consumer's bookmark.
        template <typename E> typename QueryElement<E>::Yield Fetch(Entity entity) const
        {
            using T = ComponentOf<E>;
            if constexpr (QueryElement<E>::Writes)
            {
                SparseSet<T> *pool = std::get<SparseSet<T> *>(_required);
                if (pool->TracksChanges())
                {
                    pool->Stamp(entity, ++*_changeTick);
                }
            }
            return *std::get<T *>(_components);
        }

        /// Membership test and component fetch fused into one pass: Get() is
        /// Has() plus the dense lookup, so asking Has() here and Get() again in
        /// operator* would run every sparse lookup twice. The fetched pointers
        /// are cached for operator* to dereference; they stay valid because the
        /// queried pools must not be structurally mutated during iteration
        /// (the documented Scene::Query contract).
        bool HasAll(Entity e)
        {
            return (... && ((std::get<ComponentOf<Es> *>(_components) =
                                 std::get<SparseSet<ComponentOf<Es>> *>(_required)->Get(e)) != nullptr));
        }

        /// A null excluded pool has no holders, so it rejects nobody.
        bool HasExcluded(Entity e) const
        {
            return std::apply([&](auto *... ps) { return (false || ... || (ps && ps->Has(e))); }, _excluded);
        }

        /// HasAll must run first: operator* trusts the pointer cache only when
        /// the entity matched, and short-circuiting skips the exclusion probe
        /// for entities that already failed the positive match.
        bool Matches(Entity e) { return HasAll(e) && !HasExcluded(e); }

        void SkipInvalid()
        {
            while (_pos < _entities->size() && !Matches((*_entities)[_pos]))
                ++_pos;
        }

        // Sum of the structural-version counters of the *required* pools — the
        // only ones whose reallocation can invalidate this iterator: it drives
        // iteration over a required pool's entity array (_entities) and caches
        // component pointers from the required pools (_components). Excluded
        // pools are deliberately NOT summed: HasExcluded re-probes them each step
        // through their stable pool address, so mutating an excluded pool
        // mid-iteration is safe and must not trip the check (false positive).
        // Versions only ever increase, so any Add/Remove/Clear on a required pool
        // strictly raises the sum, and one integer compare catches it. The
        // per-pool counters exist only in debug (see SparseSet::StructureVersion),
        // so this returns 0 in release, where CheckNotInvalidated's ASSISI_ASSERT
        // compiles away and never calls it from the hot path — only the ctor does.
        uint32_t CurrentStructureVersion() const
        {
#ifndef NDEBUG
            uint32_t sum = 0;
            std::apply([&](auto *... ps) { ((sum += (ps != nullptr) ? ps->StructureVersion() : 0u), ...); },
                       _required);
            return sum;
#else
            return 0;
#endif
        }

        // ASSISI_ASSERT evaluates nothing in release (its arguments sit under an
        // unevaluated sizeof), so this is an empty call there — hence no #ifndef
        // at the call sites in operator*/operator++.
        void CheckNotInvalidated() const
        {
            ASSISI_ASSERT(CurrentStructureVersion() == _versionSnapshot,
                          "structural change (Add/Remove on a queried component pool) during Query "
                          "iteration invalidated the iterator. Destroy is already deferred and safe "
                          "here; for Add/Remove, collect the entities and apply the change after the "
                          "loop.");
        }

        const std::vector<Entity> *_entities;
        std::size_t _pos;
        std::tuple<SparseSet<ComponentOf<Es>> *...> _required;
        std::tuple<const SparseSet<Xs> *...> _excluded;
        /// Cached by HasAll; valid only while the iterator is dereferenceable.
        std::tuple<ComponentOf<Es> *...> _components{};
        /// Scene's change-tick counter, for stamping Mut elements. Empty (and
        /// free) in a view that writes nothing — see NoChangeTick.
        [[no_unique_address]] ChangeTickPtr<Writes> _changeTick;
        /// Required-pool version sum at construction; read only by the debug check.
        uint32_t _versionSnapshot = 0;
    };

    Iterator begin()
    {
        static const std::vector<Entity> empty;
        return Iterator{_primary ? _primary : &empty, 0, _required, _excluded, _changeTick};
    }

    Sentinel end() const { return {}; }

private:
    friend struct Scene; // QueryView exists only to be returned by Scene::Query

    QueryView(std::tuple<SparseSet<ComponentOf<Es>> *...> required, const std::vector<Entity> *primary,
              std::tuple<const SparseSet<Xs> *...> excluded, ChangeTickPtr<Writes> changeTick)
        : _required(required), _primary(primary), _excluded(excluded), _changeTick(changeTick)
    {
    }

    std::tuple<SparseSet<ComponentOf<Es>> *...> _required;
    const std::vector<Entity> *_primary; ///< Entity list of the smallest required pool; nullptr = no results.
    std::tuple<const SparseSet<Xs> *...> _excluded;
    [[no_unique_address]] ChangeTickPtr<Writes> _changeTick; ///< Passed to each Iterator; empty when nothing writes.
};

/// @brief The view Scene::Query<Args...>() returns.
template <typename... Args> using QueryViewOf = QueryView<ElementsOf<Args...>, ExcludedOf<Args...>>;

} // namespace Assisi::ECS

// Written in nearly every system, game code included, so they are visible
// unqualified from every namespace.
using Assisi::ECS::Mut;
using Assisi::ECS::Without;
