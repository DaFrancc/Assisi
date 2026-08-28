/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file EditorOptions.cpp
/// @brief EditorOptionsPanel::Draw — the body of the F11 options overlay.
///
/// Everything the panel is and why it lives here is documented on the class, in
/// EditorOptionsPanel.hpp. This file is the layout, top to bottom: frame-time
/// readouts, GPU telemetry, the frame graph, percentile stats, the renderer A/B
/// toggles, then the persisted anti-aliasing and frame-sync settings.

#include "EditorOptionsPanel.hpp"

#include <Assisi/App/Application.hpp>
#include <Assisi/Runtime/SceneRenderer.hpp>

#include <Assisi/App/OptionsConfig.hpp>
#include <Assisi/Render/PostProcess.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/Key.hpp>

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <vector>

namespace Assisi::Editor
{

using Assisi::App::Application;
using Assisi::App::FrameSyncMode;
using Assisi::App::OptionsConfig;

// The sun's cascades. Everything here lands on the next frame: a resolution or
// cascade-count change reallocates the array in SceneRenderer::Render, and the
// rest ride into the shader as frame constants — so a knob can be compared
// against its neighbour without anything being rebuilt between the two.
void EditorOptionsPanel::DrawShadowSettings(const Frame &frame)
{
    Assisi::Render::ShadowSettings shadows = frame.renderer.ShadowSettings();
    bool changed = false;

    ImGui::TextUnformatted("Sun Shadows");

    changed |= ImGui::Checkbox("Cast Shadows", &shadows.sun.enabled);

    // The tier presets from the quality table. A tier is a preset over the knobs
    // below, never a lock on them: pressing one writes those knobs and they stay
    // editable, which is why the readout falls to Custom after any edit rather
    // than remembering which button was last pressed.
    static const char *kTierNames[] = {"Low", "Medium", "High", "Ultra"};
    const Assisi::Render::ShadowTier tier = Assisi::Render::Tier(shadows);
    for (int32_t i = 0; i < IM_ARRAYSIZE(kTierNames); ++i)
    {
        if (i > 0)
        {
            ImGui::SameLine();
        }
        const bool active = static_cast<int32_t>(tier) == i;
        ImGui::BeginDisabled(active);
        if (ImGui::SmallButton(kTierNames[i]))
        {
            // The tier's own knobs only — the biases and the blend band are
            // correctness settings no tier has an opinion about, so an edit to
            // one survives a tier change.
            const Assisi::Render::ShadowSettings preset =
                Assisi::Render::TierSettings(static_cast<Assisi::Render::ShadowTier>(i));
            shadows.sun.cascadeCount = preset.sun.cascadeCount;
            shadows.sun.resolution   = preset.sun.resolution;
            shadows.sun.format       = preset.sun.format;
            shadows.sun.maxDistance  = preset.sun.maxDistance;
            shadows.sun.filter       = preset.sun.filter;
            // The local half too, though nothing below edits it yet: a tier is
            // one point in the whole knob space, and writing half of it would
            // leave the readout reporting Custom the moment it was pressed.
            shadows.local.atlasResolution = preset.local.atlasResolution;
            shadows.local.format          = preset.local.format;
            shadows.local.faceResolution  = preset.local.faceResolution;
            shadows.local.filter          = preset.local.filter;
            changed = true;
        }
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    // The sun's own bytes, not the whole knob space's: this section is the sun,
    // and quoting an atlas nothing has allocated would be a figure with no
    // memory behind it.
    ImGui::TextDisabled("%s  (%.0f MiB)", tier == Assisi::Render::ShadowTier::Custom ? "Custom" : "",
                        static_cast<double>(Assisi::Render::SunShadowMemoryBytes(shadows.sun)) / (1024.0 * 1024.0));

    if (!shadows.sun.enabled)
    {
        ImGui::BeginDisabled();
    }

    int32_t cascadeCount = static_cast<int32_t>(shadows.sun.cascadeCount);
    if (ImGui::SliderInt("Cascades", &cascadeCount, static_cast<int32_t>(Assisi::Render::kMinShadowCascades),
                         static_cast<int32_t>(Assisi::Render::kMaxShadowCascades)))
    {
        shadows.sun.cascadeCount = static_cast<std::uint32_t>(cascadeCount);
        changed = true;
    }

    // Powers of two, because the texel lattice the cascade snaps to has to
    // divide the box evenly (see Render::Sanitized).
    static const char *kResolutionNames[] = {"512", "1024", "2048", "4096"};
    static const std::uint32_t kResolutions[] = {512u, 1024u, 2048u, 4096u};
    int32_t resolutionIndex = 2;
    for (int32_t i = 0; i < IM_ARRAYSIZE(kResolutions); ++i)
    {
        if (kResolutions[i] == shadows.sun.resolution)
        {
            resolutionIndex = i;
        }
    }
    if (ImGui::Combo("Resolution", &resolutionIndex, kResolutionNames, IM_ARRAYSIZE(kResolutionNames)))
    {
        shadows.sun.resolution = kResolutions[resolutionIndex];
        changed = true;
    }

    // **Indexed by the enum value** — this list must stay in
    // Render::ShadowMapFormat's order.
    static const char *kFormatNames[] = {"D16", "D32"};
    int32_t formatIndex = static_cast<int32_t>(shadows.sun.format);
    if (ImGui::Combo("Depth Format", &formatIndex, kFormatNames, IM_ARRAYSIZE(kFormatNames)))
    {
        shadows.sun.format = static_cast<Assisi::Render::ShadowMapFormat>(formatIndex);
        changed = true;
    }

    // **Indexed by the enum value** — this list must stay in
    // Render::ShadowFilter's order.
    static const char *kFilterNames[] = {"1 tap", "3x3 PCF", "5x5 PCF", "Vogel (12 tap)"};
    int32_t filterIndex = static_cast<int32_t>(shadows.sun.filter);
    if (ImGui::Combo("Filter", &filterIndex, kFilterNames, IM_ARRAYSIZE(kFilterNames)))
    {
        shadows.sun.filter = static_cast<Assisi::Render::ShadowFilter>(filterIndex);
        changed = true;
    }

    changed |= ImGui::SliderFloat("Distance", &shadows.sun.maxDistance, Assisi::Render::kMinShadowDistance,
                                  Assisi::Render::kMaxShadowDistance, "%.0f m");
    // 1 is fully logarithmic (near cascades get most of the resolution), 0 fully
    // uniform (they get almost none).
    changed |= ImGui::SliderFloat("Split Lambda", &shadows.sun.splitLambda, Assisi::Render::kMinSplitLambda,
                                  Assisi::Render::kMaxSplitLambda, "%.2f");
    changed |= ImGui::SliderFloat("Cascade Blend", &shadows.sun.cascadeBlend, Assisi::Render::kMinCascadeBlend,
                                  Assisi::Render::kMaxCascadeBlend, "%.2f");

    // Both biases are quoted in texels and scaled per cascade by that cascade's
    // world-per-texel, so one number holds across all of them. Raise the depth
    // bias until acne clears; if the shadow detaches at contact before it does,
    // that is the normal offset's job instead.
    changed |= ImGui::SliderFloat("Depth Bias", &shadows.sun.depthBiasTexels, Assisi::Render::kMinDepthBiasTexels,
                                  Assisi::Render::kMaxDepthBiasTexels, "%.2f texels");
    changed |= ImGui::SliderFloat("Normal Offset", &shadows.sun.normalOffsetTexels,
                                  Assisi::Render::kMinNormalOffsetTexels, Assisi::Render::kMaxNormalOffsetTexels,
                                  "%.2f texels");
    changed |= ImGui::SliderFloat("Slope Bias", &shadows.sun.slopeBias, Assisi::Render::kMinSlopeBias,
                                  Assisi::Render::kMaxSlopeBias, "%.2f");

    if (!shadows.sun.enabled)
    {
        ImGui::EndDisabled();
    }

    // Diagnostics, not settings: runtime only and never persisted. Each one
    // changes the picture, which is why they are a list with an off entry rather
    // than checkboxes that could be left on by accident.
    static const char *const kShadowDebugNames[] = {"Off", "Cascades", "Occluder Margin", "Filter Taps"};
    static_assert(std::size(kShadowDebugNames) == Assisi::Render::kShadowDebugViewCount,
                  "The debug view list must name every ShadowDebugView.");
    int debugView = static_cast<int>(frame.renderer.ShadowDebugView());
    if (ImGui::Combo("Shadow View", &debugView, kShadowDebugNames,
                     static_cast<int>(Assisi::Render::kShadowDebugViewCount)))
    {
        frame.renderer.SetShadowDebugView(static_cast<Assisi::Render::ShadowDebugView>(debugView));
    }

    const Assisi::Render::ShadowPass::Stats stats = frame.renderer.LastShadowStats();
    ImGui::Text("Cascades: %u  |  %u instances / %u batches", stats.cascades, stats.instances, stats.batches);
    ImGui::Text("Casters culled: %u  |  %u draw calls", stats.culled, stats.drawCalls);

    if (changed)
    {
        frame.renderer.SetShadowSettings(shadows);
        frame.options.shadows = shadows;
        frame.options.SaveToJson();
    }
}

bool EditorOptionsPanel::Draw(const Frame &frame)
{
    bool applyDisplay = false;

    // The toggle lives here rather than in the engine, so nothing reserves F11 and a
    // game can rebind or drop it.
    if (frame.input.IsKeyPressed(Assisi::Window::Key::F11))
    {
        _showOptions = !_showOptions;
    }

    if (!_showOptions)
    {
        return false;
    }

    ImGui::SetNextWindowSize(ImVec2(320, 420), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Options", &_showOptions))
    {
        const int32_t frameHistory = static_cast<int32_t>(frame.cpuMs.size());

        // CPU against GPU frame time: whichever dominates is what the frame is bound
        // by. Both are averaged over the same window as the FPS counter; the plots
        // further down are raw per-frame samples, so spikes survive.
        const int32_t fps = frame.fps;
        ImGui::Text("CPU: %5.2f ms    GPU: %5.2f ms", frame.cpuFrameMs, frame.gpuFrameMs);
        ImGui::Text("Frame: %5.2f ms (%d FPS)", fps > 0 ? 1000.0 / fps : 0.0, fps);

        // Hardware telemetry (NVIDIA/NVML), next to the GPU frame time because it
        // explains it: a capped frame rate downclocks the GPU, so identical work
        // takes longer and GPU-ms rises with nothing about the scene changed. Low
        // clock and power beside a high GPU-ms is that, not a regression.
        const Assisi::Render::GpuTelemetrySample &gpu = _gpuTelemetry.Poll();
        if (gpu.valid)
        {
            ImGui::Text("%s", gpu.name.c_str());
            ImGui::Text("Clock: %u MHz core / %u MHz mem", gpu.coreClockMhz, gpu.memClockMhz);
            ImGui::Text("Util:  %u%% gpu / %u%% mem", gpu.gpuUtilPct, gpu.memUtilPct);
            if (!gpu.powerSupported)
                ImGui::Text("Power: N/A    Temp: %u C", gpu.temperatureC);
            else if (gpu.powerLimitWatts > 0.0)
                ImGui::Text("Power: %.0f / %.0f W    Temp: %u C", gpu.powerWatts, gpu.powerLimitWatts,
                            gpu.temperatureC);
            else
                ImGui::Text("Power: %.0f W    Temp: %u C", gpu.powerWatts, gpu.temperatureC);
            if (gpu.memTotalBytes > 0)
                // PRIu64, never a fixed %llu: uint64_t is `unsigned long` on Linux and
                // `unsigned long long` on Windows, so a literal is wrong on one of them.
                ImGui::Text("VRAM:  %" PRIu64 " / %" PRIu64 " MiB", gpu.memUsedBytes >> 20,
                            gpu.memTotalBytes >> 20);

            // One point per fresh NVML reading, not per frame. Poll() is throttled, so
            // the sequence bumps ~5x/s and the graphs span ~30 s of history whatever
            // the frame rate.
            if (gpu.sequence != _lastGpuSequence)
            {
                _lastGpuSequence = gpu.sequence;
                _gpuClockHistory[static_cast<std::size_t>(_gpuTelemetryOffset)] = static_cast<float>(gpu.coreClockMhz);
                _gpuUtilHistory[static_cast<std::size_t>(_gpuTelemetryOffset)]  = static_cast<float>(gpu.gpuUtilPct);
                _gpuPowerHistory[static_cast<std::size_t>(_gpuTelemetryOffset)] = static_cast<float>(gpu.powerWatts);
                _gpuTelemetryOffset = (_gpuTelemetryOffset + 1) % kGpuHistory;
                if (_gpuTelemetryCount < kGpuHistory)
                {
                    ++_gpuTelemetryCount;
                }
            }

            if (_gpuTelemetryCount > 0)
            {
                // Until the ring wraps the samples sit in [0, count) in order, so plot
                // from 0. Once it is full, the write cursor is the oldest sample, which
                // is what ImPlot's Offset wants.
                const int32_t plotCount  = _gpuTelemetryCount;
                const int32_t plotOffset = _gpuTelemetryCount < kGpuHistory ? 0 : _gpuTelemetryOffset;
                const auto bufMax     = [plotCount](const std::array<float, kGpuHistory> &buf)
                                        {
                                            float m = 0.0f;
                                            for (int32_t i = 0; i < plotCount; ++i)
                                            {
                                                m = std::max(m, buf[static_cast<std::size_t>(i)]);
                                            }
                                            return m;
                                        };

                // One compact history plot per metric. `title` is drawn above the plot
                // and carries the unit, which is why the y-axis label is empty; its
                // "###id" suffix keeps the ImGui id stable independent of that text.
                // Y tick labels stay on, so the scale is readable off the axis.
                const auto drawGpuPlot = [plotCount, plotOffset](const char *title,
                                                                 const std::array<float, kGpuHistory> &buf,
                                                                 float ymax, ImVec4 color)
                                         {
                                             ImPlotSpec spec;
                                             spec.LineColor  = color;
                                             spec.FillColor  = color;
                                             spec.FillAlpha  = 0.25f;
                                             spec.LineWeight = 1.5f;
                                             spec.Offset     = plotOffset;
                                             // NoInputs: the limits are re-locked every frame anyway, so pan and
                                             // zoom would do nothing except make the x-axis look like a
                                             // draggable control.
                                             if (ImPlot::BeginPlot(title, ImVec2(-1.0f, 100.0f),
                                                                   ImPlotFlags_NoMenus | ImPlotFlags_NoLegend | ImPlotFlags_NoInputs))
                                             {
                                                 ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoGridLines,
                                                                   ImPlotAxisFlags_NoHighlight);
                                                 ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, plotCount - 1, ImPlotCond_Always);
                                                 ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, static_cast<double>(ymax), ImPlotCond_Always);
                                                 ImPlot::PlotShaded(title, buf.data(), plotCount, 0.0, 1.0, 0.0, spec);
                                                 ImPlot::PlotLine(title, buf.data(), plotCount, 1.0, 0.0, spec);
                                                 ImPlot::EndPlot();
                                             }
                                         };

                const float clockMax = std::max(bufMax(_gpuClockHistory) * 1.1f, 500.0f);
                drawGpuPlot("GPU Clock (MHz)###gpuClock", _gpuClockHistory, clockMax,
                            ImVec4(0.30f, 0.75f, 0.40f, 1.0f));
                drawGpuPlot("GPU Utilization (%)###gpuUtil", _gpuUtilHistory, 100.0f,
                            ImVec4(0.35f, 0.60f, 0.95f, 1.0f));
                // Not every GPU reports power draw — laptops often do not.
                if (gpu.powerSupported)
                {
                    const float powerMax = gpu.powerLimitWatts > 0.0
                                               ? static_cast<float>(gpu.powerLimitWatts)
                                               : std::max(bufMax(_gpuPowerHistory) * 1.1f, 50.0f);
                    drawGpuPlot("GPU Power (W)###gpuPower", _gpuPowerHistory, powerMax,
                                ImVec4(0.95f, 0.55f, 0.25f, 1.0f));
                }
                else
                {
                    // A centred N/A in a framed box of exactly a plot's footprint (the
                    // same 100 px height), so the rest of the panel sits where it does
                    // on a GPU that does report power. A flat-zero line would read as a
                    // measurement.
                    if (ImGui::BeginChild("###gpuPowerNA", ImVec2(-1.0f, 100.0f), ImGuiChildFlags_Borders))
                    {
                        const ImVec2 start = ImGui::GetCursorStartPos();
                        const ImVec2 avail = ImGui::GetContentRegionAvail();

                        const char *title = "GPU Power (W)";
                        ImGui::SetCursorPosX(start.x + ((avail.x - ImGui::CalcTextSize(title).x) * 0.5f));
                        ImGui::TextUnformatted(title);

                        const char *label     = "N/A (Unsupported by this GPU)";
                        const ImVec2 labelSize = ImGui::CalcTextSize(label);
                        ImGui::SetCursorPos(ImVec2(start.x + ((avail.x - labelSize.x) * 0.5f),
                                                   start.y + ((avail.y - labelSize.y) * 0.5f)));
                        ImGui::TextDisabled("%s", label);
                    }
                    ImGui::EndChild();
                }
            }
        }
        else
        {
            ImGui::TextDisabled("GPU telemetry unavailable (NVML not found)");
        }

        // CPU and GPU share one y-axis, so their heights compare directly. The top is
        // floored at 4 ms, or an idle scene would magnify sub-millisecond jitter into
        // a mountain range. Both series read the ring buffers in place, ImPlot's
        // Offset marking the chronological start.
        float plotMax = 4.0f;
        for (int32_t i = 0; i < frameHistory; ++i)
        {
            plotMax = std::max({plotMax, frame.cpuMs[static_cast<std::size_t>(i)],
                                frame.gpuMs[static_cast<std::size_t>(i)]});
        }
        plotMax *= 1.1f; // headroom, so the peak is not pinned to the top edge

        // One spec per series, reused for the shaded fill and the outline so the two
        // cannot drift apart in colour.
        ImPlotSpec cpuSpec;
        cpuSpec.LineColor  = ImVec4(0.95f, 0.55f, 0.25f, 1.0f); // orange
        cpuSpec.FillColor  = cpuSpec.LineColor;
        cpuSpec.FillAlpha  = 0.25f;
        cpuSpec.LineWeight = 1.5f;
        cpuSpec.Offset     = frame.offset;

        ImPlotSpec gpuSpec;
        gpuSpec.LineColor  = ImVec4(0.30f, 0.75f, 0.40f, 1.0f); // green
        gpuSpec.FillColor  = gpuSpec.LineColor;
        gpuSpec.FillAlpha  = 0.25f;
        gpuSpec.LineWeight = 1.5f;
        gpuSpec.Offset     = frame.offset;

        if (ImPlot::BeginPlot("Frame Time (ms)###frameGraph", ImVec2(-1.0f, 120.0f),
                              ImPlotFlags_NoMenus | ImPlotFlags_NoInputs))
        {
            ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoGridLines,
                              ImPlotAxisFlags_NoHighlight);
            ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, frameHistory - 1, ImPlotCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, static_cast<double>(plotMax), ImPlotCond_Always);
            ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_Horizontal);

            ImPlot::PlotShaded("CPU", frame.cpuMs.data(), frameHistory, 0.0, 1.0, 0.0, cpuSpec);
            ImPlot::PlotLine("CPU", frame.cpuMs.data(), frameHistory, 1.0, 0.0, cpuSpec);
            ImPlot::PlotShaded("GPU", frame.gpuMs.data(), frameHistory, 0.0, 1.0, 0.0, gpuSpec);
            ImPlot::PlotLine("GPU", frame.gpuMs.data(), frameHistory, 1.0, 0.0, gpuSpec);

            ImPlot::EndPlot();
        }

        // Percentile stats over the frame-delta history. "1% low" is the average of the
        // slowest 1% of frames, which is where the stutter an average hides shows up.
        // Sorting a copy every frame is cheap at this sample count.
        if (frame.sampleCount > 0)
        {
            std::vector<float> sorted(frame.frameDeltaMs.begin(), frame.frameDeltaMs.begin() + frame.sampleCount);
            std::sort(sorted.begin(), sorted.end());

            double sum = 0.0;
            for (float ms : sorted)
            {
                sum += static_cast<double>(ms);
            }
            const double avgMs = static_cast<double>(sum) / static_cast<double>(sorted.size());

            // The tail of the sort, at least one frame however short the history is.
            const int32_t worstCount = std::max<int32_t>(1, static_cast<int32_t>(sorted.size()) / 100);
            double worstSum   = 0.0;
            for (int32_t i = static_cast<int32_t>(sorted.size()) - worstCount; i < static_cast<int32_t>(sorted.size()); ++i)
            {
                worstSum += static_cast<double>(sorted[static_cast<std::size_t>(i)]);
            }
            const double onePctLowMs = worstSum / static_cast<double>(worstCount);

            const float minMs = sorted.front();
            const float maxMs = sorted.back();

            const auto toFps = [](double ms) { return ms > 0.0 ? static_cast<int32_t>(1000.0 / ms) : 0; };
            ImGui::Text("Avg:     %6.2f ms  (%d FPS)", avgMs, toFps(avgMs));
            ImGui::Text("1%% low:  %6.2f ms  (%d FPS)", onePctLowMs, toFps(onePctLowMs));
            ImGui::Text("Min/Max: %6.2f / %6.2f ms", static_cast<double>(minMs), static_cast<double>(maxMs));
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Rendering");

        // A/B toggle for view-frustum culling; read it against the item tally below.
        // Culling only ever removes draws, so a culled count stuck at 0 with everything
        // on screen means it is inert here and explains nothing — fly until geometry
        // leaves the view and it should climb. Runtime only, not persisted.
        bool frustumCulling = frame.renderer.FrustumCulling();
        if (ImGui::Checkbox("Frustum Culling", &frustumCulling))
        {
            frame.renderer.SetFrustumCulling(frustumCulling);
        }

        // A/B toggle for draw-list sorting. The image is identical either way, so the
        // batch tally is the tell: sorting puts identical same-material meshes next to
        // each other, where they coalesce into one instanced indirect draw and the
        // batch count falls toward the number of distinct meshes. Off, it climbs toward
        // the drawn-item count, every item its own batch.
        bool sortDraws = frame.renderer.SortDraws();
        if (ImGui::Checkbox("Sort Draws", &sortDraws))
        {
            frame.renderer.SetSortDraws(sortDraws);
        }

        // A/B toggle for the GPU-driven cull path: a compute pass culls every object and
        // builds the indirect draw commands on the GPU, coalescing identical
        // (mesh, submesh) instances, so the CPU issues one drawIndexedIndirect instead
        // of extracting and sorting a draw list. The opaque image must come out
        // identical to the CPU path — that is what this toggle is for. On this path the
        // tallies below are read back from the GPU and run a few frames stale, and
        // "Sort Draws" does nothing. Runtime only, not persisted to options.json.
        bool gpuCulling = frame.renderer.GpuCulling();
        if (ImGui::Checkbox("GPU Cull", &gpuCulling))
        {
            frame.renderer.SetGpuCulling(gpuCulling);
        }

        // Show or hide the editor overlays — selection outline, entity icons, collider
        // wireframes — to see the scene as a game would render it. This only skips the
        // submissions in OnRender; the scene is untouched. Whether the overlay passes
        // exist at all is a separate, Initialize-time decision
        // (EditorConfig::enableEditorVisuals, --no-editor-visuals).
        ImGui::Checkbox("Editor Overlays", &frame.showEditorOverlays);

        const Assisi::Runtime::DrawStats draw = frame.renderer.LastDrawStats();
        ImGui::Text("Items: %u drawn / %u meshes culled", draw.drawnItems, draw.culledMeshes);
        ImGui::Text("Draws: %u batches / %u indirect calls", draw.batches, draw.drawCalls);

        // Short-circuits the mesh shader to a single material channel, to look at the
        // PBR inputs directly. Runtime only. **This list is indexed by the enum
        // value** — it must stay in Render::MaterialDebugView's order.
        static const char *kDebugViewNames[] = {"Off",       "Base Color", "Metallic", "Roughness",
                                                "Normal",    "Occlusion",  "Emissive"};
        int32_t debugViewIndex     = static_cast<int32_t>(frame.renderer.DebugView());
        if (ImGui::Combo("Debug View", &debugViewIndex, kDebugViewNames, IM_ARRAYSIZE(kDebugViewNames)))
        {
            frame.renderer.SetDebugView(static_cast<Assisi::Render::MaterialDebugView>(debugViewIndex));
        }

        ImGui::Separator();

        OptionsConfig &options = frame.options;

        DrawShadowSettings(frame);

        ImGui::Separator();

        // The look. Nothing here rebuilds a target — the values ride in the tone
        // map's push constants, so every edit lands on the next frame, which is
        // what makes these usable for comparing one against another.
        //
        // **Indexed by the enum value** — this list must stay in
        // Render::TonemapOperator's order.
        static const char *kOperatorNames[] = {"AgX", "ACES", "Reinhard"};
        int32_t operatorIndex = static_cast<int32_t>(options.tonemap.op);
        if (ImGui::Combo("Tone Map", &operatorIndex, kOperatorNames, IM_ARRAYSIZE(kOperatorNames)))
        {
            options.tonemap.op = static_cast<Assisi::Render::TonemapOperator>(operatorIndex);
            options.SaveToJson();
        }

        bool lookChanged = ImGui::SliderFloat("Exposure", &options.tonemap.exposureStops,
                                              Assisi::Render::kMinExposureStops,
                                              Assisi::Render::kMaxExposureStops, "%.2f stops");
        lookChanged |= ImGui::SliderFloat("Contrast", &options.tonemap.contrast, Assisi::Render::kMinContrast,
                                          Assisi::Render::kMaxContrast, "%.2f");
        lookChanged |= ImGui::SliderFloat("Saturation", &options.tonemap.saturation, Assisi::Render::kMinSaturation,
                                          Assisi::Render::kMaxSaturation, "%.2f");
        if (lookChanged)
        {
            options.SaveToJson();
        }

        // AgX is neutral by design and reads flat ungraded, so "no grade" is a
        // comparison point rather than a default worth returning to.
        if (ImGui::SmallButton("Punchy"))
        {
            options.tonemap.contrast   = Assisi::Render::kPunchyContrast;
            options.tonemap.saturation = Assisi::Render::kPunchySaturation;
            options.SaveToJson();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Neutral"))
        {
            options.tonemap.contrast   = 1.0f;
            options.tonemap.saturation = 1.0f;
            options.SaveToJson();
        }

        ImGui::Separator();

        static const char *kModeNames[] = {"Disabled", "MSAA", "FXAA", "MSAA + FXAA"};
        int modeIndex    = static_cast<int>(options.aaMode);
        if (ImGui::Combo("AA Mode", &modeIndex, kModeNames, 4))
        {
            options.aaMode = static_cast<Assisi::Render::AaMode>(modeIndex);
            applyDisplay = true;
            options.SaveToJson();
        }

        const bool msaaActive =
            (options.aaMode == Assisi::Render::AaMode::MSAA || options.aaMode == Assisi::Render::AaMode::MSAA_FXAA);
        if (!msaaActive)
        {
            ImGui::BeginDisabled();
        }

        static const char *kSampleNames[]  = {"2x", "4x", "8x"};
        static const int32_t kSampleValues[] = {2, 4, 8};
        int sampleIndex     = 1;
        for (int32_t i = 0; i < 3; ++i)
        {
            if (kSampleValues[i] == options.msaaSamples)
            {
                sampleIndex = i;
                break;
            }
        }

        if (ImGui::Combo("MSAA Samples", &sampleIndex, kSampleNames, 3))
        {
            options.msaaSamples = kSampleValues[sampleIndex];
            if (msaaActive)
            {
                applyDisplay = true;
            }
            options.SaveToJson();
        }

        if (!msaaActive)
        {
            ImGui::EndDisabled();
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Frame Sync");

        // VSync and an FPS cap are mutually exclusive, hence radio buttons. Only the
        // option is written here; Application::Run() switches the swapchain's present
        // mode between frames, which is the only safe place to do it.
        int frameSyncIndex = static_cast<int>(options.frameSync);
        bool frameSyncChanged =
            ImGui::RadioButton("VSync", &frameSyncIndex, static_cast<int>(FrameSyncMode::VSync));
        ImGui::SameLine();
        frameSyncChanged |=
            ImGui::RadioButton("FPS Limit", &frameSyncIndex, static_cast<int>(FrameSyncMode::FpsLimit));
        if (frameSyncChanged)
        {
            options.frameSync = static_cast<FrameSyncMode>(frameSyncIndex);
            options.SaveToJson();
        }

        // The cap sub-controls are live only in FpsLimit mode. "Unlimited" is the -1
        // sentinel, and greys out Max FPS in turn.
        const bool fpsMode = (options.frameSync == FrameSyncMode::FpsLimit);

        if (!fpsMode)
        {
            ImGui::BeginDisabled();
        }
        bool unlimited = (options.fpsLimit < 0);
        if (ImGui::Checkbox("Unlimited", &unlimited))
        {
            // Leaving "unlimited" seeds a usable cap rather than dropping the user into
            // an empty field.
            options.fpsLimit = unlimited ? static_cast<std::int16_t>(-1) : static_cast<std::int16_t>(60);
            options.SaveToJson();
        }
        if (!fpsMode)
        {
            ImGui::EndDisabled();
        }

        const bool capFieldEnabled = fpsMode && !unlimited;
        if (!capFieldEnabled)
        {
            ImGui::BeginDisabled();
        }
        // Commit on IsItemDeactivatedAfterEdit, not on the InputInt's return: while the
        // field is being typed in it reports every intermediate value, so "120" passes
        // through 1 and 12 and the live pacer would follow each one. `capFps` carries
        // the half-typed number until Enter, or a click or tab away.
        int capFps = (options.fpsLimit > 0) ? options.fpsLimit : 60;
        ImGui::InputInt("Max FPS", &capFps);
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            // A cap of 0 or less is not a cap; the ceiling is what int16 holds.
            capFps            = std::clamp(capFps, 1, static_cast<int>(INT16_MAX));
            options.fpsLimit  = static_cast<std::int16_t>(capFps);
            options.SaveToJson();
        }
        if (!capFieldEnabled)
        {
            ImGui::EndDisabled();
        }
    }
    ImGui::End();
    return applyDisplay;
}

} // namespace Assisi::Editor
