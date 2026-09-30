/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AssetStore.hpp
/// @brief Assets of every registered kind by id, loaded in the background and
///        shared by everything that uses them.
///
/// The store reads cooked blobs from one AssetProvider: the package in a game,
/// a CookingProvider over the source files everywhere else. The blob's envelope
/// names its kind, AssetKindRegistry says how to load it, and the first request
/// for an id starts that load on a worker; the value becomes available once a
/// main-thread drain publishes it.

#include <Assisi/Core/AssetId.hpp>

#include <memory>
#include <typeindex>
#include <typeinfo>

namespace Assisi::Core
{

class AssetProvider;
class JobSystem;

/// @brief Every asset resolved so far, kept until Clear().
///
/// Main thread only. Workers read and load and touch none of this; the result
/// is published by a main-thread task, so nothing here needs a lock.
class AssetStore
{
  public:
    AssetStore();
    ~AssetStore();

    AssetStore(const AssetStore &) = delete;
    AssetStore &operator=(const AssetStore &) = delete;
    AssetStore(AssetStore &&) = delete;
    AssetStore &operator=(AssetStore &&) = delete;

    /// @brief Load through @p provider on @p jobs from now on. Until this is
    ///        called, every Resolve returns null.
    ///
    /// @p jobs and @p provider must outlive every load started, which the app
    /// guarantees by owning both for the life of the process.
    void Initialize(JobSystem &jobs, const AssetProvider &provider);

    /// @brief The loaded @p T for @p id, or null while it is still loading, when
    ///        @p id is nil, or when it failed to load.
    ///
    /// The first call for an id starts its load; later calls return null until
    /// it lands. An asset that failed — missing, unreadable, of a kind this
    /// build has no loader for, or not a @p T — logs one warning, stays null,
    /// and is not loaded again.
    template <typename T> [[nodiscard]] std::shared_ptr<const T> Resolve(AssetId id)
    {
        // Sound because ResolveErased only returns a value whose kind loads a T.
        return std::static_pointer_cast<const T>(ResolveErased(id, typeid(T)));
    }

    /// @brief Whether any asset is still being read or loaded.
    [[nodiscard]] bool HasPendingLoads() const;

    /// @brief Forget every asset, including failed ones, and drop the loads in
    ///        flight. Values already handed out stay valid for whoever holds them.
    void Clear();

  private:
    struct State;

    [[nodiscard]] std::shared_ptr<const void> ResolveErased(AssetId id, std::type_index wanted);

    /// Replaced whole by Clear(). A load's publish holds a weak reference to the
    /// state it started in, so one that lands after a Clear finds it gone.
    std::shared_ptr<State> _state;
    JobSystem *_jobs = nullptr;
    const AssetProvider *_provider = nullptr;
};

} // namespace Assisi::Core
