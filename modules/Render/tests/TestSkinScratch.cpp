/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestSkinScratch.cpp
/// @brief A skinned instance's range of posed vertices is its own while it
/// claims it every frame, and goes to the next instance of the same size once
/// it stops.

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>

#include <Assisi/Render/SkinScratch.hpp>

using Assisi::Render::SkinScratch;

namespace
{

constexpr uint32_t kCount = 10;
constexpr uint32_t kOtherCount = 20;
constexpr uint32_t kFirstBase = 100;
constexpr uint32_t kSecondBase = 200;
constexpr uint32_t kOwnerA = 1;
constexpr uint32_t kOwnerB = 2;
constexpr uint32_t kOwnerC = 3;

} // namespace

TEST_CASE("Skin scratch: nothing is free before any range is let go")
{
    SkinScratch scratch;
    CHECK_FALSE(scratch.Reuse(kCount, kOwnerA).has_value());
    scratch.Add(kFirstBase, kCount, kOwnerA);
    CHECK_FALSE(scratch.Reuse(kCount, kOwnerB).has_value());
}

TEST_CASE("Skin scratch: a range claimed every frame stays its owner's")
{
    SkinScratch scratch;
    scratch.Add(kFirstBase, kCount, kOwnerA);
    scratch.EndFrame();
    CHECK(scratch.Claim(kFirstBase, kOwnerA));
    scratch.EndFrame();
    CHECK(scratch.Claim(kFirstBase, kOwnerA));
    scratch.EndFrame();
    CHECK_FALSE(scratch.Reuse(kCount, kOwnerB).has_value());
}

TEST_CASE("Skin scratch: a range not claimed for a frame goes to the next instance of its size")
{
    SkinScratch scratch;
    scratch.Add(kFirstBase, kCount, kOwnerA);
    scratch.EndFrame();
    // Owner A is gone: nothing claims its range this frame.
    scratch.EndFrame();

    const std::optional<uint32_t> reused = scratch.Reuse(kCount, kOwnerB);
    REQUIRE(reused.has_value());
    CHECK(*reused == kFirstBase);
    CHECK(scratch.RangeCount() == 1);
    // And it is no longer A's.
    CHECK_FALSE(scratch.Claim(kFirstBase, kOwnerA));
}

TEST_CASE("Skin scratch: a free range is never handed to an instance of another size")
{
    SkinScratch scratch;
    scratch.Add(kFirstBase, kCount, kOwnerA);
    scratch.EndFrame();
    scratch.EndFrame();
    CHECK_FALSE(scratch.Reuse(kOtherCount, kOwnerB).has_value());
}

TEST_CASE("Skin scratch: a reused range is claimed for the frame it is reused in")
{
    SkinScratch scratch;
    scratch.Add(kFirstBase, kCount, kOwnerA);
    scratch.EndFrame();
    scratch.EndFrame();
    REQUIRE(scratch.Reuse(kCount, kOwnerB).has_value());
    scratch.EndFrame();
    // Survived the frame it was taken in, so another instance cannot have it.
    CHECK_FALSE(scratch.Reuse(kCount, kOwnerC).has_value());
    CHECK(scratch.Claim(kFirstBase, kOwnerB));
}

TEST_CASE("Skin scratch: a copy claiming the same range in the same frame is refused")
{
    // Two instances that write one range would draw one pose twice.
    SkinScratch scratch;
    scratch.Add(kFirstBase, kCount, kOwnerA);
    scratch.EndFrame();
    CHECK(scratch.Claim(kFirstBase, kOwnerA));
    CHECK_FALSE(scratch.Claim(kFirstBase, kOwnerA));
}

TEST_CASE("Skin scratch: an unknown range cannot be claimed")
{
    SkinScratch scratch;
    scratch.Add(kFirstBase, kCount, kOwnerA);
    CHECK_FALSE(scratch.Claim(kSecondBase, kOwnerA));
}

TEST_CASE("Skin scratch: clearing forgets every range")
{
    SkinScratch scratch;
    scratch.Add(kFirstBase, kCount, kOwnerA);
    scratch.Add(kSecondBase, kCount, kOwnerB);
    scratch.Clear();
    CHECK(scratch.RangeCount() == 0);
    CHECK_FALSE(scratch.Claim(kFirstBase, kOwnerA));
}
