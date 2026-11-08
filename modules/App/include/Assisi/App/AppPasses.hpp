/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AppPasses.hpp
/// @brief The frame's passes around the scene, declared with APASS: the clear,
/// the scene, the post steps, the overlays, the game UI, the capture and the
/// debug UI.
///
/// reflectgen reads this header and writes AppPassGraph, which records the
/// passes in the order their Reads<> and Writes<> fix (see
/// Assisi/Render/FramePass.hpp); Application owns the graph and the objects
/// AppImports hands it. The images the post steps pass along are imports:
/// once a frame, Application points each at the surface Render::RouteChain
/// names for the current anti-aliasing mode and overlays, so a step draws
/// wherever its image is without asking where that is. A new post effect is a
/// struct here that reads and writes those images.

#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Math/Color.hpp>
#include <Assisi/Render/BloomPass.hpp>
#include <Assisi/Render/ExposurePass.hpp>
#include <Assisi/Render/FramePass.hpp>
#include <Assisi/Render/PostProcess.hpp>
#include <Assisi/Render/PostProcessSettings.hpp>
#include <Assisi/Render/Tonemap.hpp>

#include <nvrhi/nvrhi.h>

namespace Assisi::Mondrian
{
class DrawList;
}

namespace Assisi::Mondrian::Engine
{
class QuadPass;
}

namespace Assisi::App::Passes
{

using Mondrian::Engine::QuadPass;
using Render::PostProcess;
using Render::Reads;
using Render::Recorder;
using Render::Writes;

/// @brief Where the scene draws: multisampled under MSAA.
struct SceneTarget
{
    Render::RenderTarget target;
};

/// @brief The resolved scene the post steps read; without MSAA, the scene
/// target itself.
struct SceneHdr
{
    Render::RenderTarget target;
};

/// @brief What the tone map writes and the overlays draw on.
struct ToneMapped
{
    Render::RenderTarget target;
};

/// @brief The single-sampled picture FXAA or the final copy reads.
struct Display
{
    Render::RenderTarget target;
};

/// @brief The swapchain image this frame presents.
struct Screen
{
    Render::RenderTarget target;
};

/// @brief How the scene asks to be shown, as its volumes set it this frame.
/// The scene's draw hands it on, so the steps after it read this frame's.
struct SceneLook
{
    /// Exposure, grade, white balance and glow, sanitized.
    Render::PostProcessSettings settings;
    /// The scene target holds display values rather than radiance: a material
    /// debug view, which mapping would rescale.
    bool passthrough = false;
};

/// @brief The automatic exposure the metering keeps, for the tone map.
struct Exposure
{
    nvrhi::IBuffer *state = nullptr;
};

/// @brief The bloom's glow and how much of it the tone map mixes in.
struct Bloom
{
    nvrhi::ITexture *glow = nullptr;
    float intensity = 0.0f;
};

/// @brief What the app's own code draws or reads back, called from the passes
/// that host it.
class FrameHooks
{
public:
    /// @brief The scene, into its target, and how it asks to be shown.
    virtual void DrawScene(Render::RenderFrame &frame, SceneLook &look) = 0;
    /// @brief Display-referred chrome, onto the tone-mapped image.
    virtual void DrawOverlays(Render::RenderFrame &frame) = 0;
    /// @brief The debug UI, last, onto the screen.
    virtual void DrawUi(Render::RenderFrame &frame) = 0;
    /// @brief Reads back the finished frame, before the debug UI covers it.
    virtual void CaptureFrame(nvrhi::ICommandList *commandList, nvrhi::ITexture *screen) = 0;

protected:
    FrameHooks() = default;
    ~FrameHooks() = default;
    FrameHooks(const FrameHooks &) = default;
    FrameHooks &operator=(const FrameHooks &) = default;
};

/// @brief What every pass of the frame sees.
struct AppFrame
{
    /// The player's curve, brightness and dither.
    const Render::TonemapSettings &tonemap;
    /// The game UI to draw, or null while it is hidden.
    const Mondrian::DrawList *gameUi = nullptr;
    /// What the scene target clears to, decoded to radiance.
    Math::Color4<Math::ColorSpace::Linear> clearColor;
    /// How long since the last frame, which the automatic exposure adapts over.
    float frameSeconds = 0.0f;
    /// Which surface each image is.
    Render::ChainRoute route;
    /// Bright light glows: the player's choice.
    bool bloomEnabled = false;
    /// A picture of the finished frame is asked for.
    bool captureFrame = false;
};

/// @brief What the passes are built from.
struct AppPassInit
{
    nvrhi::IDevice *device = nullptr;
    /// The vertex stage every full-screen post draw shares.
    nvrhi::IShader *fullscreenVertexShader = nullptr;
};

/// @brief The objects Application owns and shares with the passes.
APASSIMPORTS()
struct AppImports
{
    FrameHooks &frameHooks;
    PostProcess &postProcess;
    QuadPass &quadPass;
    SceneTarget &sceneTarget;
    SceneHdr &sceneHdr;
    ToneMapped &toneMapped;
    Display &display;
    Screen &screen;
};

/// @brief Clears the scene target to the clear colour and the far depth.
APASS(name = "clear-targets", base)
struct ClearPass
{
    void Record(Recorder &rec, const AppFrame &frame, Writes<SceneTarget> out);
};

/// @brief The app's scene, drawn by its OnRender. The scene's own passes run
/// inside it and time themselves.
APASS(name = "scene", untimed, after = "clear-targets")
struct ScenePass
{
    void Record(Recorder &rec, const AppFrame &frame, Writes<SceneTarget, SceneLook, FrameHooks> out);
};

/// @brief Averages the scene's samples, as radiance, into the single-sampled
/// image the post steps read.
APASS(name = "pp-resolve")
struct ResolvePass
{
    bool Wanted(const AppFrame &frame) const { return frame.route.ResolvesScene(); }
    void Record(Recorder &rec, const AppFrame &frame, Reads<SceneTarget> in, Writes<SceneHdr> out);
};

/// @brief Meters the scene's brightness and moves the automatic exposure
/// toward it, while some of the scene's exposure is automatic.
APASS(name = "pp-exposure")
struct ExposureMeterPass
{
    bool Initialize(const AppPassInit &init);
    bool Record(Recorder &rec, const AppFrame &frame, Reads<SceneHdr, SceneLook> in, Writes<Exposure> out);

    Render::ExposurePass meter;
    /// The scene image the metering reads, rebound when it is reallocated.
    nvrhi::ITexture *meteredImage = nullptr;
};

/// @brief Spreads a soft glow around the brightest light, for the tone map to
/// mix in. Its copies of the scene exist while the player has bloom on.
APASS(name = "pp-bloom")
struct GlowPass
{
    bool Initialize(const AppPassInit &init);
    bool Wanted(const AppFrame &frame) const { return frame.bloomEnabled; }
    void Release();
    void Record(Recorder &rec, const AppFrame &frame, Reads<SceneHdr, SceneLook> in, Writes<Bloom> out);

    Render::BloomPass bloom;
    /// The scene image the copies were made from, remade when it is reallocated.
    nvrhi::ITexture *source = nullptr;
};

/// @brief Exposes, grades and maps the radiance into display values: the one
/// step that leaves HDR.
APASS(name = "pp-tonemap", base)
struct TonemapPass
{
    void Record(Recorder &rec, const AppFrame &frame, Reads<SceneHdr, SceneLook, Exposure, Bloom> in,
                Writes<ToneMapped, PostProcess> out);
};

/// @brief The app's display-referred chrome, drawn by its OnRenderOverlays onto
/// the tone-mapped image, depth-testing against the scene.
APASS(name = "overlays", untimed, after = "pp-tonemap")
struct OverlaysPass
{
    bool Wanted(const AppFrame &frame) const { return frame.route.overlays; }
    void Record(Recorder &rec, const AppFrame &frame, Writes<ToneMapped, FrameHooks> out);
};

/// @brief Averages the overlays' samples into the single-sampled picture.
APASS(name = "pp-overlay-resolve")
struct OverlayResolvePass
{
    bool Wanted(const AppFrame &frame) const { return frame.route.ResolvesOverlays(); }
    void Record(Recorder &rec, const AppFrame &frame, Reads<ToneMapped> in, Writes<Display> out);
};

/// @brief Smooths the picture's edges onto the screen.
APASS(name = "pp-fxaa")
struct FxaaPass
{
    bool Wanted(const AppFrame &frame) const { return frame.route.fxaa; }
    void Record(Recorder &rec, const AppFrame &frame, Reads<Display> in, Writes<Screen, PostProcess> out);
};

/// @brief Copies the picture onto the screen when it was finished somewhere
/// else and nothing else moves it there.
APASS(name = "pp-blit", after = "pp-fxaa")
struct CopyPass
{
    bool Wanted(const AppFrame &frame) const { return frame.route.CopiesToSwapchain(); }
    void Record(Recorder &rec, const AppFrame &frame, Reads<Display> in, Writes<Screen, PostProcess> out);
};

/// @brief The game's screens: menus and HUDs, after the tone map so their
/// colours are exactly as set.
APASS(name = "game-ui", after = "pp-blit")
struct GameUiPass
{
    bool Wanted(const AppFrame &frame) const { return frame.gameUi != nullptr; }
    void Record(Recorder &rec, const AppFrame &frame, Writes<Screen, QuadPass> out);
};

/// @brief Reads back the finished frame for a picture asked of it, before the
/// debug UI draws over it.
APASS(name = "frame-capture", untimed, after = "game-ui", before = "ui")
struct FrameCapturePass
{
    bool Wanted(const AppFrame &frame) const { return frame.captureFrame; }
    void Record(Recorder &rec, const AppFrame &frame, Writes<Screen, FrameHooks> out);
};

/// @brief The app's debug UI, drawn by its OnRenderUi, last in the frame.
APASS(name = "ui", base)
struct UiPass
{
    void Record(Recorder &rec, const AppFrame &frame, Writes<Screen, FrameHooks> out);
};

} // namespace Assisi::App::Passes
