/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file FogVolumeBuffers.hpp
/// @brief The buffers the placed fog volumes reach the shaders through.

#include <Assisi/Render/Buffer.hpp>
#include <Assisi/Render/FogVolume.hpp>

#include <cstdint>

#include <nvrhi/nvrhi.h>

namespace Assisi::Render
{

/// @brief One frame's volumes, their outline edges and the screen squares' lists
/// of them, read by the mesh pass and the sky pass at the same slots.
///
/// Fixed capacity, from FogVolume.hpp's limits, so a binding set made once names
/// them for good.
class FogVolumeBuffers
{
public:
    /// @brief Where each buffer sits in both passes' register space. Must match
    /// sky/fog_volumes.glsl.
    enum class Slot : std::uint8_t
    {
        Volumes = 13,
        Edges = 14,
        Tiles = 15,
    };

    [[nodiscard]] bool Initialize(nvrhi::IDevice *device);

    /// @brief Uploads @p frame. A frame with no volumes uploads nothing: its
    /// lanes say so, and no shader reads the buffers.
    void Upload(nvrhi::ICommandList *commandList, const FogVolumeFrame &frame) const;

    [[nodiscard]] const Buffer &Volumes() const { return _volumes; }
    [[nodiscard]] const Buffer &Edges() const { return _edges; }
    [[nodiscard]] const Buffer &Tiles() const { return _tiles; }

private:
    Buffer _volumes;
    Buffer _edges;
    Buffer _tiles;
};

} // namespace Assisi::Render
