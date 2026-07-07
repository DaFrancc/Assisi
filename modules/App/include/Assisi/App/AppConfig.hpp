#pragma once

/// @file AppConfig.hpp
/// @brief Engine configuration loaded from assets/game.json.

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Backend/GraphicsBackend.hpp>

#include <string>

namespace Assisi::App
{

struct AppConfig
{
    std::string title      = "Assisi Game";
    int         width      = 1280;
    int         height     = 720;
    glm::vec4   clearColor = {0.15f, 0.15f, 0.18f, 1.f};
    double      physicsHz  = 60.0;
    double      renderHz   = 144.0;

    /// @brief Which graphics API to initialize. Vulkan is under active migration —
    /// OpenGL remains the default until the Vulkan path covers full engine rendering.
    Render::Backend::GraphicsBackend backend = Render::Backend::GraphicsBackend::OpenGL;

    /// @brief Reads assets/game.json via the asset system.
    /// Falls back to defaults if the file is missing or malformed.
    static AppConfig LoadFromJson();
};

} // namespace Assisi::App
