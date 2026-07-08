/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Buffer.hpp
/// @brief Generic fixed-capacity NVRHI structured buffer.
///
/// Used for compute-shader SSBO-equivalents (structured/storage buffers) —
/// distinct from `Render::MeshBuffer`, which is vertex/index data only.
///
/// Capacity is fixed at Create() time and never resized: callers that upload
/// a variable number of live elements each frame (e.g. clustered-lighting
/// light lists) size the buffer to a generous worst case up front and upload
/// only the live prefix, rather than reallocating every frame.

#include <cstdint>

#include <nvrhi/nvrhi.h>

namespace Assisi::Render
{

/// @brief Owner of a single NVRHI structured buffer, usable as an SRV
/// (read in shaders) and, optionally, a UAV (written by compute shaders).
class Buffer
{
  public:
    Buffer() = default;

    /// @brief Allocates a new buffer sized for `capacityElements` elements of
    /// `elementStride` bytes each, replacing any buffer already owned.
    /// @param allowUnorderedAccess  true if a compute shader will write this
    /// buffer (UAV); false for buffers only ever read by shaders (SRV).
    void Create(nvrhi::IDevice *device, uint32_t elementStride, uint32_t capacityElements,
               bool allowUnorderedAccess, const char *debugName);

    /// @brief Writes `elementCount` elements of `data` starting at element 0.
    /// Elements beyond `CapacityElements()` are silently dropped.
    void Upload(nvrhi::ICommandList *commandList, const void *data, uint32_t elementCount) const;

    /// @brief Fills the entire buffer with zero bytes (e.g. resetting atomic counters).
    void ClearToZero(nvrhi::ICommandList *commandList) const;

    bool IsValid() const { return _buffer != nullptr; }
    nvrhi::IBuffer *NativeBuffer() const { return _buffer; }
    uint32_t CapacityElements() const { return _capacityElements; }

  private:
    nvrhi::BufferHandle _buffer;
    uint32_t _elementStride = 0;
    uint32_t _capacityElements = 0;
};

} // namespace Assisi::Render
