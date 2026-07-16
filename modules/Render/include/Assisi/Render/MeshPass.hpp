/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file MeshPass.hpp
/// @brief NVRHI graphics pipeline for drawing lit, opaque scene geometry.
///
/// One pipeline shared by every entity: `MeshBuffer`'s vertex layout in, a
/// per-draw model + model-view-projection matrix pushed as constants, a
/// per-frame constant buffer carrying the camera and cluster-grid parameters,
/// a full glTF metallic-roughness Material (base colour, normal, metallic-
/// roughness, occlusion, emissive — factors + textures) bound per-submesh, and
/// clustered point/spot/directional lighting (see ClusterGrid) read directly
/// from cube_min.frag.

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>

#include <nvrhi/nvrhi.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/ClusterGrid.hpp>
#include <Assisi/Render/DrawItem.hpp>
#include <Assisi/Render/Material.hpp>
#include <Assisi/Render/MeshBuffer.hpp>
#include <Assisi/Render/RenderFrame.hpp>

namespace Assisi::Render
{
/// @brief Which material channel the mesh pass visualizes instead of the lit
/// result. None = the normal render; the rest short-circuit the shader to one
/// channel for debugging. Values must match the `kDebug*` constants in
/// cube_min.frag (packed into FrameConstants).
enum class MaterialDebugView : uint32_t
{
    None = 0,
    BaseColor,
    Metallic,
    Roughness,
    Normal,
    Occlusion,
    Emissive,
};

class MeshPass
{
  public:
    MeshPass() = default;

    /// @brief Inputs to Initialize(). Grouped into a struct so the call site
    /// stays legible and adding an input doesn't ripple through every caller.
    struct InitParams
    {
        nvrhi::IDevice        *device = nullptr;
        nvrhi::FramebufferInfo framebufferInfo;
        /// Compiled-SPIR-V shader paths (see ShaderModule.hpp).
        std::string vertexShaderSpvPath;
        std::string pixelShaderSpvPath;
        /// Must outlive the pass — its light buffers bind into every binding set.
        const ClusterGrid *clusterGrid = nullptr;
        /// The AssetCache's bindless material-texture table + layout (stage D):
        /// the layout joins the pipeline as register space 1, the table binds
        /// every draw. Both must outlive the pass (AssetCache owns them; the
        /// handles stay stable across Clear()).
        nvrhi::IBindingLayout   *bindlessLayout = nullptr;
        nvrhi::IDescriptorTable *bindlessTable = nullptr;
    };

    /// @brief Loads the shaders and builds the pipeline against the framebuffer
    /// format in @p params.
    /// @return false if either shader failed to load or the pipeline failed to build.
    [[nodiscard]] bool Initialize(const InitParams &params);

    /// @brief Recreates just the graphics pipeline against a new FramebufferInfo,
    /// reusing the shaders/input layout/binding layout already loaded by
    /// Initialize(). Needed when the render target's sample count changes at
    /// runtime (see Render::PostProcess) — a pipeline built for sampleCount=1
    /// isn't compatible with an MSAA framebuffer. Existing binding sets (which
    /// don't depend on FramebufferInfo) stay valid and cached.
    /// @pre IsValid() — call Initialize() first.
    [[nodiscard]] bool RebuildPipeline(const nvrhi::FramebufferInfo &framebufferInfo);

    /// @brief Updates the per-frame constant buffer (camera view matrix and
    /// cluster-grid parameters). Call once per frame, before any Draw() calls.
    void UpdateFrameConstants(nvrhi::ICommandList *commandList, const glm::mat4 &view, uint32_t screenWidth,
                              uint32_t screenHeight, float nearZ, float farZ, uint32_t dirLightCount,
                              MaterialDebugView debugView = MaterialDebugView::None) const;

    /// @brief State-change counts from one Submit — the consumer half of the
    /// draw-stats (the producer counts drawn/culled). materialBinds is the
    /// distinct binding-set runs the sorted items reduced to; meshBinds is the
    /// distinct vertex-buffer runs, which the shared GeometryArena (stage C)
    /// collapses to ~1 since every mesh draws from the one arena buffer.
    struct SubmitStats
    {
        uint32_t drawCalls    = 0;
        uint32_t materialBinds = 0;
        uint32_t meshBinds     = 0;
    };

    /// @brief Records `items` into `frame` in order — one drawIndexed per DrawItem,
    /// binding each item's Material and mesh vertex/index buffers. The caller sorts
    /// the span by DrawItem::sortKey first (material/mesh-major), so consecutive
    /// items share state and NVRHI's state cache collapses the redundant binds; the
    /// MVP is derived per item as `viewProjection * item.model`. Items must
    /// reference live resources (valid until the frame is submitted).
    /// @param frame  The frame's command list + framebuffer + viewport size.
    /// @pre IsValid() — call Initialize() first, and UpdateFrameConstants() this frame.
    [[nodiscard]] SubmitStats Submit(const RenderFrame &frame, const glm::mat4 &viewProjection,
                                     std::span<const DrawItem> items) const;

    bool IsValid() const { return _pipeline != nullptr; }

    /// @brief Drops every cached material binding set. Call when the resources they
    /// reference may be freed or replaced (e.g. an AssetCache is cleared or a
    /// level is unloaded) so a later Draw() rebuilds against live resources
    /// instead of returning a binding set that pins a destroyed texture — or,
    /// worse, aliases a freed address the allocator has since reused.
    void InvalidateBindingSets() { _bindingSetCache.clear(); }

  private:
    /// @brief Returns the binding set for `material`, creating and caching it on
    /// first use, keyed by the material's stable id. NVRHI binding sets reference
    /// concrete resources (the material's constants buffer + five textures), so
    /// one is needed per distinct material — unlike the mesh, which is bound
    /// directly via vertex/index buffer bindings rather than the binding set.
    nvrhi::IBindingSet *GetOrCreateBindingSet(const Material &material) const;

    nvrhi::IDevice *_device = nullptr;
    const ClusterGrid *_clusterGrid = nullptr;

    nvrhi::ShaderHandle           _vertexShader;
    nvrhi::ShaderHandle           _pixelShader;
    nvrhi::InputLayoutHandle      _inputLayout;
    nvrhi::BindingLayoutHandle    _bindingLayout;
    nvrhi::SamplerHandle          _sampler;
    nvrhi::GraphicsPipelineHandle _pipeline;
    nvrhi::BufferHandle           _frameConstantsBuffer;

    // The AssetCache's bindless material-texture table + its layout (stage D).
    // Non-owning: the AssetCache owns them and keeps the handles stable across
    // Clear(). The layout is register space 1 in the pipeline; the table is bound
    // as the second binding set each draw.
    nvrhi::BindingLayoutHandle   _bindlessLayout;
    nvrhi::DescriptorTableHandle _bindlessTable;

    /// @brief Cache of binding sets keyed by the material's stable id (Material::Id).
    ///
    /// The id is monotonic and never reused (it survives AssetCache::Clear), so a
    /// stale entry is dead, never wrong: an id whose material was freed simply
    /// never comes up again. When the asset set backing the scene changes (a level
    /// unload or an AssetCache clear frees the materials' GPU resources), the owner
    /// still calls InvalidateBindingSets() for memory hygiene — otherwise dead
    /// entries would pin their constants buffers and textures alive forever. A
    /// future streaming path evicts per-id rather than clearing wholesale.
    mutable std::unordered_map<uint32_t, nvrhi::BindingSetHandle> _bindingSetCache;
};
} /* namespace Assisi::Render */
