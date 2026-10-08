/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SkinScratch.hpp
/// @brief Which ranges of the geometry arena hold skinned instances' posed
///        vertices, and who owns each.
///
/// The arena only grows, so a range is never given back to it. A range whose
/// owner stops claiming it is freed here instead, and the next instance with
/// the same vertex count reuses it. Owners claim their range every frame; one
/// not claimed by the end of a frame is free, which is how a despawned or
/// rebound instance lets go without anyone telling this table.
///
/// Device-free: it decides where vertices go, the arena holds them.

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Assisi::Render
{

class SkinScratch
{
public:
    /// @brief Claims a free range of exactly @p vertexCount vertices for
    ///        @p owner and returns its vertex base, or nothing when none is free.
    [[nodiscard]] std::optional<uint32_t> Reuse(uint32_t vertexCount, uint32_t owner);

    /// @brief Records a range just allocated from the arena as @p owner's,
    ///        claimed for this frame.
    void Add(uint32_t vertexBase, uint32_t vertexCount, uint32_t owner);

    /// @brief Claims @p owner's range at @p vertexBase for this frame.
    ///
    /// False when the range is no longer @p owner's, or was already claimed this
    /// frame: a copy of an instance carries the same range, and two instances
    /// writing one range would draw one pose twice. The caller then acquires a
    /// range of its own.
    [[nodiscard]] bool Claim(uint32_t vertexBase, uint32_t owner);

    /// @brief Frees every range not claimed since the last EndFrame.
    void EndFrame();

    /// @brief Forgets every range, for an arena that was just reset.
    void Clear();

    /// @brief Ranges held, owned or free.
    [[nodiscard]] uint32_t RangeCount() const { return static_cast<uint32_t>(_ranges.size()); }

private:
    struct Range
    {
        uint32_t vertexBase = 0;
        uint32_t vertexCount = 0;
        uint32_t owner = kFree;
        bool claimed = false;
    };

    /// An owner no instance is given; the range is free.
    static constexpr uint32_t kFree = 0;

    std::vector<Range> _ranges;
    std::unordered_map<uint32_t, uint32_t> _rangeAt; ///< Vertex base → index into _ranges.
};

} // namespace Assisi::Render
