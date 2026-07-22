/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file LinePass.hpp
/// @brief Generic world-space coloured 3D line renderer.

#include <cstdint>
#include <span>
#include <string>

#include <nvrhi/nvrhi.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/RenderFrame.hpp>

namespace Assisi::Render
{

/// @brief One endpoint of a line segment: a world-space position and an RGBA
/// colour written straight to the scene target. Segments are drawn as a LineList,
/// so vertices come in consecutive pairs (0-1, 2-3, ...).
struct LineVertex
{
    glm::vec3 position;
    glm::vec4 color;
};

/// @brief Draws coloured line segments in world space — a minimal debug/overlay
/// line facility with no knowledge of what it is drawing (the editor uses it for
/// collider wireframes). Vertices are a LineList: each consecutive pair is one
/// segment.
///
/// Two draw modes share one vertex format and shader, differing only in depth:
///   - depth-tested — occluded by scene geometry (no depth write), so lines read
///     as sitting in the world;
///   - on-top — depth test off, always visible through geometry (x-ray).
/// The caller chooses per Draw() call. The pass owns a single dynamic vertex
/// buffer that grows to fit the largest batch it is handed.
class LinePass
{
  public:
    /// @param sceneFramebufferInfo  Format/samples of the framebuffer the lines
    ///        composite into (the scene target, including its depth attachment).
    [[nodiscard]] bool Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &sceneFramebufferInfo,
                                  const std::string &vertexShaderSpvPath, const std::string &pixelShaderSpvPath);

    /// @brief Rebuild both pipelines for a new scene render-target format (e.g. an
    /// MSAA toggle). Shaders, input layout and binding set are reused. No-op (true)
    /// before Initialize().
    [[nodiscard]] bool RebuildPipeline(const nvrhi::FramebufferInfo &sceneFramebufferInfo);

    [[nodiscard]] bool IsValid() const { return _depthTestedPipeline != nullptr && _onTopPipeline != nullptr; }

    /// @brief Record one LineList batch. @p viewProjection is the camera's
    /// projection*view; @p onTop selects the x-ray (depth-test-off) pipeline. The
    /// vertex buffer grows if @p vertices exceeds its capacity. No-op if not
    /// initialised or @p vertices is empty (or has an odd count — the last stray
    /// vertex is dropped).
    void Draw(const RenderFrame &frame, const glm::mat4 &viewProjection, std::span<const LineVertex> vertices,
              bool onTop);

  private:
    [[nodiscard]] bool BuildPipelines(const nvrhi::FramebufferInfo &sceneFramebufferInfo);
    /// @brief Grow buffer @p slot to hold at least @p vertexCount vertices, reusing
    /// it while it is already large enough. Returns false on a failed allocation.
    /// Each depth mode owns a slot so a frame's two Draw() calls never write the
    /// same buffer (which would be a write-after-read hazard on one command list).
    [[nodiscard]] bool EnsureVertexCapacity(int32_t slot, uint32_t vertexCount);

    nvrhi::IDevice *_device = nullptr;

    nvrhi::ShaderHandle        _vertexShader;
    nvrhi::ShaderHandle        _pixelShader;
    nvrhi::InputLayoutHandle   _inputLayout;
    nvrhi::BindingLayoutHandle _bindingLayout;
    nvrhi::BindingSetHandle    _bindingSet; // push constants only

    // Two pipelines over the same state, differing only in depth-test enable.
    nvrhi::GraphicsPipelineHandle _depthTestedPipeline;
    nvrhi::GraphicsPipelineHandle _onTopPipeline;

    // One dynamic vertex buffer per depth mode (slot 0 = depth-tested, 1 = on-top),
    // each grown to fit the largest batch it has drawn. Separate buffers so the two
    // Draw() calls in a frame never write the same resource.
    nvrhi::BufferHandle _vertexBuffers[2];
    uint32_t            _vertexCapacities[2] = {0, 0}; // in vertices
};

} // namespace Assisi::Render
