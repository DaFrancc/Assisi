/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file FrameCapture.hpp
/// @brief One rendered frame, off the GPU and onto disk as a PNG.
///
/// The copy is recorded into the frame's own command list, so it can be taken at
/// any point in the frame — before the debug UI draws, for a picture of the scene
/// alone. Reading it back waits for the GPU, which makes this fit for the one
/// frame a capture ends on and nothing else.

#include <cstdint>
#include <string>

#include <nvrhi/nvrhi.h>

namespace Assisi::Render
{

class FrameCapture
{
public:
    /// @brief Records a copy of @p source onto @p commandList.
    ///
    /// @p source stays in whatever state the frame needs next; nvrhi transitions
    /// it for the copy and back. @return false if the staging texture could not
    /// be allocated.
    [[nodiscard]] bool Record(nvrhi::IDevice *device, nvrhi::ICommandList *commandList, nvrhi::ITexture *source);

    /// @brief Waits for the GPU, then writes the recorded copy to @p path as an
    /// 8-bit RGBA PNG. Call after the list Record wrote to has been submitted.
    ///
    /// @return false if nothing was recorded, or the map or the file write failed;
    /// each logs its own reason.
    [[nodiscard]] bool Write(nvrhi::IDevice *device, const std::string &path);

private:
    nvrhi::StagingTextureHandle _staging;
    std::uint32_t _width = 0;
    std::uint32_t _height = 0;
    nvrhi::Format _format = nvrhi::Format::UNKNOWN;
};

} // namespace Assisi::Render
