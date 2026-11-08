/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file AppPasses.cpp
/// @brief The bodies of the passes AppPasses.hpp declares.

#include <Assisi/App/AppPasses.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Mondrian/Engine/QuadPass.hpp>
#include <Assisi/Render/SceneDepth.hpp>

namespace Assisi::App::Passes
{
namespace
{
constexpr const char *kBloomDownShader = "shaders/bloom_down.frag.spv";
constexpr const char *kBloomUpShader = "shaders/bloom_up.frag.spv";
constexpr const char *kExposureHistogramShader = "shaders/exposure_histogram.comp.spv";
constexpr const char *kExposureAdaptShader = "shaders/exposure_adapt.comp.spv";
} // namespace

// --- clear-targets ----------------------------------------------------------

void ClearPass::Record(Recorder &rec, const AppFrame &frame, Writes<SceneTarget> out)
{
    const Render::RenderTarget &target = out.Get<SceneTarget>().target;
    nvrhi::ICommandList *commandList = rec.CommandList();
    // The scene target holds radiance, so the clear colour arrives decoded, as
    // any sRGB texture is; the tone map puts it back where it was.
    commandList->clearTextureFloat(
        target.colorTexture, nvrhi::AllSubresources,
        nvrhi::Color(frame.clearColor.r, frame.clearColor.g, frame.clearColor.b, frame.clearColor.a));
    if (target.depthTexture != nullptr)
    {
        commandList->clearDepthStencilTexture(target.depthTexture, nvrhi::AllSubresources, true,
                                              Render::kSceneDepthFar, false, 0);
    }
}

// --- scene ------------------------------------------------------------------

void ScenePass::Record(Recorder &rec, const AppFrame & /*frame*/, Writes<SceneTarget, SceneLook, FrameHooks> out)
{
    Render::RenderFrame target = rec.Frame(out.Get<SceneTarget>());
    out.Get<FrameHooks>().DrawScene(target, out.Get<SceneLook>());
}

// --- pp-resolve -------------------------------------------------------------

void ResolvePass::Record(Recorder &rec, const AppFrame & /*frame*/, Reads<SceneTarget> in, Writes<SceneHdr> out)
{
    rec.CommandList()->resolveTexture(out.Get<SceneHdr>().target.colorTexture, nvrhi::AllSubresources,
                                      in.Get<SceneTarget>().target.colorTexture, nvrhi::AllSubresources);
}

// --- pp-exposure ------------------------------------------------------------

bool ExposureMeterPass::Initialize(const AppPassInit &init)
{
    if (!meter.Initialize(init.device, kExposureHistogramShader, kExposureAdaptShader))
    {
        Core::Log::Warn("Application: automatic exposure unavailable (its pipelines failed to build).");
        return false;
    }
    return true;
}

bool ExposureMeterPass::Record(Recorder &rec, const AppFrame &frame, Reads<SceneHdr, SceneLook> in,
                               Writes<Exposure> out)
{
    const SceneLook &look = in.Get<SceneLook>();
    if (look.passthrough || look.settings.exposure.automatic <= 0.0f)
    {
        return false;
    }
    const Render::RenderTarget &scene = in.Get<SceneHdr>().target;
    if (scene.colorTexture != meteredImage)
    {
        meter.Configure(scene.colorTexture, scene.width, scene.height);
        meteredImage = scene.colorTexture;
    }
    meter.Run(rec.CommandList(), look.settings.exposure, frame.frameSeconds);
    out.Get<Exposure>() = Exposure{.state = meter.State()};
    return true;
}

// --- pp-bloom ---------------------------------------------------------------

bool GlowPass::Initialize(const AppPassInit &init)
{
    if (!bloom.Initialize(init.device, init.fullscreenVertexShader, kBloomDownShader, kBloomUpShader))
    {
        Core::Log::Warn("Application: bloom unavailable (its pipelines failed to build).");
        return false;
    }
    return true;
}

void GlowPass::Release()
{
    if (source != nullptr)
    {
        bloom.Configure(0u, 0u, nullptr);
        source = nullptr;
    }
}

void GlowPass::Record(Recorder &rec, const AppFrame & /*frame*/, Reads<SceneHdr, SceneLook> in, Writes<Bloom> out)
{
    const Render::RenderTarget &scene = in.Get<SceneHdr>().target;
    if (scene.colorTexture != source)
    {
        bloom.Configure(scene.width, scene.height, scene.colorTexture);
        source = scene.colorTexture;
    }
    // The copies stay while the player has bloom on, so a volume that fades the
    // glow out and back in allocates nothing on the way.
    const SceneLook &look = in.Get<SceneLook>();
    if (!bloom.Ready() || look.passthrough || look.settings.bloomIntensity <= 0.0f)
    {
        out.Get<Bloom>() = Bloom{};
        return;
    }
    bloom.Run(rec.CommandList(), look.settings.bloomScatter);
    out.Get<Bloom>() = Bloom{.glow = bloom.Output(), .intensity = look.settings.bloomIntensity};
}

// --- pp-tonemap -------------------------------------------------------------

void TonemapPass::Record(Recorder &rec, const AppFrame &frame, Reads<SceneHdr, SceneLook, Exposure, Bloom> in,
                         Writes<ToneMapped, PostProcess> out)
{
    const SceneLook &look = in.Get<SceneLook>();
    const Bloom &bloom = in.Get<Bloom>();
    const Render::TonemapConstants constants =
        Render::MakeTonemapConstants(frame.tonemap, look.settings, bloom.intensity, look.passthrough);
    out.Get<PostProcess>().Tonemap(rec.Frame(out.Get<ToneMapped>()), in.Get<SceneHdr>().target.colorTexture,
                                   bloom.glow, in.Get<Exposure>().state, constants);
}

// --- overlays ---------------------------------------------------------------

void OverlaysPass::Record(Recorder &rec, const AppFrame & /*frame*/, Writes<ToneMapped, FrameHooks> out)
{
    Render::RenderFrame target = rec.Frame(out.Get<ToneMapped>());
    out.Get<FrameHooks>().DrawOverlays(target);
}

// --- pp-overlay-resolve -----------------------------------------------------

void OverlayResolvePass::Record(Recorder &rec, const AppFrame & /*frame*/, Reads<ToneMapped> in, Writes<Display> out)
{
    rec.CommandList()->resolveTexture(out.Get<Display>().target.colorTexture, nvrhi::AllSubresources,
                                      in.Get<ToneMapped>().target.colorTexture, nvrhi::AllSubresources);
}

// --- pp-fxaa ----------------------------------------------------------------

void FxaaPass::Record(Recorder &rec, const AppFrame & /*frame*/, Reads<Display> in, Writes<Screen, PostProcess> out)
{
    out.Get<PostProcess>().Fxaa(rec.Frame(out.Get<Screen>()), in.Get<Display>().target.colorTexture);
}

// --- pp-blit ----------------------------------------------------------------

void CopyPass::Record(Recorder &rec, const AppFrame & /*frame*/, Reads<Display> in, Writes<Screen, PostProcess> out)
{
    out.Get<PostProcess>().Copy(rec.Frame(out.Get<Screen>()), in.Get<Display>().target.colorTexture);
}

// --- game-ui ----------------------------------------------------------------

void GameUiPass::Record(Recorder &rec, const AppFrame &frame, Writes<Screen, QuadPass> out)
{
    out.Get<QuadPass>().Draw(rec.CommandList(), out.Get<Screen>().target.framebuffer, *frame.gameUi);
}

// --- frame-capture ----------------------------------------------------------

void FrameCapturePass::Record(Recorder &rec, const AppFrame & /*frame*/, Writes<Screen, FrameHooks> out)
{
    out.Get<FrameHooks>().CaptureFrame(rec.CommandList(), out.Get<Screen>().target.colorTexture);
}

// --- ui ---------------------------------------------------------------------

void UiPass::Record(Recorder &rec, const AppFrame & /*frame*/, Writes<Screen, FrameHooks> out)
{
    Render::RenderFrame target = rec.Frame(out.Get<Screen>());
    out.Get<FrameHooks>().DrawUi(target);
}

} // namespace Assisi::App::Passes
