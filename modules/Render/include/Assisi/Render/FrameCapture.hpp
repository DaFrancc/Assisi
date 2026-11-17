/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file FrameCapture.hpp
/// @brief One rendered frame, off the GPU and onto disk as a PNG.
///
/// The copy is recorded into the frame's own command list, so it can be taken at
/// any point in the frame — before the debug UI draws, for a picture of the scene
/// alone. Write reads it back by waiting for the GPU, which suits the one frame a
/// capture ends on. A frame that must not stall instead marks the submission,
/// polls Ready on later frames, and hands Read's pixels to WritePng on a worker.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nvrhi/nvrhi.h>

namespace Assisi::Render
{

/// @brief One captured frame's pixels: 8-bit RGBA, rows top to bottom.
struct CapturedImage
{
    std::vector<std::uint8_t> rgba;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

class FrameCapture
{
public:
    /// @brief Records a copy of @p source onto @p commandList.
    ///
    /// @p source stays in whatever state the frame needs next; nvrhi transitions
    /// it for the copy and back. @return false if the staging texture could not
    /// be allocated.
    [[nodiscard]] bool Record(nvrhi::IDevice *device, nvrhi::ICommandList *commandList, nvrhi::ITexture *source);

    /// @brief Marks the end of the work the copy belongs to, so Ready can tell
    /// when the GPU has finished it without waiting. Call after the list Record
    /// wrote to has been submitted. @return false if no query could be made.
    [[nodiscard]] bool MarkSubmitted(nvrhi::IDevice *device);

    /// @brief Whether the GPU has finished the copy MarkSubmitted marked. Never
    /// waits.
    [[nodiscard]] bool Ready(nvrhi::IDevice *device) const;

    /// @brief The copied pixels. Call once Ready, or with the device idle.
    ///
    /// Empty, with the reason logged, if nothing was recorded or the map failed.
    [[nodiscard]] std::optional<CapturedImage> Read(nvrhi::IDevice *device);

    /// @brief Writes @p image to @p path as a PNG. Touches no GPU state, so it
    /// runs on any thread. @return false, with the reason logged, if the write
    /// failed.
    [[nodiscard]] static bool WritePng(const CapturedImage &image, const std::string &path);

    /// @brief Waits for the GPU, then writes the recorded copy to @p path as an
    /// 8-bit RGBA PNG. Call after the list Record wrote to has been submitted.
    ///
    /// @return false if nothing was recorded, or the map or the file write failed;
    /// each logs its own reason.
    [[nodiscard]] bool Write(nvrhi::IDevice *device, const std::string &path);

private:
    nvrhi::StagingTextureHandle _staging;
    nvrhi::EventQueryHandle _submitted;
    std::uint32_t _width = 0;
    std::uint32_t _height = 0;
    nvrhi::Format _format = nvrhi::Format::UNKNOWN;
};

} // namespace Assisi::Render
