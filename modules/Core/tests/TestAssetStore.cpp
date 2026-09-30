/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestAssetStore.cpp
/// @brief Assets of a registered kind load by id in the background, from a
/// package or from source files cooked on demand; one that fails is not loaded
/// again; and one asked for as the wrong type is never loaded as it.

#include <Assisi/Core/AssetKind.hpp>
#include <Assisi/Core/AssetProvider.hpp>
#include <Assisi/Core/AssetStore.hpp>
#include <Assisi/Core/CookingProvider.hpp>
#include <Assisi/Core/JobSystem.hpp>
#include <Assisi/Testing/TestAssetKinds.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace Assisi;

namespace
{

/// Enough workers that a load really runs off the main thread.
constexpr std::uint32_t kWorkers = 2;

/// Resolves repeated for an asset that failed, standing in for a system asking every frame.
constexpr std::uint32_t kRepeatedResolves = 10;

/// Bytes by id, counting every open: a package, or a source tree, held in memory.
class MemoryProvider final : public Core::AssetProvider
{
  public:
    void Add(Core::AssetId id, std::vector<std::byte> bytes) { _files[id] = std::move(bytes); }

    [[nodiscard]] std::expected<std::vector<std::byte>, Core::AssetError> Open(Core::AssetId id) const override
    {
        _opens.fetch_add(1);
        const std::unordered_map<Core::AssetId, std::vector<std::byte>>::const_iterator found = _files.find(id);
        if (found == _files.end())
        {
            return std::unexpected(Core::AssetErrorCode::UnknownAssetId);
        }
        return found->second;
    }

    [[nodiscard]] std::expected<Core::AssetId, Core::AssetError> Resolve(std::string_view) const override
    {
        return std::unexpected(Core::AssetErrorCode::UnknownAssetId);
    }

    [[nodiscard]] std::uint32_t Opens() const { return _opens.load(); }

  private:
    std::unordered_map<Core::AssetId, std::vector<std::byte>> _files;
    mutable std::atomic<std::uint32_t> _opens = 0;
};

std::vector<std::byte> Bytes(std::string_view text)
{
    const std::byte *first = reinterpret_cast<const std::byte *>(text.data());
    return std::vector<std::byte>{first, first + text.size()};
}

/// A blob of @p kind holding @p payload, as the cook would write it.
std::vector<std::byte> Blob(Core::AssetKindId kind, std::string_view payload)
{
    const Core::AssetKind *registered = Core::AssetKindRegistry::Instance().Find(kind);
    REQUIRE(registered != nullptr);
    // The raw kind has no cook step, so its blob is exactly the envelope and the payload.
    REQUIRE(Core::AssetKindRegistry::Instance().CookStepFor(kind) == nullptr);
    std::expected<std::vector<std::byte>, Core::AssetError> blob = Core::CookAssetBytes(*registered, Bytes(payload));
    REQUIRE(blob.has_value());
    return std::move(*blob);
}

/// Run workers and main-thread publishes until nothing is loading.
void Finish(Core::JobSystem &jobs, const Core::AssetStore &store)
{
    jobs.HelpUntil([&store] { return !store.HasPendingLoads(); }, true);
}

/// Source files by id with the path and kind each one's sidecar would give it,
/// held in memory: a source tree without the files.
class SourceTree
{
  public:
    /// @param kind empty for a sidecar that names none.
    Core::AssetId Add(const std::string &path, const std::string &kind, std::string_view contents)
    {
        const Core::AssetId id = Core::DerivedAssetId(path);
        _files.Add(id, Bytes(contents));
        _paths[id] = path;
        if (!kind.empty())
        {
            _kinds[id] = kind;
        }
        return id;
    }

    [[nodiscard]] Core::CookingProvider Provider() const
    {
        return Core::CookingProvider{_files, [this](Core::AssetId id) { return Lookup(_paths, id); },
                                     [this](Core::AssetId id) { return Lookup(_kinds, id); }};
    }

  private:
    using TextById = std::unordered_map<Core::AssetId, std::string>;

    static std::optional<std::string> Lookup(const TextById &table, Core::AssetId id)
    {
        const TextById::const_iterator found = table.find(id);
        if (found == table.end())
        {
            return std::nullopt;
        }
        return found->second;
    }

    MemoryProvider _files;
    TextById _paths;
    TextById _kinds;
};

/// A type no kind loads as.
struct NotLoadedByAnyKind
{
};

const Core::AssetId kPresent = Core::DerivedAssetId("things/present.traw");
const Core::AssetId kMissing = Core::DerivedAssetId("things/missing.traw");

} // namespace

TEST_CASE("An asset loads in the background and is shared once the load lands")
{
    Core::JobSystem jobs(kWorkers);
    MemoryProvider package;
    package.Add(kPresent, Blob(Testing::kRawKind, "hello"));
    Core::AssetStore store;
    store.Initialize(jobs, package);

    CHECK(store.Resolve<Testing::TestBytes>(kPresent) == nullptr);
    CHECK(store.HasPendingLoads());
    Finish(jobs, store);

    const std::shared_ptr<const Testing::TestBytes> loaded = store.Resolve<Testing::TestBytes>(kPresent);
    REQUIRE(loaded != nullptr);
    CHECK(loaded->bytes == Bytes("hello"));
    // The same value every time, rather than a load per request.
    CHECK(store.Resolve<Testing::TestBytes>(kPresent) == loaded);
    CHECK(package.Opens() == 1);
}

TEST_CASE("A nil id resolves to nothing and opens nothing")
{
    Core::JobSystem jobs(kWorkers);
    MemoryProvider package;
    Core::AssetStore store;
    store.Initialize(jobs, package);

    CHECK(store.Resolve<Testing::TestBytes>(Core::AssetId{}) == nullptr);
    CHECK_FALSE(store.HasPendingLoads());
    CHECK(package.Opens() == 0);
}

TEST_CASE("A missing asset stays null and is opened once, however often it is asked for")
{
    Core::JobSystem jobs(kWorkers);
    MemoryProvider package;
    Core::AssetStore store;
    store.Initialize(jobs, package);

    for (std::uint32_t i = 0; i < kRepeatedResolves; ++i)
    {
        CHECK(store.Resolve<Testing::TestBytes>(kMissing) == nullptr);
        Finish(jobs, store);
    }
    CHECK(package.Opens() == 1);
}

TEST_CASE("An asset whose load fails stays null and is opened once")
{
    Core::JobSystem jobs(kWorkers);
    MemoryProvider package;
    // The raw kind refuses an empty payload.
    package.Add(kPresent, Blob(Testing::kRawKind, ""));
    Core::AssetStore store;
    store.Initialize(jobs, package);

    for (std::uint32_t i = 0; i < kRepeatedResolves; ++i)
    {
        CHECK(store.Resolve<Testing::TestBytes>(kPresent) == nullptr);
        Finish(jobs, store);
    }
    CHECK(package.Opens() == 1);
}

TEST_CASE("An asset asked for as a type its kind does not load is never loaded as it")
{
    Core::JobSystem jobs(kWorkers);
    MemoryProvider package;
    package.Add(kPresent, Blob(Testing::kRawKind, "hello"));
    Core::AssetStore store;
    store.Initialize(jobs, package);

    const std::uint32_t loadsBefore = Testing::TestKindLoads().load();
    CHECK(store.Resolve<NotLoadedByAnyKind>(kPresent) == nullptr);
    Finish(jobs, store);
    CHECK(store.Resolve<NotLoadedByAnyKind>(kPresent) == nullptr);
    CHECK(Testing::TestKindLoads().load() == loadsBefore);
}

TEST_CASE("An asset loaded as one type is not handed out as another")
{
    Core::JobSystem jobs(kWorkers);
    MemoryProvider package;
    package.Add(kPresent, Blob(Testing::kRawKind, "hello"));
    Core::AssetStore store;
    store.Initialize(jobs, package);

    (void)store.Resolve<Testing::TestBytes>(kPresent);
    Finish(jobs, store);
    REQUIRE(store.Resolve<Testing::TestBytes>(kPresent) != nullptr);
    CHECK(store.Resolve<NotLoadedByAnyKind>(kPresent) == nullptr);
}

TEST_CASE("A blob of a kind nothing in this build loads stays null and is opened once")
{
    Core::JobSystem jobs(kWorkers);
    MemoryProvider package;
    Core::BitWriter writer;
    Core::WriteCookedHeader(writer, Core::AssetKindId{"a kind no module in this build registers"});
    const std::span<const std::byte> header = writer.Data();
    package.Add(kPresent, std::vector<std::byte>{header.begin(), header.end()});
    Core::AssetStore store;
    store.Initialize(jobs, package);

    for (std::uint32_t i = 0; i < kRepeatedResolves; ++i)
    {
        CHECK(store.Resolve<Testing::TestBytes>(kPresent) == nullptr);
        Finish(jobs, store);
    }
    CHECK(package.Opens() == 1);
}

TEST_CASE("A load that lands after a Clear is dropped, and the next request loads again")
{
    Core::JobSystem jobs(kWorkers);
    MemoryProvider package;
    package.Add(kPresent, Blob(Testing::kRawKind, "hello"));
    Core::AssetStore store;
    store.Initialize(jobs, package);

    CHECK(store.Resolve<Testing::TestBytes>(kPresent) == nullptr);
    // The wait below is for this load's open, which never comes if none started.
    REQUIRE(store.HasPendingLoads());
    // Before any main-thread drain, so the load cannot have published yet.
    store.Clear();
    CHECK_FALSE(store.HasPendingLoads());
    jobs.HelpUntil([&package] { return package.Opens() == 1; }, true);
    (void)jobs.DrainMain();

    CHECK(store.Resolve<Testing::TestBytes>(kPresent) == nullptr);
    Finish(jobs, store);
    CHECK(store.Resolve<Testing::TestBytes>(kPresent) != nullptr);
    CHECK(package.Opens() == 2);
}

TEST_CASE("Source files cook on demand and load through the same store")
{
    SourceTree tree;
    const Core::AssetId kSource = tree.Add("things/sample.tbytes", "test reversed bytes", "abc");
    const Core::CookingProvider cooking = tree.Provider();

    Core::JobSystem jobs(kWorkers);
    Core::AssetStore store;
    store.Initialize(jobs, cooking);
    (void)store.Resolve<Testing::TestBytes>(kSource);
    Finish(jobs, store);

    const std::shared_ptr<const Testing::TestBytes> loaded = store.Resolve<Testing::TestBytes>(kSource);
    REQUIRE(loaded != nullptr);
    // Cooked by the kind's own step, then finished on the main thread before it was shared.
    CHECK(loaded->bytes == Bytes("cba"));
    CHECK(loaded->finished);
}

TEST_CASE("An asset whose finishing step fails stays null")
{
    SourceTree tree;
    // Reversed by the cook, so the payload starts with the 'F' that fails the finish.
    const Core::AssetId kSource = tree.Add("things/finish-fails.tbytes", "test reversed bytes", "abF");
    const Core::CookingProvider cooking = tree.Provider();

    Core::JobSystem jobs(kWorkers);
    Core::AssetStore store;
    store.Initialize(jobs, cooking);
    (void)store.Resolve<Testing::TestBytes>(kSource);
    Finish(jobs, store);
    CHECK(store.Resolve<Testing::TestBytes>(kSource) == nullptr);
}

TEST_CASE("A PNG whose sidecar says it is another kind loads as that kind")
{
    SourceTree tree;
    const Core::AssetId kPhoto = tree.Add("things/photo.png", "test raw bytes", "not really a png");
    const Core::CookingProvider cooking = tree.Provider();

    Core::JobSystem jobs(kWorkers);
    Core::AssetStore store;
    store.Initialize(jobs, cooking);
    (void)store.Resolve<Testing::TestBytes>(kPhoto);
    Finish(jobs, store);

    const std::shared_ptr<const Testing::TestBytes> loaded = store.Resolve<Testing::TestBytes>(kPhoto);
    REQUIRE(loaded != nullptr);
    CHECK(loaded->bytes == Bytes("not really a png"));
}

TEST_CASE("The cooking provider takes a file's kind from its sidecar and nowhere else")
{
    SourceTree tree;
    // A .tbytes with no kind is refused, although only one kind reads .tbytes.
    const Core::AssetId kNoKind = tree.Add("things/unstated.tbytes", "", "abc");
    // A kind that does not read the file's format.
    const Core::AssetId kWrongFormat = tree.Add("things/wrong.traw", "test reversed bytes", "abc");
    // A kind this build does not have.
    const Core::AssetId kUnknownKind = tree.Add("things/unknown.tbytes", "no such kind", "abc");
    // An engine kind, which has its own loaders and is not served here.
    const Core::AssetId kTexture = tree.Add("things/plain.png", "texture", "abc");
    // A kind whose cook step refuses the file.
    const Core::AssetId kRefused = tree.Add("things/refused.tbytes", "test reversed bytes", "Xyz");
    const Core::AssetId kNoPath = Core::DerivedAssetId("things/nowhere.tbytes");
    const Core::CookingProvider cooking = tree.Provider();

    CHECK(cooking.Open(kNoKind).error() == Core::AssetErrorCode::UnknownAssetId);
    CHECK(cooking.Open(kWrongFormat).error() == Core::AssetErrorCode::UnknownAssetId);
    CHECK(cooking.Open(kUnknownKind).error() == Core::AssetErrorCode::UnknownAssetId);
    CHECK(cooking.Open(kTexture).error() == Core::AssetErrorCode::UnknownAssetId);
    CHECK(cooking.Open(kNoPath).error() == Core::AssetErrorCode::UnknownAssetId);
    // The step's own error, not one the provider made up.
    CHECK(cooking.Open(kRefused).error().detail == Testing::kCookRefusedDetail);
}
