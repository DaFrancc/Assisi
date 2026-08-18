/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file OptionsConfig.hpp
/// @brief User-facing runtime options persisted to options.json.

#include <Assisi/Render/PostProcess.hpp>
#include <Assisi/Render/ShadowSettings.hpp>

#include <cstdint>

namespace Assisi::App
{

/// @brief How the frame rate is governed. The two modes are mutually exclusive —
/// exactly one paces the loop at a time.
enum class FrameSyncMode : std::uint8_t
{
    VSync,    ///< FIFO present mode — frame rate locked to the display refresh; fpsLimit is ignored.
    FpsLimit, ///< IMMEDIATE present mode — frame rate governed by OptionsConfig::fpsLimit.
};

/// @brief User preferences loaded from and saved to options.json under the user root.
struct OptionsConfig
{
    Render::AaMode aaMode      = Render::AaMode::None;
    int32_t msaaSamples = 4;        ///< MSAA sample count; valid values: 2, 4, 8.

    /// @brief Tone curve, exposure and grade. Sanitized on load — the file is
    /// hand-editable and these lanes reach a shader.
    Render::TonemapSettings tonemap;

    /// @brief The sun's cascade knobs. Sanitized on load for the same reason,
    /// and one more: these size a GPU allocation, so a hand-typed resolution
    /// reaches createTexture if nothing clamps it.
    Render::ShadowSettings shadows;

    FrameSyncMode frameSync = FrameSyncMode::VSync;

    /// @brief Target frame rate, applied only when frameSync == FpsLimit. Sentinel
    /// values: -1 means unlimited (no CPU-side cap); 0 is invalid and never stored.
    /// Any positive value is the FPS cap the frame pacer targets.
    std::int16_t fpsLimit = -1;

    /// @brief Reads options.json from the user root.
    /// Returns defaults if the file is missing or malformed.
    static OptionsConfig LoadFromJson();

    /// @brief Writes the current settings to options.json under the user root.
    void SaveToJson() const;
};

} // namespace Assisi::App