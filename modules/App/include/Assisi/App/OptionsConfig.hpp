#pragma once

/// @file OptionsConfig.hpp
/// @brief User-facing runtime options persisted to options.json.

#include <Assisi/Render/PostProcess.hpp>

namespace Assisi::App
{

/// @brief User preferences loaded from and saved to options.json in the working directory.
struct OptionsConfig
{
    Render::AaMode aaMode      = Render::AaMode::None;
    int            msaaSamples = 4; ///< MSAA sample count; valid values: 2, 4, 8.

    /// @brief Reads options.json from the working directory.
    /// Returns defaults if the file is missing or malformed.
    static OptionsConfig LoadFromJson();

    /// @brief Writes the current settings to options.json in the working directory.
    void SaveToJson() const;
};

} // namespace Assisi::App