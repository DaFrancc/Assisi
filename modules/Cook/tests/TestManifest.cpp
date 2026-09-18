/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestManifest.cpp
/// @brief The cooked tree's index: what it writes, what it reads back, and what
/// it does with a line it cannot read.
///
/// The manifest is what makes the cook incremental, so a parse that quietly
/// mis-read a key would either re-cook everything (slow, noticed) or skip an
/// asset that changed (fast, shipped wrong). These pin the second one.

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <Assisi/Cook/CookTree.hpp>

using Assisi::Cook::DeserializeManifest;
using Assisi::Cook::ManifestEntry;
using Assisi::Cook::SerializeManifest;

TEST_CASE("A manifest round-trips every field")
{
    const std::vector<ManifestEntry> written{
        ManifestEntry{.vpath      = "levels/Test.alvl",
                      .guid       = "3f2504e0-4f89-41d3-9a0c-0305e82c3301",
                      .cookKey    = 0x0123456789abcdefULL,
                      .outputHash = 0xfedcba9876543210ULL},
        ManifestEntry{.vpath      = "textures/checker.png",
                      .guid       = "11111111-2222-4333-8444-555555555555",
                      .cookKey    = 1,
                      .outputHash = 0},
    };

    const std::vector<ManifestEntry> read = DeserializeManifest(SerializeManifest(written));
    REQUIRE(read.size() == written.size());
    for (std::size_t i = 0; i < written.size(); ++i)
    {
        CHECK(read[i].vpath == written[i].vpath);
        CHECK(read[i].guid == written[i].guid);
        CHECK(read[i].cookKey == written[i].cookKey);
        CHECK(read[i].outputHash == written[i].outputHash);
    }
}

TEST_CASE("A 64-bit key survives, rather than losing its top bits")
{
    // Written as hex text for exactly this reason: a key that went through a
    // double would come back changed, and the asset would re-cook forever or
    // never.
    const std::vector<ManifestEntry> written{ManifestEntry{.vpath      = "a",
                                                           .guid       = "b",
                                                           .cookKey    = 0xFFFFFFFFFFFFFFFFULL,
                                                           .outputHash = 0x8000000000000001ULL}};

    const std::vector<ManifestEntry> read = DeserializeManifest(SerializeManifest(written));
    REQUIRE(read.size() == 1);
    CHECK(read[0].cookKey == 0xFFFFFFFFFFFFFFFFULL);
    CHECK(read[0].outputHash == 0x8000000000000001ULL);
}

TEST_CASE("A malformed line is dropped rather than half-read")
{
    // Dropping costs a re-cook of that asset. Guessing at it risks skipping one
    // that changed, which ships the old bytes.
    const std::string text = "levels/Test.alvl 3f2504e0 0123456789abcdef fedcba9876543210\n"
                             "this line has no fields\n"
                             "two fields only\n"
                             "textures/x.png guid notahexkey alsonothex\n"
                             "\n";

    const std::vector<ManifestEntry> read = DeserializeManifest(text);
    REQUIRE(read.size() == 1);
    CHECK(read[0].vpath == "levels/Test.alvl");
}

TEST_CASE("An empty manifest reads as no entries")
{
    CHECK(DeserializeManifest("").empty());
    CHECK(DeserializeManifest("\n\n").empty());
}

TEST_CASE("The manifest is written in the order it is given")
{
    // Sorted upstream by the walk, so that two cooked trees diff line by line.
    // Re-sorting here would hide a walk that stopped being deterministic.
    const std::vector<ManifestEntry> written{
        ManifestEntry{.vpath = "b", .guid = "g", .cookKey = 1, .outputHash = 1},
        ManifestEntry{.vpath = "a", .guid = "g", .cookKey = 2, .outputHash = 2},
    };

    const std::string text = SerializeManifest(written);
    CHECK(text.find("b ") < text.find("a "));
}
