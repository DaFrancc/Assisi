/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file EditorChiaraPanel.hpp
/// @brief The capture control panel.

#include <Assisi/App/Application.hpp>

namespace Assisi::Editor
{

/// @brief Draw the capture controls — recording toggle, ring coverage, and the
/// dump buttons — into the current UI frame.
///
/// Draws contents, not a window, so the caller puts it wherever it likes. Draws
/// nothing in a build without the capture system.
void DrawChiaraPanel(App::Application &app);

} // namespace Assisi::Editor
