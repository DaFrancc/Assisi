/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/ShadowAtlas.hpp>

#include <algorithm>

namespace Assisi::Render
{
namespace
{
/// @brief The largest power of two at or below @p value, or zero for zero.
std::uint32_t FloorPowerOfTwo(std::uint32_t value)
{
    std::uint32_t power = 1;
    while (power <= value / 2u)
    {
        power *= 2u;
    }
    return value == 0u ? 0u : power;
}
} // namespace

void ShadowAtlasAllocator::Reset(std::uint32_t resolution)
{
    _resolution = resolution;
    _tiledResolution = FloorPowerOfTwo(resolution);
    _allocatedTexels = 0;

    for (std::vector<ShadowViewRect> &nodes : _free)
    {
        nodes.clear();
    }

    // The atlas starts as a grid of the largest tiles that fit, rather than as
    // one root node: the top size class is capped at kMaxShadowFaceResolution,
    // and an atlas wider than that holds several of them side by side.
    const std::uint32_t rootSize = ShadowSizeClassResolution(RootClass());
    if (_tiledResolution < rootSize)
    {
        return; // an atlas smaller than the smallest tile holds nothing
    }
    const std::uint32_t across = _tiledResolution / rootSize;
    _free[RootClass()].reserve(static_cast<std::size_t>(across) * across);
    for (std::uint32_t y = 0; y < across; ++y)
    {
        for (std::uint32_t x = 0; x < across; ++x)
        {
            _free[RootClass()].push_back(
                ShadowViewRect{.x = x * rootSize, .y = y * rootSize, .width = rootSize, .height = rootSize});
        }
    }
}

std::uint32_t ShadowAtlasAllocator::RootClass() const
{
    // The largest class the atlas can hold at all. A 512 atlas cannot hand out a
    // 2048 tile, so its root is 512's class and everything below splits from
    // there.
    std::uint32_t sizeClass = kShadowSizeClassCount - 1u;
    while (sizeClass > 0u && ShadowSizeClassResolution(sizeClass) > _tiledResolution)
    {
        --sizeClass;
    }
    return sizeClass;
}

std::uint32_t ShadowAtlasAllocator::Available(std::uint32_t sizeClass) const
{
    if (_tiledResolution == 0u || sizeClass >= kShadowSizeClassCount ||
        ShadowSizeClassResolution(sizeClass) > _tiledResolution)
    {
        return 0;
    }
    // A free node of a bigger class splits into four of the class below — both
    // axes halve at once — so the count accumulates downward from the root.
    std::uint32_t available = 0;
    for (std::uint32_t klass = RootClass() + 1u; klass-- > sizeClass;)
    {
        available = available * 4u + static_cast<std::uint32_t>(_free[klass].size());
    }
    return available;
}

std::uint64_t ShadowAtlasAllocator::TotalTexels() const
{
    return static_cast<std::uint64_t>(_tiledResolution) * _tiledResolution;
}

bool ShadowAtlasAllocator::Split(std::uint32_t sizeClass)
{
    // The top class has nothing above it to split from: Reset laid every one of
    // those out, and when they are gone the atlas is full.
    if (sizeClass >= RootClass())
    {
        return false;
    }
    const std::uint32_t parentClass = sizeClass + 1u;
    if (_free[parentClass].empty() && !Split(parentClass))
    {
        return false;
    }

    const ShadowViewRect parent = _free[parentClass].back();
    _free[parentClass].pop_back();

    const std::uint32_t half = parent.width / 2u;
    _free[sizeClass].push_back(ShadowViewRect{.x = parent.x, .y = parent.y, .width = half, .height = half});
    _free[sizeClass].push_back(ShadowViewRect{.x = parent.x + half, .y = parent.y, .width = half, .height = half});
    _free[sizeClass].push_back(ShadowViewRect{.x = parent.x, .y = parent.y + half, .width = half, .height = half});
    _free[sizeClass].push_back(
        ShadowViewRect{.x = parent.x + half, .y = parent.y + half, .width = half, .height = half});
    return true;
}

ShadowViewRect ShadowAtlasAllocator::Allocate(std::uint32_t sizeClass)
{
    sizeClass = std::min(sizeClass, kShadowSizeClassCount - 1u);
    // A tile bigger than the atlas cannot be served at any class, and clamping
    // it to the root here rather than failing would hand back a tile the caller
    // did not ask for and could not tell apart from the one it did.
    if (_tiledResolution == 0u || ShadowSizeClassResolution(sizeClass) > _tiledResolution)
    {
        return ShadowViewRect{};
    }

    if (_free[sizeClass].empty() && !Split(sizeClass))
    {
        return ShadowViewRect{};
    }

    const ShadowViewRect tile = _free[sizeClass].back();
    _free[sizeClass].pop_back();
    _allocatedTexels += static_cast<std::uint64_t>(tile.width) * tile.height;
    return tile;
}

} // namespace Assisi::Render
