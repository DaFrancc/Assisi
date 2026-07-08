/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PostProcess.hpp
/// @brief Anti-aliasing post-process pipeline: MSAA, FXAA, or both.
///
/// The scene never draws directly into the swapchain when a mode other than
/// `None` is active — it draws into an offscreen target owned by this class
/// (SceneFramebuffer()), which Resolve() then MSAA-resolves and/or FXAA's
/// into the real swapchain framebuffer. `Application::RenderFrame` wires
/// this up by swapping the `VulkanFrame` it hands to `OnRender()`; derived
/// apps never see the offscreen target directly.

#include <cstdint>
#include <string>

#include <nvrhi/nvrhi.h>

namespace Assisi::Render::Vulkan
{
struct VulkanFrame;
}

namespace Assisi::Render
{

/// @brief Anti-aliasing technique selection.
enum class AaMode
{
    None,     ///< No anti-aliasing — scene renders directly into the swapchain.
    MSAA,     ///< Multisample AA (hardware, geometry edges only).
    FXAA,     ///< Fast approximate AA (post-process, covers specular highlights too).
    MSAA_FXAA ///< MSAA resolved into an FXAA pass for best quality.
};

class PostProcess
{
  public:
    PostProcess() = default;

    /// @brief Builds the FXAA fullscreen pipeline against `swapchainFramebufferInfo`
    /// (the format/sample-count FXAA always draws into, since its output always
    /// targets the real swapchain). Call once, before the first Configure().
    bool Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &swapchainFramebufferInfo,
                    const std::string &vertexShaderSpvPath, const std::string &fragmentShaderSpvPath);

    /// @brief (Re)allocates offscreen render targets for the given size, mode, and
    /// MSAA sample count. Cheap to call every time any of these might have changed
    /// (resize, or an options-menu edit) — no-ops internally skip unaffected targets.
    void Configure(uint32_t width, uint32_t height, AaMode mode, uint32_t msaaSamples);

    /// @brief The framebuffer the scene should render into this frame, or nullptr
    /// if it should render directly into the swapchain framebuffer (mode == None).
    [[nodiscard]] nvrhi::IFramebuffer *SceneFramebuffer() const;
    [[nodiscard]] nvrhi::ITexture     *SceneColorTexture() const;
    [[nodiscard]] nvrhi::ITexture     *SceneDepthTexture() const;

    /// @brief The FramebufferInfo of SceneFramebuffer() (or the swapchain's, when
    /// mode == None) — scene pipelines must be built/rebuilt to match this. Only
    /// changes when the mode moves into or out of {MSAA, MSAA_FXAA}, since those
    /// are the only modes with a sample count other than 1.
    [[nodiscard]] nvrhi::FramebufferInfo SceneFramebufferInfo() const;

    /// @brief Resolves and/or FXAA's the offscreen scene render into `frame`'s
    /// swapchain framebuffer. No-op if mode is None. Call after the scene draws,
    /// before ImGui.
    void Resolve(nvrhi::ICommandList *commandList, const Vulkan::VulkanFrame &frame) const;

    [[nodiscard]] AaMode Mode() const { return _mode; }

  private:
    void Rebuild();
    void RunFxaa(nvrhi::ICommandList *commandList, const Vulkan::VulkanFrame &frame) const;

    nvrhi::IDevice        *_device = nullptr;
    nvrhi::FramebufferInfo _swapchainInfo;

    AaMode   _mode = AaMode::None;
    uint32_t _msaaSamples = 4;
    uint32_t _width = 0;
    uint32_t _height = 0;

    // Multisample scene target — used by MSAA and MSAA_FXAA.
    nvrhi::TextureHandle     _msaaColor;
    nvrhi::TextureHandle     _msaaDepth;
    nvrhi::FramebufferHandle _msaaFramebuffer;

    // Single-sample offscreen scene target — used by FXAA (the scene renders
    // directly into it) and MSAA_FXAA (the MSAA resolve destination).
    nvrhi::TextureHandle     _sceneColor;
    nvrhi::TextureHandle     _sceneDepth;
    nvrhi::FramebufferHandle _sceneFramebuffer;

    // FXAA fullscreen pass.
    nvrhi::BindingLayoutHandle    _fxaaBindingLayout;
    nvrhi::SamplerHandle          _fxaaSampler;
    nvrhi::GraphicsPipelineHandle _fxaaPipeline;
    nvrhi::BindingSetHandle       _fxaaBindingSet; // rebuilt whenever _sceneColor is recreated
};

} // namespace Assisi::Render
