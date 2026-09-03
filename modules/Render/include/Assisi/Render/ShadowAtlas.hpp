/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ShadowAtlas.hpp
/// @brief Cutting square power-of-two tiles out of one square shadow texture.
///
/// Every spot light's map and every face of every point light is a rectangle of
/// the same texture, and this is what hands those rectangles out. It is a buddy
/// allocator: the atlas is one node, a node too big for the request splits into
/// four quadrants, and the smallest free node of the requested size wins. That
/// gives exact tiling with no wasted texels between tiles and no fragmentation
/// that a reset does not clear — which matters because the atlas is rebuilt from
/// nothing every frame, so there is never a long-lived free list to degrade.
///
/// Sizes are size *classes* rather than free numbers. A class is a power of two
/// between kMinShadowFaceResolution and kMaxShadowFaceResolution, and demoting a
/// light means moving it one class down. Restricting to powers of two is what
/// makes the quadrant split exact: a tile that is not a power of two either
/// leaves a gap or overlaps its neighbour, and an overlapping tile is one light
/// reading another's depth.
///
/// Device-free by construction. The whole of what an allocator decides is
/// arithmetic, and none of it needs a texture to be checked.

#include <cstdint>
#include <vector>

#include <Assisi/Render/ShadowSettings.hpp>
#include <Assisi/Render/ShadowView.hpp>

namespace Assisi::Render
{

/// @brief How many size classes there are between the face-resolution bounds,
/// inclusive of both.
///
/// 128, 256, 512, 1024, 2048 — five, and the count is derived rather than
/// written down so moving either bound moves this with it.
[[nodiscard]] constexpr std::uint32_t ShadowSizeClassCount()
{
    std::uint32_t count = 1;
    for (std::uint32_t size = kMinShadowFaceResolution; size < kMaxShadowFaceResolution; size *= 2u)
    {
        ++count;
    }
    return count;
}

inline constexpr std::uint32_t kShadowSizeClassCount = ShadowSizeClassCount();

/// @brief The tile edge, in texels, of size class @p sizeClass.
///
/// Class 0 is the smallest tile, so a *higher* class is a bigger tile and
/// demoting a light is subtracting one. That direction is the one the caller
/// wants: overflow demotes, and `class - 1` reads as less.
[[nodiscard]] constexpr std::uint32_t ShadowSizeClassResolution(std::uint32_t sizeClass)
{
    if (sizeClass >= kShadowSizeClassCount)
    {
        sizeClass = kShadowSizeClassCount - 1u;
    }
    return kMinShadowFaceResolution << sizeClass;
}

/// @brief The class whose tile is @p resolution, rounded down.
///
/// Rounded down rather than to the nearest, so a resolution between two classes
/// gets the one it fits inside. A resolution under the floor gets class 0 and one
/// over the ceiling gets the top class, which is the same clamping every other
/// resolution here takes.
[[nodiscard]] constexpr std::uint32_t ShadowSizeClassOf(std::uint32_t resolution)
{
    std::uint32_t sizeClass = 0;
    while (sizeClass + 1u < kShadowSizeClassCount && ShadowSizeClassResolution(sizeClass + 1u) <= resolution)
    {
        ++sizeClass;
    }
    return sizeClass;
}

/// @brief One square texture, cut into square power-of-two tiles.
///
/// Reset() every frame and allocate into it; the allocator holds no memory that
/// outlives a frame's assignment, so a light losing its tile costs nothing to
/// undo.
class ShadowAtlasAllocator
{
public:
    ShadowAtlasAllocator() = default;

    /// @brief Point the allocator at an atlas of @p resolution texels a side and
    /// free everything.
    ///
    /// @p resolution is expected to be a power of two — LocalShadowSettings
    /// guarantees one — and a non-power-of-two is used as-is for the rect bounds
    /// while the tiling is computed from the power of two at or below it, which
    /// leaves the strip past that edge unallocated rather than handing out a tile
    /// that runs off the texture.
    void Reset(std::uint32_t resolution);

    /// @brief The atlas edge the last Reset() was given.
    [[nodiscard]] std::uint32_t Resolution() const { return _resolution; }

    /// @brief Cut a tile of size class @p sizeClass out of the atlas.
    ///
    /// @return the tile, or a rect of zero width when the atlas has no free node
    /// that big left. A zero rect is the caller's signal to demote and try again,
    /// which is what turns an overflow into a smaller shadow rather than no
    /// shadow.
    [[nodiscard]] ShadowViewRect Allocate(std::uint32_t sizeClass);

    /// @brief How many more tiles of size class @p sizeClass the atlas can still
    /// hand out.
    ///
    /// Counts what splitting would yield as well as what is already free, so it
    /// is the real answer rather than the free list's length. A caller that needs
    /// several tiles at once — a point light needs six, and five of six is a
    /// light with a hole in it — asks this before committing to any of them,
    /// because a tile once handed out cannot be given back until Reset().
    [[nodiscard]] std::uint32_t Available(std::uint32_t sizeClass) const;

    /// @brief Texels handed out since the last Reset().
    [[nodiscard]] std::uint64_t AllocatedTexels() const { return _allocatedTexels; }

    /// @brief Texels the atlas holds in total, which AllocatedTexels is a
    /// fraction of. Zero before the first Reset().
    [[nodiscard]] std::uint64_t TotalTexels() const;

private:
    /// @brief The largest class the atlas itself can hold, which is where a
    /// split starts. Clamped to the top class, so an 8192 atlas still splits
    /// down from 2048 tiles rather than trying to hand out one 8192 tile.
    [[nodiscard]] std::uint32_t RootClass() const;

    /// @brief Split one free node of class @p sizeClass into four of the class
    /// below, returning false when there is none to split.
    ///
    /// Recursive through the class above it, so asking for a small tile of an
    /// empty atlas splits the whole chain down in one call.
    [[nodiscard]] bool Split(std::uint32_t sizeClass);

    // Free tiles per size class, smallest class first. A vector rather than a
    // tree: a node is fully described by its rect, and a buddy allocator that
    // never coalesces — which this does not, because Reset is the only free —
    // needs nothing else.
    std::vector<ShadowViewRect> _free[kShadowSizeClassCount];
    std::uint32_t _resolution = 0;
    // The power-of-two edge the tiling is laid out on, at or below _resolution.
    std::uint32_t _tiledResolution = 0;
    std::uint64_t _allocatedTexels = 0;
};

} // namespace Assisi::Render
