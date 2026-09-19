/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file QuadPass.hpp
/// @brief Draws a Mondrian draw list into a display-encoded target.
///
/// Colours are written as they are authored, with no tone map after them, and
/// blended premultiplied. The pass targets whatever frame it is handed, so the
/// swapchain is one target among several rather than the only one it knows.

#include <Assisi/Mondrian/DrawList.hpp>
#include <Assisi/Render/RenderFrame.hpp>

#include <nvrhi/nvrhi.h>

namespace Assisi::Mondrian::Engine
{

class QuadPass
{
public:
    /// @brief Builds the pipeline against @p framebufferInfo, which every frame
    /// later handed to Draw must match. False, logged, when a shader or the
    /// pipeline cannot be built.
    [[nodiscard]] bool Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &framebufferInfo);

    /// @brief Records @p drawList into @p frame's framebuffer. A no-op before a
    /// successful Initialize and for an empty list.
    void Draw(const Render::RenderFrame &frame, const DrawList &drawList);

    /// @brief Releases every GPU handle. Must run before the device is destroyed.
    void Shutdown();

private:
    nvrhi::ShaderHandle _vertexShader;
    nvrhi::ShaderHandle _pixelShader;
    nvrhi::BindingLayoutHandle _bindingLayout;
    nvrhi::BindingSetHandle _bindingSet;
    nvrhi::GraphicsPipelineHandle _pipeline;
};

} // namespace Assisi::Mondrian::Engine
