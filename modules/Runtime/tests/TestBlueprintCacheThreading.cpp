/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBlueprintCacheThreading.cpp
/// @brief The blueprint definition cache is reached from more than one thread,
///        and the definitions it lends out outlive it.
///
/// Two threads reach `GetBlueprintDefinition` today. Async travel deserializes on
/// a worker (App/src/World.cpp), and staging an instance there asks for the
/// definition; the editor asks for the same thing every frame on the main thread
/// to name the selected instance's members. A `std::map` being read while another
/// thread inserts into it is undefined behaviour, and eviction is worse than that:
/// a definition handed out is a definition somebody is still walking.
///
/// The first case is what the tsan target exists for. The second needs no threads
/// at all — holding a definition across an eviction is the same lifetime bug, just
/// spelled deterministically so asan catches it every run rather than sometimes.

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Runtime/Blueprint.hpp>

using namespace Assisi;
using Assisi::Runtime::BlueprintDefinition;
using Assisi::Runtime::BlueprintResult;

namespace
{

constexpr std::int32_t kSources = 6;

std::string SourceName(std::int32_t index)
{
    return "car" + std::to_string(index) + ".abp";
}

/// A root holding kSources distinct blueprints, each nesting the one below it so a
/// build is a real walk rather than a single file read.
std::filesystem::path FreshRoot(const std::string &name)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("assisi_bpc_" + name);
    std::error_code             ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());
    Runtime::ClearBlueprintCache();

    for (std::int32_t index = 0; index < kSources; ++index)
    {
        const float fov = 55.f + static_cast<float>(index);

        nlohmann::json doc = {
            {"version", 2},
            {"entities",
             nlohmann::json::array({{{"name", "body"}, {"components", {{"Camera", {{"fovDegrees", fov}}}}}}})}};
        if (index > 0)
        {
            doc["instances"] = nlohmann::json::array({{{"name", "inner"}, {"source", SourceName(index - 1)}}});
        }

        std::ofstream out(root / SourceName(index), std::ios::binary);
        out << doc.dump(2);
        REQUIRE(out.good());
    }
    return root;
}

} // namespace

TEST_CASE("Blueprint cache: eight threads build and look up at once")
{
    const std::filesystem::path root = FreshRoot("hammer");

    // Set before any thread starts and never touched again: the asset root is
    // write-once, read-many, so it is not what this case is measuring.
    constexpr std::int32_t kThreads    = 8;
    constexpr std::int32_t kIterations = 40;

    std::atomic<std::int32_t> nulls{0};
    std::vector<std::thread>  workers;
    workers.reserve(kThreads);

    // Every thread walks the same sources in a different rotation, so some arrive
    // at a given file first and build it while others are already reading it.
    for (std::int32_t index = 0; index < kThreads; ++index)
    {
        workers.emplace_back(
            [index, &nulls]
            {
                for (std::int32_t iteration = 0; iteration < kIterations; ++iteration)
                {
                    for (std::int32_t step = 0; step < kSources; ++step)
                    {
                        const std::int32_t which = (index + step) % kSources;

                        const BlueprintResult definition = Runtime::GetBlueprintDefinition(SourceName(which));
                        if (!definition || (*definition)->members.empty())
                        {
                            nulls.fetch_add(1, std::memory_order_relaxed);
                            continue;
                        }

                        // Read through it as a caller would, so a torn or
                        // half-inserted definition is something this notices.
                        for (const Runtime::BlueprintMemberDesc &member : (*definition)->members)
                        {
                            if (member.name.empty())
                                nulls.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            });
    }

    for (std::thread &worker : workers)
        worker.join();

    CHECK(nulls.load(std::memory_order_relaxed) == 0);

    // Built once, however many threads asked: the cache is the whole reason
    // spawning a hundred bullets does not reparse bullet.abp a hundred times.
    for (std::int32_t index = 0; index < kSources; ++index)
    {
        const BlueprintResult first  = Runtime::GetBlueprintDefinition(SourceName(index));
        const BlueprintResult second = Runtime::GetBlueprintDefinition(SourceName(index));
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK(*first == *second);
    }
}

TEST_CASE("Blueprint cache: a definition handed out survives the cache being cleared")
{
    const std::filesystem::path root = FreshRoot("clear");

    const BlueprintResult loaded = Runtime::GetBlueprintDefinition(SourceName(0));
    REQUIRE(loaded.has_value());
    const std::shared_ptr<const BlueprintDefinition> held = *loaded;
    REQUIRE_FALSE(held->members.empty());
    const std::string expected = held->members.front().name;

    // What a level unload does under a worker that is still staging instances out
    // of the definition it was handed a moment ago.
    Runtime::ClearBlueprintCache();

    CHECK(held->members.front().name == expected);
    CHECK(held->source == SourceName(0));
}

TEST_CASE("Blueprint cache: a definition handed out survives its file being invalidated")
{
    const std::filesystem::path root = FreshRoot("invalidate");

    // The nesting one, so invalidating the inner file evicts this outer entry too —
    // the eviction a caller holding the outer definition never asked for.
    const BlueprintResult loaded = Runtime::GetBlueprintDefinition(SourceName(1));
    REQUIRE(loaded.has_value());
    const std::shared_ptr<const BlueprintDefinition> held = *loaded;
    const std::size_t members = held->members.size();

    Runtime::InvalidateBlueprint(SourceName(0));

    CHECK(held->members.size() == members);
    for (const Runtime::BlueprintMemberDesc &member : held->members)
        CHECK_FALSE(member.name.empty());
}

TEST_CASE("Blueprint cache: lookups race an eviction without tearing the map")
{
    const std::filesystem::path root = FreshRoot("evict");

    constexpr std::int32_t kReaders    = 6;
    constexpr std::int32_t kIterations = 200;

    std::atomic<bool>        keepEvicting{true};
    std::atomic<std::int32_t> torn{0};
    std::vector<std::thread> readers;
    readers.reserve(kReaders);

    for (std::int32_t index = 0; index < kReaders; ++index)
    {
        readers.emplace_back(
            [index, &torn]
            {
                for (std::int32_t iteration = 0; iteration < kIterations; ++iteration)
                {
                    const std::int32_t which = (index + iteration) % kSources;

                    // A miss is legitimate here only if the file cannot be built,
                    // which it always can — an eviction just means the next ask
                    // rebuilds it.
                    const BlueprintResult definition = Runtime::GetBlueprintDefinition(SourceName(which));
                    if (!definition || (*definition)->source != SourceName(which))
                        torn.fetch_add(1, std::memory_order_relaxed);
                }
            });
    }

    // The editor saving a blueprint while a level loads in the background: an
    // eviction landing in the middle of somebody else's lookup.
    std::thread evictor(
        [&keepEvicting]
        {
            std::int32_t which = 0;
            while (keepEvicting.load(std::memory_order_relaxed))
            {
                Runtime::InvalidateBlueprint(SourceName(which % kSources));
                ++which;
            }
        });

    for (std::thread &reader : readers)
        reader.join();
    keepEvicting.store(false, std::memory_order_relaxed);
    evictor.join();

    CHECK(torn.load(std::memory_order_relaxed) == 0);
}
