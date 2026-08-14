/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file OutlinePass.hpp
/// @brief Coloured always-on-top / depth-tested silhouette outline via
/// screen-space edge detection.

#include <cstdint>
#include <span>
#include <string>

#include <nvrhi/nvrhi.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/RenderFrame.hpp>

namespace Assisi::Render
{

class MeshBuffer;

/// @brief Draws a selection/collider outline — a coloured border around one or
/// more meshes, drawn over the scene via screen-space edge detection.
///
/// Technique — the same screen-space edge detect Unreal uses, which (unlike a
/// normal-extruded "hull" outline) is clean on any topology, boxes included:
///   1. Mask pass — render each silhouette into a single-channel offscreen
///      coverage mask. Depth test off ("always on top"), or tested against the
///      scene depth so occluded parts of the silhouette drop out.
///   2. Edge pass — a fullscreen triangle reads that mask and paints the caller's
///      colour on pixels just OUTSIDE the covered region.
///
/// A batch of meshes rendered into one mask yields one merged outline of one
/// colour (their union). Call once per outline group.
///
/// The outline is always drawn on top of the scene (not depth-tested) — it is a
/// selection highlight, so it stays visible even when the object is occluded, the
/// way editor selection outlines conventionally behave.
class OutlinePass
{
public:
    /// @brief One silhouette to stamp into the mask: a mesh at a world transform.
    struct OutlineItem
    {
        const MeshBuffer *mesh;
        glm::mat4 model;
    };

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

    /// @brief Outline @p mesh at @p model in @p color, on top of the scene.
    /// @p viewProjection is the camera's projection*view. No-op if not initialised
    /// or the mesh has no GPU buffers.
    void Draw(const RenderFrame &frame, const glm::mat4 &viewProjection, const MeshBuffer &mesh,
              const glm::mat4 &model, const glm::vec3 &color);

    /// @brief Outline a batch of meshes as one merged border of @p color. Their
    /// silhouettes union into a single mask, so overlapping meshes share one
    /// outline. No-op if the batch is empty.
    void DrawOutlines(const RenderFrame &frame, const glm::mat4 &viewProjection, std::span<const OutlineItem> items,
                      const glm::vec3 &color);

    /// @brief Outline a meshless entity's icon — the selection highlight for an
    /// entity drawn as a billboard. The quad is centred at @p center, spanning
    /// ±@p halfSize along the (unit) @p cameraRight / @p cameraUp axes, matching the
    /// drawn icon; @p iconTexture is sampled so the outline traces the icon's
    /// artwork. Painted in @p color, always on top. No-op if not initialised or
    /// @p iconTexture is null.
    void DrawBillboard(const RenderFrame &frame, const glm::mat4 &viewProjection, const glm::vec3 &center,
                       const glm::vec3 &cameraRight, const glm::vec3 &cameraUp, float halfSize,
                       nvrhi::ITexture *iconTexture, const glm::vec3 &color);

private:
    [[nodiscard]] bool BuildEdgePipeline(const nvrhi::FramebufferInfo &sceneFramebufferInfo);
    /// @brief (Re)create the coverage mask target + its (depth-less) framebuffer and
    /// the edge pass's binding set for a new viewport size. No-op when already that
    /// size.
    [[nodiscard]] bool EnsureMask(uint32_t width, uint32_t height);
    void RecordSilhouette(const RenderFrame &frame, const glm::mat4 &modelViewProjection, const MeshBuffer &mesh);
    void RecordBillboardMaskPass(const RenderFrame &frame, const glm::mat4 &viewProjection, const glm::vec3 &center,
                                 const glm::vec3 &cameraRight, const glm::vec3 &cameraUp, float halfSize);
    /// @brief (Re)build the billboard mask's binding set for @p iconTexture, reusing
    /// it while the texture is unchanged. Returns false if there is no texture.
    [[nodiscard]] bool EnsureBillboardBindingSet(nvrhi::ITexture *iconTexture);
    void RecordEdgePass(const RenderFrame &frame, const glm::vec3 &color);

    nvrhi::IDevice *_device = nullptr;

    // Mask pass: renders the silhouette into the R8 coverage target.
    nvrhi::ShaderHandle _maskVertexShader;
    nvrhi::ShaderHandle _maskPixelShader;
    nvrhi::InputLayoutHandle _maskInputLayout;
    nvrhi::BindingLayoutHandle _maskBindingLayout;
    nvrhi::BindingSetHandle _maskBindingSet;       // push constants only
    nvrhi::GraphicsPipelineHandle _maskPipeline;   // depth off (always on top)

    // Billboard mask variant: stamps a meshless entity's icon into the same
    // coverage mask (sampling the icon so only its opaque artwork counts), so the
    // edge pass outlines the icon's shape. Shares the coverage target and edge
    // pass; has its own quad vertex stage, icon-sampling pixel stage, sampler, and
    // a binding set rebuilt only when the icon texture changes.
    nvrhi::ShaderHandle _billboardMaskVertexShader;
    nvrhi::ShaderHandle _billboardMaskPixelShader;
    nvrhi::BindingLayoutHandle _billboardMaskBindingLayout;
    nvrhi::SamplerHandle _billboardSampler;
    nvrhi::BindingSetHandle _billboardMaskBindingSet;       // push constants + icon texture + sampler
    nvrhi::ITexture *_billboardTexture = nullptr;              // non-owning; identifies the current binding set
    nvrhi::GraphicsPipelineHandle _billboardMaskPipeline;

    // Edge pass: fullscreen edge detect that composites the coloured border.
    nvrhi::ShaderHandle _edgeVertexShader;
    nvrhi::ShaderHandle _edgePixelShader;
    nvrhi::BindingLayoutHandle _edgeBindingLayout;
    nvrhi::SamplerHandle _sampler;
    nvrhi::GraphicsPipelineHandle _edgePipeline;
    nvrhi::BindingSetHandle _edgeBindingSet;       // depends on the mask texture

    // The offscreen coverage mask, sized to the viewport and recreated on resize.
    nvrhi::TextureHandle _maskTexture;
    nvrhi::FramebufferHandle _maskFramebuffer;
    uint32_t _maskWidth  = 0;
    uint32_t _maskHeight = 0;
};

} // namespace Assisi::Render
