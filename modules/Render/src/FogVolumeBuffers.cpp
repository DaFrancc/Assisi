/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/FogVolumeBuffers.hpp>

#include <Assisi/Core/Logger.hpp>

#include <cstdint>

namespace Assisi::Render
{
namespace
{
// An offset and a count per square, then every square's entries.
constexpr uint32_t kTileWords = kMaxFogTiles * 2u + kMaxFogTileEntries;
} // namespace

bool FogVolumeBuffers::Initialize(nvrhi::IDevice *device)
{
    _volumes.Create(device, sizeof(FogVolumeGpu), kMaxFogVolumes, false, "FogVolumes::Volumes");
    _edges.Create(device, sizeof(FogOutlineEdgeGpu), kMaxFogOutlineEdges, false, "FogVolumes::Edges");
    _tiles.Create(device, sizeof(uint32_t), kTileWords, false, "FogVolumes::Tiles");
    if (!_volumes.IsValid() || !_edges.IsValid() || !_tiles.IsValid())
    {
        Core::Log::Error("FogVolumeBuffers: failed to create the fog volume buffers.");
        return false;
    }
    return true;
}

void FogVolumeBuffers::Upload(nvrhi::ICommandList *commandList, const FogVolumeFrame &frame) const
{
    if (frame.Lanes().x == 0u)
    {
        return;
    }
    _volumes.Upload(commandList, frame.volumes.data(), static_cast<uint32_t>(frame.volumes.size()));
    if (!frame.edges.empty())
    {
        _edges.Upload(commandList, frame.edges.data(), static_cast<uint32_t>(frame.edges.size()));
    }
    _tiles.Upload(commandList, frame.tiles.data(), static_cast<uint32_t>(frame.tiles.size()));
}

} // namespace Assisi::Render
