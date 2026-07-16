/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file OutlinePass.hpp
/// @brief Always-on-top orange selection outline via screen-space edge detection.

#include <cstdint>
#include <string>

#include <nvrhi/nvrhi.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/RenderFrame.hpp>

namespace Assisi::Render
{

class MeshBuffer;

/// @brief Draws a selection outline — an orange border around one mesh, rendered
/// over everything so it stays visible even when the object is occluded.
///
/// Technique — the same screen-space edge detect Unreal uses, which (unlike a
/// normal-extruded "hull" outline) is clean on any topology, boxes included:
///   1. Mask pass — render the selected mesh's silhouette into a single-channel
///      offscreen coverage mask (depth test off, so the whole silhouette is
///      covered even through occluders).
///   2. Edge pass — a fullscreen triangle reads that mask and paints orange on
///      pixels just OUTSIDE the silhouette (within a few pixels of a covered
///      texel), giving a uniform-thickness border composited over the scene.
///
/// The mask is an internal R8 render target sized to the viewport, recreated when
/// the viewport changes. The mask pipeline targets that mask's format; the edge
/// pipeline targets the scene framebuffer's format (rebuilt on an MSAA change).
class OutlinePass
{
  public:
    /// @param sceneFramebufferInfo  Format/samples of the framebuffer the edge
    ///        pass composites into (the scene target).
    /// @param width,height          Initial viewport size for the coverage mask.
    /// @param billboardMaskVertexShaderSpvPath  Vertex stage that generates a
    ///        camera-facing quad from gl_VertexIndex (the entity-icon billboard
    ///        shader) — used to outline a meshless entity's icon.
    /// @param billboardMaskPixelShaderSpvPath   Pixel stage that samples the icon
    ///        and writes coverage only where it is opaque, so the outline traces the
    ///        icon's artwork rather than its bounding quad.
    [[nodiscard]] bool Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &sceneFramebufferInfo,
                                  uint32_t width, uint32_t height, const std::string &maskVertexShaderSpvPath,
                                  const std::string &maskPixelShaderSpvPath,
                                  const std::string &edgeVertexShaderSpvPath,
                                  const std::string &edgePixelShaderSpvPath,
                                  const std::string &billboardMaskVertexShaderSpvPath,
                                  const std::string &billboardMaskPixelShaderSpvPath);

    /// @brief Rebuild the edge pipeline against a new scene render-target format
    /// (e.g. an MSAA toggle). The mask pipeline/target are unaffected. No-op
    /// (true) before Initialize().
    [[nodiscard]] bool RebuildPipeline(const nvrhi::FramebufferInfo &sceneFramebufferInfo);

    [[nodiscard]] bool IsValid() const { return _maskPipeline != nullptr && _edgePipeline != nullptr; }

    /// @brief Record the outline for @p mesh placed at @p model. @p viewProjection
    /// is the camera's projection*view. Draws LOD0 (or the whole mesh if it has no
    /// LOD table). Resizes the mask to the frame if needed. No-op if not
    /// initialised or the mesh has no GPU buffers.
    void Draw(const RenderFrame &frame, const glm::mat4 &viewProjection, const MeshBuffer &mesh,
              const glm::mat4 &model);

    /// @brief Outline a meshless entity's icon — the selection highlight for an
    /// entity drawn as a billboard. The quad is centred at @p center, spanning
    /// ±@p halfSize along the (unit) @p cameraRight / @p cameraUp axes, matching the
    /// drawn icon; @p iconTexture is sampled so the outline traces the icon's
    /// artwork. @p viewProjection is projection*view. No-op if not initialised or
    /// @p iconTexture is null.
    void DrawBillboard(const RenderFrame &frame, const glm::mat4 &viewProjection, const glm::vec3 &center,
                       const glm::vec3 &cameraRight, const glm::vec3 &cameraUp, float halfSize,
                       nvrhi::ITexture *iconTexture);

  private:
    [[nodiscard]] bool BuildEdgePipeline(const nvrhi::FramebufferInfo &sceneFramebufferInfo);
    /// @brief (Re)create the coverage mask target + its framebuffer and the edge
    /// pass's binding set for a new viewport size. No-op when already that size.
    [[nodiscard]] bool EnsureMask(uint32_t width, uint32_t height);
    void RecordMaskPass(const RenderFrame &frame, const glm::mat4 &modelViewProjection, const MeshBuffer &mesh);
    void RecordBillboardMaskPass(const RenderFrame &frame, const glm::mat4 &viewProjection, const glm::vec3 &center,
                                 const glm::vec3 &cameraRight, const glm::vec3 &cameraUp, float halfSize);
    /// @brief (Re)build the billboard mask's binding set for @p iconTexture, reusing
    /// it while the texture is unchanged. Returns false if there is no texture.
    [[nodiscard]] bool EnsureBillboardBindingSet(nvrhi::ITexture *iconTexture);
    void RecordEdgePass(const RenderFrame &frame);

    nvrhi::IDevice *_device = nullptr;

    // Mask pass: renders the silhouette into the R8 coverage target.
    nvrhi::ShaderHandle           _maskVertexShader;
    nvrhi::ShaderHandle           _maskPixelShader;
    nvrhi::InputLayoutHandle      _maskInputLayout;
    nvrhi::BindingLayoutHandle    _maskBindingLayout;
    nvrhi::BindingSetHandle       _maskBindingSet; // push constants only
    nvrhi::GraphicsPipelineHandle _maskPipeline;

    // Billboard mask variant: stamps a meshless entity's icon into the same
    // coverage mask (sampling the icon so only its opaque artwork counts), so the
    // edge pass outlines the icon's shape. Shares the coverage target and edge
    // pass; has its own quad vertex stage, icon-sampling pixel stage, sampler, and
    // a binding set rebuilt only when the icon texture changes.
    nvrhi::ShaderHandle           _billboardMaskVertexShader;
    nvrhi::ShaderHandle           _billboardMaskPixelShader;
    nvrhi::BindingLayoutHandle    _billboardMaskBindingLayout;
    nvrhi::SamplerHandle          _billboardSampler;
    nvrhi::BindingSetHandle       _billboardMaskBindingSet; // push constants + icon texture + sampler
    nvrhi::ITexture              *_billboardTexture = nullptr; // non-owning; identifies the current binding set
    nvrhi::GraphicsPipelineHandle _billboardMaskPipeline;

    // Edge pass: fullscreen edge detect that composites the orange border.
    nvrhi::ShaderHandle           _edgeVertexShader;
    nvrhi::ShaderHandle           _edgePixelShader;
    nvrhi::BindingLayoutHandle    _edgeBindingLayout;
    nvrhi::SamplerHandle          _sampler;
    nvrhi::GraphicsPipelineHandle _edgePipeline;
    nvrhi::BindingSetHandle       _edgeBindingSet; // depends on the mask texture

    // The offscreen coverage mask, sized to the viewport and recreated on resize.
    nvrhi::TextureHandle     _maskTexture;
    nvrhi::FramebufferHandle _maskFramebuffer;
    uint32_t                 _maskWidth  = 0;
    uint32_t                 _maskHeight = 0;
};

} // namespace Assisi::Render
