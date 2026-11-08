/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PostProcess.hpp
/// @brief The offscreen scene target, the images the post steps pass along, and
/// the full-screen draws that bring the scene to the swapchain.
///
/// The scene never draws directly into the swapchain. It draws into an HDR
/// offscreen target owned by this class (SceneFramebuffer()), and the post steps
/// bring it to the swapchain framebuffer. Each step is a pass of its own; this
/// class owns what they share: the targets, which of them each named image is
/// for the current configuration (RouteChain), and the tone map, FXAA and copy
/// pipelines.
///
/// The steps have a seam among them: display-referred content (editor chrome —
/// the selection outline, entity icons, collider wireframes) draws *after* the
/// tone map, because those colours are already what they should look like on
/// screen and mapping them would restate them. An app that has none says so,
/// and the seam and its extra targets do not exist.

#include <cstdint>
#include <string>

#include <Assisi/Render/FramePass.hpp>
#include <Assisi/Render/Tonemap.hpp>

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

/// @brief What the chain is asked to accommodate, beyond the AA mode.
struct ChainOptions
{
    /// The app draws display-referred content of its own after the tone map.
    /// Costs a target and a copy, so an app without chrome does not ask for it.
    bool overlays = false;
};

/// @brief Which surface each image the post steps name is, for one AA mode and
/// one choice of overlays. The steps read and write the names; only this knows
/// where they land, so a step never checks the configuration to find its target.
struct ChainRoute
{
    /// Where the scene draws.
    ChainSurface sceneTarget = ChainSurface::SceneHdr;
    /// The resolved scene the tone map reads: always SceneHdr, which the scene
    /// draws into itself unless it is multisampled.
    ChainSurface sceneHdr = ChainSurface::SceneHdr;
    /// Where the tone map writes and the overlays draw.
    ChainSurface toneMapped = ChainSurface::Swapchain;
    /// The single-sampled picture FXAA or the final copy reads.
    ChainSurface display = ChainSurface::Swapchain;
    bool overlays = false;
    bool fxaa = false;

    /// @brief The scene draws multisampled and resolves into sceneHdr.
    [[nodiscard]] constexpr bool ResolvesScene() const { return sceneTarget != sceneHdr; }
    /// @brief The overlays draw multisampled and resolve into display.
    [[nodiscard]] constexpr bool ResolvesOverlays() const { return toneMapped != display; }
    /// @brief The picture ends somewhere other than the swapchain with no FXAA
    /// to move it there, so a copy does.
    [[nodiscard]] constexpr bool CopiesToSwapchain() const { return !fxaa && display != ChainSurface::Swapchain; }
};

/// @brief Routes the chain for the AA mode and what the app needs — the whole
/// shape of the frame in one place, so Configure() allocates exactly what the
/// steps touch.
[[nodiscard]] constexpr ChainRoute RouteChain(AaMode mode, ChainOptions options = {})
{
    const bool multisampled = (mode == AaMode::MSAA || mode == AaMode::MSAA_FXAA);

    ChainRoute route;
    route.overlays = options.overlays;
    route.fxaa = (mode == AaMode::FXAA || mode == AaMode::MSAA_FXAA);
    route.sceneTarget = multisampled ? ChainSurface::SceneMultisample : ChainSurface::SceneHdr;

    // Overlays under MSAA draw at the scene's sample count, which is the only way
    // they can share the depth buffer the scene wrote — and it is also what keeps
    // wireframe edges as smooth as they were before the tone map moved out of the
    // mesh shader. With nothing after it, the tone map writes the swapchain
    // itself, so the picture is never copied there.
    if (options.overlays)
    {
        route.toneMapped = multisampled ? ChainSurface::OverlayMultisample : ChainSurface::Ldr;
    }
    else if (route.fxaa)
    {
        route.toneMapped = ChainSurface::Ldr;
    }
    route.display = route.toneMapped == ChainSurface::OverlayMultisample ? ChainSurface::Ldr : route.toneMapped;
    return route;
}

class PostProcess
{
public:
    PostProcess() = default;

    struct InitParams
    {
        nvrhi::IDevice *device = nullptr;
        /// The format/sample count the last step draws into.
        nvrhi::FramebufferInfo swapchainFramebufferInfo;
        /// Shared fullscreen-triangle vertex stage for every post pass.
        std::string vertexShaderSpvPath;
        std::string tonemapShaderSpvPath;
        std::string fxaaShaderSpvPath;
    };

    /// @brief Loads the tone map and FXAA shaders and builds what does not
    /// depend on the AA mode. Call once, before the first Configure().
    [[nodiscard]] bool Initialize(const InitParams &params);

    /// @brief (Re)allocates offscreen render targets for the given size, mode, and
    /// MSAA sample count. Cheap to call every time any of these might have changed
    /// (resize, or an options-menu edit) — no-ops internally skip unaffected targets.
    void Configure(uint32_t width, uint32_t height, AaMode mode, uint32_t msaaSamples, ChainOptions options);

    /// @brief Which surface each named image is, as the last Configure() set it.
    [[nodiscard]] const ChainRoute &Route() const { return _route; }

    /// @brief The target @p surface is, with the scene's depth where the scene or
    /// the overlays depth-test. The swapchain is the one surface this class does
    /// not own, so its target is @p swapchain.
    [[nodiscard]] RenderTarget Target(ChainSurface surface, const RenderTarget &swapchain) const;

    /// @brief The vertex stage every full-screen post draw shares.
    [[nodiscard]] nvrhi::IShader *FullscreenVertexShader() const { return _fullscreenVertexShader; }

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

    /// @brief Maps the radiance in @p scene into @p frame's target: the glow
    /// mixed in, exposed, graded, encoded for display, as @p constants say.
    /// Without a glow the scene fills its slot, and without metered exposure a
    /// stand-in does; the shader reads neither unless @p constants ask it to.
    void Tonemap(const RenderFrame &frame, nvrhi::ITexture *scene, nvrhi::ITexture *glow, nvrhi::IBuffer *exposure,
                 const TonemapConstants &constants);

    /// @brief Smooths the edges of @p display into @p frame's target.
    void Fxaa(const RenderFrame &frame, nvrhi::ITexture *display);

    /// @brief Copies @p display, already mapped, into @p frame's target: the tone
    /// map shader told to pass its input through.
    void Copy(const RenderFrame &frame, nvrhi::ITexture *display);

    [[nodiscard]] AaMode Mode() const { return _mode; }

    /// @brief Releases all GPU resources this owns. Call during app shutdown,
    /// before the NVRHI device is destroyed — the member's own destructor would
    /// otherwise run too late (after the device teardown in ~Application).
    void Shutdown();

private:
    /// @brief A full-screen draw's pipeline and the binding set it last drew
    /// with, rebuilt when what it reads changes.
    struct FullscreenDraw
    {
        nvrhi::GraphicsPipelineHandle pipeline;
        nvrhi::BindingSetHandle bindings;
        nvrhi::ITexture *source = nullptr;
        nvrhi::ITexture *glow = nullptr;
        nvrhi::IBuffer *exposure = nullptr;
    };

    void Rebuild();
    /// @brief The pipeline for @p shader drawing into @p destination, logged
    /// when it cannot be built.
    [[nodiscard]] nvrhi::GraphicsPipelineHandle MakePipeline(nvrhi::IShader *shader, nvrhi::IBindingLayout *layout,
                                                             ChainSurface destination) const;
    /// @brief Binds the tone map shader's inputs, for the tone map and the copy.
    void BindTonemapInputs(FullscreenDraw &draw, nvrhi::ITexture *source, nvrhi::ITexture *glow,
                           nvrhi::IBuffer *exposure) const;
    /// @brief Records @p draw into @p frame's target with @p constants pushed.
    static void Draw(const RenderFrame &frame, const FullscreenDraw &draw, const void *constants,
                     uint32_t constantsSize);
    [[nodiscard]] nvrhi::IFramebuffer *FramebufferFor(ChainSurface surface) const;
    [[nodiscard]] nvrhi::ITexture *TextureFor(ChainSurface surface) const;
    [[nodiscard]] nvrhi::FramebufferInfo InfoFor(ChainSurface surface) const;
    /// @brief Whether @p surface carries the scene's depth: where the scene
    /// draws, and where the overlays depth-test against it.
    [[nodiscard]] bool CarriesSceneDepth(ChainSurface surface) const;

    nvrhi::IDevice *_device = nullptr;
    nvrhi::FramebufferInfo _swapchainInfo;

    AaMode _mode = AaMode::None;
    ChainOptions _options;
    ChainRoute _route = RouteChain(AaMode::None);
    uint32_t _msaaSamples = 4;
    uint32_t _width = 0;
    uint32_t _height = 0;

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

    // Full-screen draws. Every one samples through the same sampler; they differ
    // in shader, source and destination, so each has its own pipeline, built
    // against the surface it draws into.
    nvrhi::ShaderHandle _fullscreenVertexShader;
    nvrhi::ShaderHandle _tonemapShader;
    nvrhi::ShaderHandle _fxaaShader;
    nvrhi::SamplerHandle _sampler;
    nvrhi::BindingLayoutHandle _tonemapBindingLayout;
    nvrhi::BindingLayoutHandle _fxaaBindingLayout;
    /// Fills the tone map's exposure slot while nothing metered: the shader
    /// reads the slot only when some of the exposure is automatic, which is
    /// exactly when the metering runs.
    nvrhi::BufferHandle _noExposure;

    FullscreenDraw _tonemap;
    FullscreenDraw _fxaa;
    FullscreenDraw _copy;
};

} // namespace Assisi::Render
