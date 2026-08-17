/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PostProcess.hpp
/// @brief The offscreen scene target and the post-process chain that lands it
/// on the swapchain: MSAA resolve, tone map, overlays, FXAA.
///
/// The scene never draws directly into the swapchain. It draws into an HDR
/// offscreen target owned by this class (SceneFramebuffer()), and the chain
/// brings it to the swapchain framebuffer. `Application::RenderFrame` wires this
/// up by swapping the `RenderFrame` it hands to `OnRender()`; derived apps never
/// see the offscreen target directly.
///
/// The chain has a seam in it: display-referred content (editor chrome — the
/// selection outline, entity icons, collider wireframes) draws *after* the tone
/// map, because those colours are already what they should look like on screen
/// and mapping them would restate them. An app that has none says so, and the
/// seam and its extra targets do not exist.

#include <array>
#include <cstdint>
#include <string>

#include <nvrhi/nvrhi.h>

namespace Assisi::Render
{
struct RenderFrame;

/// @brief Anti-aliasing technique selection.
enum class AaMode : std::uint8_t
{
    None,     ///< No anti-aliasing — the scene target is single-sampled.
    MSAA,     ///< Multisample AA (hardware, geometry edges only).
    FXAA,     ///< Fast approximate AA (post-process, covers specular highlights too).
    MSAA_FXAA ///< MSAA resolved into an FXAA pass for best quality.
};

/// @brief The format every scene target carries, whatever the AA mode.
///
/// Float, so lighting accumulates unbounded and the tone map decides what a
/// value over 1 looks like. An 8-bit target clamps and gamma-encodes before
/// anything downstream can act on the radiance — which is why the MSAA resolve
/// and every post effect used to run on perceptual values.
inline constexpr nvrhi::Format kSceneColorFormat = nvrhi::Format::RGBA16_FLOAT;

/// @brief A surface the chain reads or writes.
enum class ChainSurface : std::uint8_t
{
    SceneMultisample,   ///< HDR, `msaaSamples` samples — where the scene draws under MSAA.
    SceneHdr,           ///< HDR, single-sampled — where the scene draws otherwise, and the resolve target.
    OverlayMultisample, ///< Display-encoded, `msaaSamples` samples — overlays under MSAA, sharing the scene's depth.
    Ldr,                ///< Display-encoded, single-sampled.
    Swapchain           ///< The real backbuffer.
};

/// @brief True for the surfaces that hold linear radiance rather than display values.
[[nodiscard]] constexpr bool IsHdrSurface(ChainSurface surface)
{
    return surface == ChainSurface::SceneMultisample || surface == ChainSurface::SceneHdr;
}

/// @brief True for the surfaces that carry more than one sample per pixel.
[[nodiscard]] constexpr bool IsMultisampleSurface(ChainSurface surface)
{
    return surface == ChainSurface::SceneMultisample || surface == ChainSurface::OverlayMultisample;
}

enum class ChainStage : std::uint8_t
{
    Resolve,  ///< Multisample resolve. Both sides share a colour space, so samples average in it.
    Tonemap,  ///< The one radiance -> display-encoded step in the chain.
    Overlays, ///< The seam: the app draws display-referred chrome. Reads and writes the same surface.
    Blit,     ///< Straight copy, for when the chain's last real work did not land on the swapchain.
    Fxaa      ///< Edge-directed blur. Wants perceptual input, so it is always last.
};

struct ChainStep
{
    ChainStage stage{};
    ChainSurface source{};
    ChainSurface destination{};
};

/// @brief What the chain is asked to accommodate, beyond the AA mode.
struct ChainOptions
{
    /// The app draws display-referred content of its own after the tone map.
    /// Costs a target and a copy, so an app without chrome does not ask for it.
    bool overlays = false;
};

inline constexpr std::uint32_t kMaxChainSteps = 5;

/// @brief The chain for one configuration: which stages run, in order, over
/// which surfaces.
struct ChainPlan
{
    ChainSurface sceneTarget = ChainSurface::SceneHdr; ///< where the scene itself draws
    std::array<ChainStep, kMaxChainSteps> steps{};
    std::uint32_t stepCount = 0;

    [[nodiscard]] constexpr bool Has(ChainStage stage) const { return IndexOf(stage) < stepCount; }

    /// @brief Position of the first step with this stage, or stepCount if absent.
    [[nodiscard]] constexpr std::uint32_t IndexOf(ChainStage stage) const
    {
        for (std::uint32_t i = 0; i < stepCount; ++i)
        {
            if (steps[i].stage == stage)
            {
                return i;
            }
        }
        return stepCount;
    }

    /// @brief Where the app's overlays draw, or Swapchain-if-absent guarded by Has().
    [[nodiscard]] constexpr ChainSurface OverlaySurface() const
    {
        const std::uint32_t index = IndexOf(ChainStage::Overlays);
        return index < stepCount ? steps[index].destination : ChainSurface::Swapchain;
    }
};

/// @brief Derives the chain from the AA mode and what the app needs — the whole
/// shape of the frame in one place, so Rebuild() allocates exactly what the
/// execution walks.
[[nodiscard]] constexpr ChainPlan PlanChain(AaMode mode, ChainOptions options = {})
{
    const bool multisampled = (mode == AaMode::MSAA || mode == AaMode::MSAA_FXAA);
    const bool fxaa = (mode == AaMode::FXAA || mode == AaMode::MSAA_FXAA);

    ChainPlan plan;
    plan.sceneTarget = multisampled ? ChainSurface::SceneMultisample : ChainSurface::SceneHdr;

    if (multisampled)
    {
        plan.steps[plan.stepCount++] = {ChainStage::Resolve, ChainSurface::SceneMultisample, ChainSurface::SceneHdr};
    }

    // Overlays under MSAA draw at the scene's sample count, which is the only way
    // they can share the depth buffer the scene wrote — and it is also what keeps
    // wireframe edges as smooth as they were before the tone map moved out of the
    // mesh shader.
    ChainSurface tonemapTarget = ChainSurface::Swapchain;
    if (options.overlays)
    {
        tonemapTarget = multisampled ? ChainSurface::OverlayMultisample : ChainSurface::Ldr;
    }
    else if (fxaa)
    {
        tonemapTarget = ChainSurface::Ldr;
    }

    // The tone map runs in every configuration: it is the only thing that turns
    // radiance into something a display format can hold.
    plan.steps[plan.stepCount++] = {ChainStage::Tonemap, ChainSurface::SceneHdr, tonemapTarget};

    if (options.overlays)
    {
        plan.steps[plan.stepCount++] = {ChainStage::Overlays, tonemapTarget, tonemapTarget};
        if (multisampled)
        {
            plan.steps[plan.stepCount++] = {ChainStage::Resolve, ChainSurface::OverlayMultisample, ChainSurface::Ldr};
        }
    }

    const ChainSurface last = plan.steps[plan.stepCount - 1].destination;
    if (fxaa)
    {
        plan.steps[plan.stepCount++] = {ChainStage::Fxaa, last, ChainSurface::Swapchain};
    }
    else if (last != ChainSurface::Swapchain)
    {
        plan.steps[plan.stepCount++] = {ChainStage::Blit, last, ChainSurface::Swapchain};
    }
    return plan;
}

class PostProcess
{
public:
    PostProcess() = default;

    struct InitParams
    {
        nvrhi::IDevice *device = nullptr;
        /// The format/sample count the last stage of the chain draws into.
        nvrhi::FramebufferInfo swapchainFramebufferInfo;
        /// Shared fullscreen-triangle vertex stage for every post pass.
        std::string vertexShaderSpvPath;
        std::string tonemapShaderSpvPath;
        std::string fxaaShaderSpvPath;
    };

    /// @brief Loads the post-process shaders and builds what does not depend on
    /// the AA mode. Call once, before the first Configure().
    [[nodiscard]] bool Initialize(const InitParams &params);

    /// @brief (Re)allocates offscreen render targets for the given size, mode, and
    /// MSAA sample count. Cheap to call every time any of these might have changed
    /// (resize, or an options-menu edit) — no-ops internally skip unaffected targets.
    void Configure(uint32_t width, uint32_t height, AaMode mode, uint32_t msaaSamples, ChainOptions options);

    /// @brief The framebuffer the scene renders into this frame. Never the
    /// swapchain: the scene is HDR and the swapchain is not.
    [[nodiscard]] nvrhi::IFramebuffer *SceneFramebuffer() const;
    [[nodiscard]] nvrhi::ITexture *SceneColorTexture() const;
    [[nodiscard]] nvrhi::ITexture *SceneDepthTexture() const;

    /// @brief The FramebufferInfo of SceneFramebuffer() — scene pipelines must be
    /// built/rebuilt to match this. Only changes when the mode moves into or out
    /// of {MSAA, MSAA_FXAA}, since the sample count is all that varies.
    [[nodiscard]] nvrhi::FramebufferInfo SceneFramebufferInfo() const;

    /// @brief The framebuffer display-referred overlays draw into, or nullptr if
    /// this chain has no overlay seam. It carries the same depth buffer the scene
    /// wrote, so overlays still depth-test against the scene.
    [[nodiscard]] nvrhi::IFramebuffer *OverlayFramebuffer() const;
    /// @brief The FramebufferInfo of OverlayFramebuffer() — overlay pipelines must
    /// be built/rebuilt to match this. Differs from the scene's in format.
    [[nodiscard]] nvrhi::FramebufferInfo OverlayFramebufferInfo() const;

    /// @brief Runs the chain up to the overlay seam: the MSAA resolve, then the
    /// tone map. Call after the scene draws.
    void RunBeforeOverlays(nvrhi::ICommandList *commandList, const RenderFrame &frame) const;

    /// @brief Runs the rest of the chain, landing the result in `frame`'s
    /// swapchain framebuffer. Call after the overlays draw (or straight after
    /// RunBeforeOverlays when there are none), before ImGui.
    void RunAfterOverlays(nvrhi::ICommandList *commandList, const RenderFrame &frame) const;

    /// @brief Makes the tone map copy its input through instead of mapping it.
    ///
    /// For frames whose scene target already holds display values rather than
    /// radiance — the material debug views, which short-circuit lighting to show
    /// a channel raw. Mapping those would rescale the very numbers being read.
    void SetTonemapPassthrough(bool passthrough) { _tonemapPassthrough = passthrough; }

    [[nodiscard]] AaMode Mode() const { return _mode; }

    /// @brief Releases all GPU resources this owns. Call during app shutdown,
    /// before the NVRHI device is destroyed — the member's own destructor would
    /// otherwise run too late (after the device teardown in ~Application).
    void Shutdown();

private:
    void Rebuild();
    void RunSteps(nvrhi::ICommandList *commandList, const RenderFrame &frame, uint32_t first, uint32_t last) const;
    [[nodiscard]] nvrhi::IFramebuffer *FramebufferFor(ChainSurface surface, const RenderFrame *frame) const;
    [[nodiscard]] nvrhi::ITexture *TextureFor(ChainSurface surface, const RenderFrame *frame) const;
    [[nodiscard]] nvrhi::FramebufferInfo InfoFor(ChainSurface surface) const;

    nvrhi::IDevice *_device = nullptr;
    nvrhi::FramebufferInfo _swapchainInfo;

    AaMode _mode = AaMode::None;
    ChainOptions _options;
    ChainPlan _plan = PlanChain(AaMode::None);
    uint32_t _msaaSamples = 4;
    uint32_t _width = 0;
    uint32_t _height = 0;
    bool _tonemapPassthrough = false;

    // Multisample HDR scene target — used by MSAA and MSAA_FXAA. Its depth is
    // shared with the overlay target, so overlays depth-test against the scene.
    nvrhi::TextureHandle _msaaColor;
    nvrhi::TextureHandle _msaaDepth;
    nvrhi::FramebufferHandle _msaaFramebuffer;

    // Single-sample HDR scene target — where the scene draws without MSAA, and
    // the resolve destination with it. Always exists: it is the tone map's input.
    nvrhi::TextureHandle _sceneColor;
    nvrhi::TextureHandle _sceneDepth;
    nvrhi::FramebufferHandle _sceneFramebuffer;

    // Display-encoded targets. The multisample one only exists when overlays and
    // MSAA are both on; the single-sample one whenever anything at all follows
    // the tone map.
    nvrhi::TextureHandle _overlayMsaaColor;
    nvrhi::FramebufferHandle _overlayMsaaFramebuffer;
    nvrhi::TextureHandle _ldrColor;
    nvrhi::FramebufferHandle _ldrFramebuffer;

    // Fullscreen post passes. Every one samples a single texture through the same
    // sampler; they differ in shader, source and destination — so their pipelines
    // and binding sets are built per chain step.
    nvrhi::ShaderHandle _fullscreenVertexShader;
    nvrhi::ShaderHandle _tonemapShader;
    nvrhi::ShaderHandle _fxaaShader;
    nvrhi::SamplerHandle _sampler;
    nvrhi::BindingLayoutHandle _tonemapBindingLayout;
    nvrhi::BindingLayoutHandle _fxaaBindingLayout;

    struct StepResources
    {
        nvrhi::GraphicsPipelineHandle pipeline;
        nvrhi::BindingSetHandle bindingSet;
    };
    std::array<StepResources, kMaxChainSteps> _stepResources;
};

} // namespace Assisi::Render
