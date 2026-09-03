/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Render/ShadowAtlas.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace Assisi::Render;

namespace
{
/// Whether two tiles share any texel. The defect this whole file exists to
/// catch: two overlapping tiles are two lights reading each other's depth, and
/// nothing at runtime reports it.
bool Overlaps(const ShadowViewRect &lhs, const ShadowViewRect &rhs)
{
    return lhs.x < rhs.x + rhs.width && rhs.x < lhs.x + lhs.width && lhs.y < rhs.y + rhs.height &&
           rhs.y < lhs.y + lhs.height;
}

bool InsideAtlas(const ShadowViewRect &tile, std::uint32_t resolution)
{
    return tile.x + tile.width <= resolution && tile.y + tile.height <= resolution;
}

/// Allocate until the atlas refuses, returning what it handed out.
std::vector<ShadowViewRect> DrainAt(ShadowAtlasAllocator &atlas, std::uint32_t sizeClass)
{
    std::vector<ShadowViewRect> tiles;
    for (;;)
    {
        const ShadowViewRect tile = atlas.Allocate(sizeClass);
        if (tile.width == 0)
        {
            return tiles;
        }
        tiles.push_back(tile);
    }
}
} // namespace

TEST_CASE("Size classes are the powers of two between the face-resolution bounds")
{
    CHECK(ShadowSizeClassResolution(0) == kMinShadowFaceResolution);
    CHECK(ShadowSizeClassResolution(kShadowSizeClassCount - 1u) == kMaxShadowFaceResolution);

    // Each class is twice the one below, which is what makes a demotion a
    // halving of the tile's edge and a quartering of its texels.
    for (std::uint32_t sizeClass = 1; sizeClass < kShadowSizeClassCount; ++sizeClass)
    {
        CHECK(ShadowSizeClassResolution(sizeClass) == ShadowSizeClassResolution(sizeClass - 1u) * 2u);
    }

    // The class of a resolution is the class that resolution names, and a
    // resolution between two classes rounds down to the one it fits inside.
    for (std::uint32_t sizeClass = 0; sizeClass < kShadowSizeClassCount; ++sizeClass)
    {
        CHECK(ShadowSizeClassOf(ShadowSizeClassResolution(sizeClass)) == sizeClass);
    }
    CHECK(ShadowSizeClassOf(511) == ShadowSizeClassOf(256));
    CHECK(ShadowSizeClassOf(1) == 0);
    CHECK(ShadowSizeClassOf(1u << 20) == kShadowSizeClassCount - 1u);
}

TEST_CASE("A 4096 atlas cut into 512 tiles yields exactly 64 disjoint tiles")
{
    ShadowAtlasAllocator atlas;
    atlas.Reset(4096);

    const std::uint32_t sizeClass = ShadowSizeClassOf(512);
    const std::vector<ShadowViewRect> tiles = DrainAt(atlas, sizeClass);

    // 4096 / 512 = 8 a side.
    REQUIRE(tiles.size() == 64);
    for (const ShadowViewRect &tile : tiles)
    {
        CHECK(tile.width == 512);
        CHECK(tile.height == 512);
        CHECK(InsideAtlas(tile, 4096));
        // Every tile lands on its own class's lattice, which is what makes the
        // tiling exact rather than merely non-overlapping.
        CHECK(tile.x % 512u == 0);
        CHECK(tile.y % 512u == 0);
    }
    for (std::size_t i = 0; i < tiles.size(); ++i)
    {
        for (std::size_t j = i + 1; j < tiles.size(); ++j)
        {
            CHECK_FALSE(Overlaps(tiles[i], tiles[j]));
        }
    }

    // Exactly full: 64 tiles of 512 is the whole 4096 texture, so the tiling
    // wastes nothing between tiles.
    CHECK(atlas.AllocatedTexels() == atlas.TotalTexels());
}

TEST_CASE("Available reports what the atlas can really still hand out")
{
    ShadowAtlasAllocator atlas;
    atlas.Reset(2048);

    const std::uint32_t smallClass = ShadowSizeClassOf(256);
    const std::uint32_t available = atlas.Available(smallClass);
    CHECK(available == 64); // 2048 / 256 = 8 a side

    // The count is the promise the pass commits a point light's six faces
    // against, so it has to be exact rather than optimistic: draining must
    // deliver exactly this many and no more.
    CHECK(DrainAt(atlas, smallClass).size() == available);
    CHECK(atlas.Available(smallClass) == 0);
}

TEST_CASE("Cutting a big tile costs the right number of small ones")
{
    ShadowAtlasAllocator atlas;
    atlas.Reset(2048);

    const std::uint32_t smallClass = ShadowSizeClassOf(256);
    const std::uint32_t bigClass = ShadowSizeClassOf(1024);
    const std::uint32_t before = atlas.Available(smallClass);

    const ShadowViewRect big = atlas.Allocate(bigClass);
    REQUIRE(big.width == 1024);

    // A 1024 tile is sixteen 256 tiles, and taking it must remove exactly that
    // many from what the smaller class can still serve — no more (which would
    // strand texels) and no fewer (which would promise tiles that do not exist).
    CHECK(atlas.Available(smallClass) == before - 16u);
    CHECK(DrainAt(atlas, smallClass).size() == before - 16u);
}

TEST_CASE("A mixed sequence of classes tiles the atlas without overlapping")
{
    ShadowAtlasAllocator atlas;
    atlas.Reset(1024);

    std::vector<ShadowViewRect> tiles;
    // Deliberately out of size order, which is the case a naive shelf allocator
    // gets wrong: a small tile taken first must not strand the room a large one
    // needs, and a large one taken after must not straddle the small one.
    for (const std::uint32_t resolution : {256u, 512u, 128u, 256u, 128u})
    {
        const ShadowViewRect tile = atlas.Allocate(ShadowSizeClassOf(resolution));
        REQUIRE(tile.width == resolution);
        CHECK(InsideAtlas(tile, 1024));
        CHECK(tile.x % resolution == 0);
        CHECK(tile.y % resolution == 0);
        tiles.push_back(tile);
    }
    for (std::size_t i = 0; i < tiles.size(); ++i)
    {
        for (std::size_t j = i + 1; j < tiles.size(); ++j)
        {
            CHECK_FALSE(Overlaps(tiles[i], tiles[j]));
        }
    }
}

TEST_CASE("An exhausted atlas refuses rather than handing out a tile twice")
{
    ShadowAtlasAllocator atlas;
    atlas.Reset(512);

    const std::uint32_t sizeClass = ShadowSizeClassOf(256);
    const std::vector<ShadowViewRect> tiles = DrainAt(atlas, sizeClass);
    CHECK(tiles.size() == 4);

    // The zero rect is the caller's signal to demote, and it must keep coming:
    // an allocator that started reissuing tiles once full would give two lights
    // the same texels.
    CHECK(atlas.Allocate(sizeClass).width == 0);
    CHECK(atlas.Allocate(sizeClass).width == 0);
    CHECK(atlas.Available(sizeClass) == 0);
}

TEST_CASE("A tile larger than the atlas is refused rather than silently shrunk")
{
    ShadowAtlasAllocator atlas;
    atlas.Reset(512);

    // Handing back a smaller tile would be a tile the caller did not ask for and
    // could not tell apart from the one it did — so it is biased, and clamped,
    // for a resolution it never got.
    CHECK(atlas.Allocate(ShadowSizeClassOf(1024)).width == 0);
    CHECK(atlas.Available(ShadowSizeClassOf(1024)) == 0);
    // The class that does fit is still served.
    CHECK(atlas.Allocate(ShadowSizeClassOf(512)).width == 512);
}

TEST_CASE("Reset gives every texel back")
{
    ShadowAtlasAllocator atlas;
    atlas.Reset(1024);

    const std::uint32_t sizeClass = ShadowSizeClassOf(256);
    const std::size_t first = DrainAt(atlas, sizeClass).size();
    CHECK(first == 16);
    CHECK(atlas.AllocatedTexels() == atlas.TotalTexels());

    // The atlas is rebuilt from nothing every frame, so a Reset that leaked
    // would shrink the usable atlas a little more with every frame drawn.
    atlas.Reset(1024);
    CHECK(atlas.AllocatedTexels() == 0);
    CHECK(DrainAt(atlas, sizeClass).size() == first);
}

TEST_CASE("An atlas wider than the largest tile holds several of them")
{
    ShadowAtlasAllocator atlas;
    atlas.Reset(8192);

    // The top size class is capped at kMaxShadowFaceResolution, so an 8192 atlas
    // is a grid of those rather than one root node — laying out a single root
    // would leave everything past the first 2048 unreachable.
    const std::uint32_t topClass = kShadowSizeClassCount - 1u;
    const std::vector<ShadowViewRect> tiles = DrainAt(atlas, topClass);
    CHECK(tiles.size() == (8192u / kMaxShadowFaceResolution) * (8192u / kMaxShadowFaceResolution));
    CHECK(atlas.AllocatedTexels() == atlas.TotalTexels());
}

TEST_CASE("An atlas smaller than the smallest tile holds nothing")
{
    ShadowAtlasAllocator atlas;
    atlas.Reset(64);

    // Degrades to serving nothing rather than to a rect running off the texture.
    CHECK(atlas.Available(0) == 0);
    CHECK(atlas.Allocate(0).width == 0);
}
