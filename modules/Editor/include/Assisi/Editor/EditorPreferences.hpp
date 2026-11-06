/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file EditorPreferences.hpp
/// @brief The editor's own settings, kept in editor.json beside options.json.
///
/// A file of their own because options.json is the player's: the game writes it
/// whole from its own settings, which would drop anything else kept there.

#include <string>
#include <string_view>

#include <Assisi/Math/GLM.hpp>

namespace Assisi::Editor
{

/// @brief The narrowest and widest a spline's curve may be drawn, in pixels.
inline constexpr float kMinSplineThickness = 1.f;
inline constexpr float kMaxSplineThickness = 8.f;

/// @brief How splines are drawn in the viewport.
struct SplinePreferences
{
    /// The colour of splines on entities that are not selected. A selected
    /// entity's draw in the selection's colours, so they never read as these.
    glm::vec4 color{1.0f, 0.35f, 0.85f, 0.9f};

    /// How wide every spline's curve is drawn, in pixels.
    float thickness = 2.f;

    bool operator==(const SplinePreferences &) const = default;
};

/// @brief Everything editor.json holds.
struct EditorPreferences
{
    SplinePreferences splines;

    bool operator==(const EditorPreferences &) const = default;
};

/// @brief The preferences @p text describes.
///
/// A setting that is missing or of the wrong kind keeps its default, and one
/// out of range is brought into it, so a file edited by hand never stops the
/// editor starting. Each one passed over is logged.
[[nodiscard]] EditorPreferences ParseEditorPreferences(std::string_view text);

/// @brief @p preferences as editor.json's text.
[[nodiscard]] std::string FormatEditorPreferences(const EditorPreferences &preferences);

/// @brief editor.json from the user root, or the defaults when there is none.
[[nodiscard]] EditorPreferences LoadEditorPreferences();

/// @brief Writes @p preferences to editor.json in the user root, logging a
///        failure.
void SaveEditorPreferences(const EditorPreferences &preferences);

} // namespace Assisi::Editor
