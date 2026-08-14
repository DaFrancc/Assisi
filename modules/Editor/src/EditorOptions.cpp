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
