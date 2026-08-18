/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file EditorOptionsPanel.hpp
/// @brief The F11 options overlay, and the telemetry history behind its graphs.

#include <Assisi/Render/GpuTelemetry.hpp>

#include <array>
#include <cstdint>

#include <Assisi/App/OptionsConfig.hpp>
#include <Assisi/Window/InputContext.hpp>

#include <span>

namespace Assisi::Runtime
{
class SceneRenderer;
}

namespace Assisi::Editor
{

/// @brief The F11 overlay: a CPU/GPU frame graph, percentile frame-time stats,
/// and the anti-aliasing / frame-sync controls.
///
/// Lives in the editor rather than the engine base class. Application exposes the
/// timing history and the persisted OptionsConfig; this assembles a debug UI on
/// top, and a game built on the template can restyle or drop it without editing
/// the engine.
///
/// It takes the frame's data rather than the app that produced it. That keeps
/// the panel out of Application's inheritance chain — the timing history is
/// protected, readable only by a derived class — and makes the whole dependency
/// surface the struct below.
class EditorOptionsPanel
{
public:
    /// @brief One frame's worth of everything the overlay reads or writes.
    struct Frame
    {
        Window::InputContext &input;
        Runtime::SceneRenderer &renderer;
        App::OptionsConfig &options;
        bool &showEditorOverlays;

        int32_t fps;
        double cpuFrameMs;
        double gpuFrameMs;

        /// The rolling history behind the graphs: ring buffers of `sampleCount`
        /// samples, `offset` being the oldest and the next slot to overwrite.
        ///
        /// **Fill all five from one source.** The panel indexes the three spans
        /// by each other's length and walks `frameDeltaMs` to `sampleCount`
        /// without re-checking, so spans of differing lengths — or a
        /// `sampleCount` past their end — read out of bounds silently. The only
        /// caller builds them from a single `Application::FrameStatsView`, which
        /// is what keeps that true.
        std::span<const float> cpuMs;
        std::span<const float> gpuMs;
        std::span<const float> frameDeltaMs;
        int32_t offset;
        int32_t sampleCount;
    };

    /// @brief Draw the overlay if it is open, and handle its F11 toggle.
    /// @return Whether a display setting changed and the render targets need
    ///         rebuilding — the caller applies it, because only it can.
    [[nodiscard]] bool Draw(const Frame &frame);

private:
    /// @brief The sun-shadow section of the overlay: the tier presets, the
    /// cascade knobs, the biases, and the cascade debug view.
    ///
    /// Split out because it is a third of the panel on its own, and because the
    /// two halves of it persist differently — the knobs are saved settings, the
    /// cascade view is a runtime look.
    static void DrawShadowSettings(const Frame &frame);

    bool _showOptions = false;

    /// NVIDIA GPU telemetry (clocks/power/util/temp). Initialises NVML on first
    /// poll, so it costs nothing until the overlay is opened, and reports an
    /// invalid sample on non-NVIDIA systems.
    Assisi::Render::GpuTelemetry _gpuTelemetry;

    /// Ring buffers behind the telemetry graphs, advanced once per fresh NVML
    /// sample (~5 Hz, gated on `GpuTelemetrySample::sequence`) rather than per
    /// frame, so they span ~30 s whatever the frame rate. `_gpuTelemetryOffset`
    /// is the next write slot and the chronological start ImPlot wants;
    /// `_gpuTelemetryCount` saturates at the capacity. Only advanced while the
    /// overlay is open.
    static constexpr int32_t kGpuHistory = 150;       // ~30 s at 5 Hz
    std::array<float, kGpuHistory> _gpuClockHistory{};
    std::array<float, kGpuHistory> _gpuUtilHistory{};
    std::array<float, kGpuHistory> _gpuPowerHistory{};
    int32_t _gpuTelemetryOffset = 0;
    int32_t _gpuTelemetryCount  = 0;
    uint64_t _lastGpuSequence    = 0;
};

} // namespace Assisi::Editor
