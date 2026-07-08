/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file RenderFrame.hpp
/// @brief Backend-neutral handle to the render target acquired for the current frame.
///
/// The fields are all backend-agnostic `nvrhi::` interfaces, so this type does
/// not name a specific graphics API — the Vulkan backend produces it today, but
/// a future D3D12 backend would produce the same struct unchanged. App/game code
/// (an `OnRender` override) only ever sees this; it never touches the backend
/// context that created it.

#include <cstdint>

#include <nvrhi/nvrhi.h>

namespace Assisi::Render
{

/// @brief Resources for the swapchain image acquired for the current frame.
struct RenderFrame
{
    nvrhi::ICommandList *commandList = nullptr;
    nvrhi::ITexture     *colorTexture = nullptr;
    nvrhi::ITexture     *depthTexture = nullptr;
    nvrhi::IFramebuffer *framebuffer = nullptr;
    uint32_t             width = 0;
    uint32_t             height = 0;
};

} // namespace Assisi::Render
