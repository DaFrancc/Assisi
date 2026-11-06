/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file DoubleClick.hpp
/// @brief Tells a press that completes a double-click from any other.
///
/// Counted from the presses the editor itself sees rather than read from
/// ImGui's flag, which ImGui sets when it draws, after the editor's update has
/// already handled the press: read from the update, the flag and the press are
/// never on the same frame.

#include <Assisi/Math/GLM.hpp>

namespace Assisi::Editor
{

/// @brief How soon after the first press the second must come, in seconds, and
///        how near it, in pixels. ImGui's own defaults, so a double-click feels
///        the same over a panel and over the viewport.
inline constexpr double kDoubleClickSeconds = 0.3;
inline constexpr float kDoubleClickPixels = 6.f;

/// @brief The presses of one button, as they arrive.
class DoubleClick
{
public:
    /// @brief Records a press at @p seconds and @p cursor, and says whether it
    ///        completes a double-click.
    ///
    /// A press that completes one does not start the next, so three quick
    /// presses are a double-click and then a single.
    [[nodiscard]] bool Press(double seconds, glm::vec2 cursor);

private:
    double _seconds = 0.0;
    glm::vec2 _cursor{0.f};
    bool _waiting = false; ///< Whether the last press could be the first of a double-click.
};

} // namespace Assisi::Editor
