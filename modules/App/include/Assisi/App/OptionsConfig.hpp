#pragma once

/// @file OptionsConfig.hpp
/// @brief User-facing runtime options persisted to options.json.

namespace Assisi::App
{

/// @brief Anti-aliasing technique selection.
enum class AaMode
{
    None,     ///< No anti-aliasing.
    MSAA,     ///< Multisample AA (hardware, geometry edges only).
    FXAA,     ///< Fast approximate AA (post-process, covers specular highlights).
    MSAA_FXAA ///< MSAA resolved into an FXAA pass for best quality.
};

/// @brief User preferences loaded from and saved to options.json in the working directory.
struct OptionsConfig
{
    AaMode aaMode     = AaMode::None;
    int    msaaSamples = 4; ///< MSAA sample count; valid values: 2, 4, 8.

    /// @brief Reads options.json from the working directory.
    /// Returns defaults if the file is missing or malformed.
    static OptionsConfig LoadFromJson();

    /// @brief Writes the current settings to options.json in the working directory.
    void SaveToJson() const;
};

} // namespace Assisi::App