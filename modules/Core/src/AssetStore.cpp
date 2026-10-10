/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Core/AssetStore.hpp>

#include <Assisi/Core/AssetKind.hpp>
#include <Assisi/Core/AssetProvider.hpp>
#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Core/JobSystem.hpp>
#include <Assisi/Core/Logger.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Assisi::Core
{

namespace
{

/// A loaded value and the finishing step it still needs, or why there is none.
struct Loaded
{
    std::shared_ptr<void> value;
    const AssetKind *kind = nullptr;
};

using LoadResult = std::expected<Loaded, AssetError>;

bool Accepts(std::span<const std::type_index> accepted, std::type_index type)
{
    return std::ranges::find(accepted, type) != accepted.end();
}

/// Worker side: read the blob, find its kind, check it loads as one of the
/// types asked for, and load it. Touches no store state.
LoadResult ReadAndLoad(const AssetProvider &provider, AssetId id, std::span<const std::type_index> accepted)
{
    const std::expected<std::vector<std::byte>, AssetError> bytes = provider.Open(id);
    if (!bytes)
    {
        return std::unexpected(bytes.error());
    }

    BitReader reader{*bytes};
    const std::expected<AssetKindId, CookedBlobError> kindId = ReadCookedHeader(reader);
    if (!kindId)
    {
        return std::unexpected(AssetError{AssetErrorCode::CorruptAsset, ToString(kindId.error())});
    }
    const AssetKind *kind = AssetKindRegistry::Instance().Find(*kindId);
    if (kind == nullptr || !kind->load)
    {
        return std::unexpected(
            AssetError{AssetErrorCode::UnsupportedEncoding, "nothing in this build loads the asset's kind"});
    }
    if (!Accepts(accepted, kind->valueType))
    {
        return std::unexpected(AssetErrorCode::WrongType);
    }

    const std::span<const std::byte> payload = std::span<const std::byte>{*bytes}.subspan(kCookedHeaderBytes);
    std::expected<std::shared_ptr<void>, AssetError> value = kind->load(payload);
    if (!value)
    {
        return std::unexpected(value.error());
    }
    return Loaded{.value = std::move(*value), .kind = kind};
}

} // namespace

struct AssetStore::State
{
    std::unordered_map<AssetId, std::shared_ptr<const void>> resident;

    /// Ids with a load in flight, so asking again while one runs starts no second.
    std::unordered_set<AssetId> loading;

    /// Ids whose load failed. Their warning has been logged and they are not
    /// loaded again, so a system asking every frame costs one read, not one a frame.
    std::unordered_set<AssetId> failed;

    /// The type each resident id loaded as, so asking for it as another type is
    /// refused instead of cast.
    std::unordered_map<AssetId, std::type_index> residentType;

    /// How many times each id has been forgotten. A load publishes only if its
    /// id's count is still the one it started with, so a load that began on
    /// the old file never lands over a newer one.
    std::unordered_map<AssetId, std::uint32_t> forgotten;
};

AssetStore::AssetStore() : _state(std::make_shared<State>())
{
}

AssetStore::~AssetStore() = default;

void AssetStore::Initialize(JobSystem &jobs, const AssetProvider &provider)
{
    _jobs = &jobs;
    _provider = &provider;
}

AssetStore::ErasedAsset AssetStore::ResolveErased(AssetId id, std::span<const std::type_index> accepted)
{
    if (id.IsNil() || _jobs == nullptr)
    {
        return {};
    }

    State &state = *_state;
    const std::unordered_map<AssetId, std::shared_ptr<const void>>::const_iterator found = state.resident.find(id);
    if (found != state.resident.end())
    {
        const std::type_index type = state.residentType.at(id);
        if (!Accepts(accepted, type))
        {
            if (state.failed.insert(id).second)
            {
                Log::Warn("AssetStore: asset {} was asked for as a type it does not load as.", id.ToString());
            }
            return {};
        }
        return ErasedAsset{.value = found->second, .type = type};
    }
    if (state.loading.contains(id) || state.failed.contains(id))
    {
        return {};
    }

    state.loading.insert(id);
    const std::weak_ptr<State> startedIn = _state;
    const std::uint32_t generation = state.forgotten[id];
    // A copy: the caller's types live only as long as its call.
    const std::vector<std::type_index> types{accepted.begin(), accepted.end()};
    _jobs->Run(Pool::Worker, [provider = _provider, id, types] { return ReadAndLoad(*provider, id, types); })
        .Then(Pool::Main,
              [startedIn, id, generation](LoadResult result)
              {
                  const std::shared_ptr<State> live = startedIn.lock();
                  if (live == nullptr || live->forgotten[id] != generation)
                  {
                      return;
                  }
                  live->loading.erase(id);

                  if (result && result->kind->finish)
                  {
                      const std::expected<void, AssetError> finished = result->kind->finish(result->value.get());
                      if (!finished)
                      {
                          result = std::unexpected(finished.error());
                      }
                  }
                  if (!result)
                  {
                      live->failed.insert(id);
                      Log::Warn("AssetStore: asset {} did not load ({}); anything using it goes without.",
                                id.ToString(), Describe(result.error()));
                      return;
                  }
                  live->resident.emplace(id, std::move(result->value));
                  live->residentType.emplace(id, result->kind->valueType);
              });
    return {};
}

void AssetStore::Forget(AssetId id)
{
    State &state = *_state;
    state.resident.erase(id);
    state.residentType.erase(id);
    state.failed.erase(id);
    state.loading.erase(id);
    ++state.forgotten[id];
}

bool AssetStore::HasPendingLoads() const
{
    return !_state->loading.empty();
}

void AssetStore::Clear()
{
    _state = std::make_shared<State>();
}

} // namespace Assisi::Core
