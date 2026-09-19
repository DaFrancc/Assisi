/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file OptionsConfig.hpp
/// @brief User-facing runtime options persisted to options.json.

#include <Assisi/App/AppConfig.hpp>
#include <Assisi/Render/EnvironmentSettings.hpp>
#include <Assisi/Render/PostProcess.hpp>
#include <Assisi/Render/ShadowSettings.hpp>
#include <Assisi/Render/SsaoSettings.hpp>
#include <Assisi/Window/InputBindings.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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
///
/// The one writable settings file. Everything here is the player's, holds only
/// what they changed, and is safe to delete — doing so restores what the build
/// ships. Two kinds of value live here and resolve slightly differently: most
/// override a default that is a field initialiser below, while the window size
/// and the action bindings override a default that ships as a file under
/// `assets/config/`. Both are the player's, which is why they are together.
struct OptionsConfig
{
    /// @brief The tap interval the player chose, or nullopt to keep the game's
    /// standard. Ignored when the game holds everyone to its standard.
    std::optional<double> multiTapSeconds;

    /// @brief The window size the player chose, or nullopt to keep the shipped
    /// one. A size is stored only once it differs from what shipped, so a patch
    /// that changes the default reaches everyone who never picked a size.
    std::optional<int32_t> width;
    std::optional<int32_t> height;

    /// @brief The actions the player rebound, and only those.
    ///
    /// Applied over the shipped bindings per action, so an action absent here
    /// keeps what shipped. Empty on a fresh install.
    Window::InputBindings bindings;

    Render::AaMode aaMode = Render::AaMode::None;
    int32_t msaaSamples = 4; ///< MSAA sample count; valid values: 2, 4, 8.

    /// @brief Tone curve, exposure and grade. Sanitized on load — the file is
    /// hand-editable and these lanes reach a shader.
    Render::TonemapSettings tonemap;

    /// @brief The shadow knobs, in their sun and local halves. Sanitized on
    /// load for the same reason, and one more: these size a GPU allocation, so
    /// a hand-typed resolution reaches createTexture if nothing clamps it.
    Render::ShadowSettings shadows;

    /// @brief Whether the sky is reflected, and how finely. Sanitized on load:
    /// the resolution sizes the probe's cubes.
    Render::EnvironmentSettings environment;

    /// @brief Screen-space ambient occlusion. Sanitized on load: the sample
    /// count bounds a loop over a fixed-size kernel.
    Render::SsaoSettings ambientOcclusion;

    FrameSyncMode frameSync = FrameSyncMode::VSync;

    /// @brief Target frame rate, applied only when frameSync == FpsLimit. Sentinel
    /// values: -1 means unlimited (no CPU-side cap); 0 is invalid and never stored.
    /// Any positive value is the FPS cap the frame pacer targets.
    std::int16_t fpsLimit = -1;

    /// @brief The tap interval to run with: the player's when @p config lets
    /// players choose and they have, the game's standard otherwise.
    [[nodiscard]] double MultiTapSeconds(const AppConfig &config) const
    {
        return config.playerSetsMultiTap && multiTapSeconds ? *multiTapSeconds : config.multiTapSeconds;
    }

    /// @brief Parse @p text as an options document.
    ///
    /// Anything the document does not mention keeps its default, and a document
    /// that will not parse at all yields defaults entire — a file someone
    /// hand-edited into nonsense costs the settings, never the launch.
    [[nodiscard]] static OptionsConfig FromJsonText(std::string_view text);

    /// @brief The settings that differ from the defaults, as an options document,
    /// with floats rounded to four decimal places. Round-trips through
    /// FromJsonText: every field read there that is not written here is at its
    /// default, so a setting nobody changed follows the defaults as they change.
    [[nodiscard]] std::string ToJsonText() const;

    /// @brief Reads options.json from the user root.
    /// Returns defaults if the file is missing or malformed.
    static OptionsConfig LoadFromJson();

    /// @brief Writes the current settings to options.json under the user root.
    void SaveToJson() const;
};

} // namespace Assisi::App