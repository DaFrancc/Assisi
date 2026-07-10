/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file SandboxOptions.cpp
/// @brief The F12 options overlay: a CPU/GPU frame graph, percentile frame-time
/// stats, and the anti-aliasing / frame-sync controls.
///
/// This lives in the app, not the engine base class: Application exposes the
/// timing history and the persisted OptionsConfig, and the template assembles
/// the debug UI on top. A game built on this template can restyle, rebind, or
/// drop the overlay without editing the engine (see the round-3 review item
/// "Application is a framework and a debug tool at once").

#include "SandboxApp.hpp"

#include <Assisi/App/OptionsConfig.hpp>
#include <Assisi/Render/PostProcess.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/Key.hpp>

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cstdint>
#include <vector>

using Assisi::App::Application;
using Assisi::App::FrameSyncMode;
using Assisi::App::OptionsConfig;

void SandboxApp::DrawOptionsWindow()
{
    // F12 toggles the overlay. Handled here, in the app, so the engine no longer
    // reserves the key — a game can rebind or remove this freely.
    if (GetInput().IsKeyPressed(Assisi::Window::Key::F12))
    {
        _showOptions = !_showOptions;
    }

    if (!_showOptions)
    {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(320, 420), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Options", &_showOptions))
    {
        const Application::FrameStatsView stats = GetFrameStats();
        const int                         frameHistory = static_cast<int>(stats.cpuMs.size());

        // CPU vs GPU frame time: if CPU >> GPU we're CPU-bound, and vice versa.
        // The numbers are averaged over the same ~0.5s window as the FPS counter;
        // the plots below show raw per-frame samples so spikes stay visible.
        const int fps = GetFps();
        ImGui::Text("CPU: %5.2f ms    GPU: %5.2f ms", GetCpuFrameMs(), GetGpuFrameMs());
        ImGui::Text("Frame: %5.2f ms (%d FPS)", fps > 0 ? 1000.0 / fps : 0.0, fps);

        // Combined CPU/GPU plot on one shared y-axis so their heights are directly
        // comparable. Floor the top at 4 ms so an idle scene doesn't magnify sub-ms
        // jitter; the Y axis ticks give the top/bottom limits, and the legend
        // toggles each series. Both series read the ring buffers directly via
        // ImPlot's offset argument, which marks the chronological start.
        float plotMax = 4.0f;
        for (int i = 0; i < frameHistory; ++i)
        {
            plotMax = std::max({plotMax, stats.cpuMs[i], stats.gpuMs[i]});
        }
        plotMax *= 1.1f; // headroom so the peak isn't pinned to the top edge

        // One ImPlotSpec per series carries its color, fill alpha, and the ring
        // buffer's Offset (chronological start); reused for both the shaded fill
        // and the outline so the two stay the same color.
        ImPlotSpec cpuSpec;
        cpuSpec.LineColor  = ImVec4(0.95f, 0.55f, 0.25f, 1.0f); // orange
        cpuSpec.FillColor  = cpuSpec.LineColor;
        cpuSpec.FillAlpha  = 0.25f;
        cpuSpec.LineWeight = 1.5f;
        cpuSpec.Offset     = stats.offset;

        ImPlotSpec gpuSpec;
        gpuSpec.LineColor  = ImVec4(0.30f, 0.75f, 0.40f, 1.0f); // green
        gpuSpec.FillColor  = gpuSpec.LineColor;
        gpuSpec.FillAlpha  = 0.25f;
        gpuSpec.LineWeight = 1.5f;
        gpuSpec.Offset     = stats.offset;

        if (ImPlot::BeginPlot("##frameGraph", ImVec2(-1.0f, 130.0f), ImPlotFlags_NoMenus))
        {
            ImPlot::SetupAxes(nullptr, "ms", ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoGridLines,
                              ImPlotAxisFlags_NoHighlight);
            ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, frameHistory - 1, ImPlotCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, plotMax, ImPlotCond_Always);
            ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_Horizontal);

            ImPlot::PlotShaded("CPU", stats.cpuMs.data(), frameHistory, 0.0, 1.0, 0.0, cpuSpec);
            ImPlot::PlotLine("CPU", stats.cpuMs.data(), frameHistory, 1.0, 0.0, cpuSpec);
            ImPlot::PlotShaded("GPU", stats.gpuMs.data(), frameHistory, 0.0, 1.0, 0.0, gpuSpec);
            ImPlot::PlotLine("GPU", stats.gpuMs.data(), frameHistory, 1.0, 0.0, gpuSpec);

            ImPlot::EndPlot();
        }

        // Percentile stats over the frame-delta history. "1% low" is the average
        // of the slowest 1% of frames (GamersNexus-style) — the stutter the
        // averages hide. Sorting a copy each frame is cheap at this sample count.
        if (stats.sampleCount > 0)
        {
            std::vector<float> sorted(stats.frameDeltaMs.begin(), stats.frameDeltaMs.begin() + stats.sampleCount);
            std::sort(sorted.begin(), sorted.end());

            double sum = 0.0;
            for (float ms : sorted)
            {
                sum += ms;
            }
            const double avgMs = sum / sorted.size();

            // Average the slowest 1% (at least one frame) from the tail.
            const int    worstCount = std::max<int>(1, static_cast<int>(sorted.size()) / 100);
            double       worstSum   = 0.0;
            for (int i = static_cast<int>(sorted.size()) - worstCount; i < static_cast<int>(sorted.size()); ++i)
            {
                worstSum += sorted[i];
            }
            const double onePctLowMs = worstSum / worstCount;

            const float minMs = sorted.front();
            const float maxMs = sorted.back();

            const auto toFps = [](double ms) { return ms > 0.0 ? static_cast<int>(1000.0 / ms) : 0; };
            ImGui::Text("Avg:     %6.2f ms  (%d FPS)", avgMs, toFps(avgMs));
            ImGui::Text("1%% low:  %6.2f ms  (%d FPS)", onePctLowMs, toFps(onePctLowMs));
            ImGui::Text("Min/Max: %6.2f / %6.2f ms", minMs, maxMs);
        }
        ImGui::Separator();

        OptionsConfig &options = GetOptions();

        static const char *kModeNames[] = {"Disabled", "MSAA", "FXAA", "MSAA + FXAA"};
        int                modeIndex    = static_cast<int>(options.aaMode);
        if (ImGui::Combo("AA Mode", &modeIndex, kModeNames, 4))
        {
            options.aaMode = static_cast<Assisi::Render::AaMode>(modeIndex);
            ApplyDisplayOptions();
            options.SaveToJson();
        }

        const bool msaaActive =
            (options.aaMode == Assisi::Render::AaMode::MSAA || options.aaMode == Assisi::Render::AaMode::MSAA_FXAA);
        if (!msaaActive)
        {
            ImGui::BeginDisabled();
        }

        static const char *kSampleNames[]  = {"2x", "4x", "8x"};
        static const int   kSampleValues[] = {2, 4, 8};
        int                sampleIndex     = 1;
        for (int i = 0; i < 3; ++i)
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
                ApplyDisplayOptions();
            }
            options.SaveToJson();
        }

        if (!msaaActive)
        {
            ImGui::EndDisabled();
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Frame Sync");

        // VSync and an FPS cap are mutually exclusive modes — radio buttons make
        // that visible. The swapchain present-mode switch happens between frames
        // in Application::Run(); here we only edit the option.
        int  frameSyncIndex = static_cast<int>(options.frameSync);
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

        // FPS-cap sub-controls: only live in FpsLimit mode (greyed under VSync).
        // "Unlimited" toggles the -1 sentinel (no cap) and greys out Max FPS.
        const bool fpsMode = (options.frameSync == FrameSyncMode::FpsLimit);

        if (!fpsMode)
        {
            ImGui::BeginDisabled();
        }
        bool unlimited = (options.fpsLimit < 0);
        if (ImGui::Checkbox("Unlimited", &unlimited))
        {
            // Leaving "unlimited" seeds a sane cap the user can then edit.
            options.fpsLimit = unlimited ? static_cast<std::int16_t>(-1) : static_cast<std::int16_t>(60);
            options.SaveToJson();
        }
        if (!fpsMode)
        {
            ImGui::EndDisabled();
        }

        // Max FPS is live only in FpsLimit mode with a finite cap; greyed otherwise.
        const bool capFieldEnabled = fpsMode && !unlimited;
        if (!capFieldEnabled)
        {
            ImGui::BeginDisabled();
        }
        // While the field is being typed in, InputInt reports every intermediate
        // value ("120" passes through 1 and 12). Commit only once the user finishes
        // editing — Enter or clicking/tabbing away — so the live pacer never sees a
        // half-typed cap. `fps` still tracks the in-progress edit for display.
        int capFps = (options.fpsLimit > 0) ? options.fpsLimit : 60;
        ImGui::InputInt("Max FPS", &capFps);
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            // Clamp to a valid positive int16 — 0 and negatives are invalid caps.
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
}
