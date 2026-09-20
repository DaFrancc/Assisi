/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CursorShape.hpp
/// @brief What the pointer looks like, which is how it says what it would do
/// where it stands.
///
/// Here rather than in the window or the UI because both have to say it and
/// neither may depend on the other: the UI names the shape it wants, the
/// window installs it, and this is the word they share.

#include <cstddef>
#include <cstdint>

namespace Assisi::Core
{

/// @brief The shapes a desktop offers, so a control names the one it means
/// rather than the nearest one that happens to exist.
///
/// Not every platform has every shape: the diagonal resizes and NotAllowed
/// come from a newer standard that not all cursor themes carry on Linux. One
/// the platform lacks leaves the pointer as it was.
enum class CursorShape : uint8_t
{
    Arrow,      ///< the platform's own default, and the only shape a captured pointer has
    Text,       ///< the I-beam over text that can be selected or typed into
    Hand,       ///< over something that follows when clicked
    Crosshair,  ///< over something aimed at rather than pressed
    ResizeX,    ///< a vertical edge, dragged left and right
    ResizeY,    ///< a horizontal edge, dragged up and down
    ResizeFall, ///< a corner, dragged down-right and up-left
    ResizeRise, ///< a corner, dragged up-right and down-left
    /// Taking hold of something to move it in any direction. Drawn as a
    /// grabbing hand on some platforms and a four-headed arrow on others,
    /// which is as near as a grabbing hand can be asked for: there is no
    /// standard shape of one.
    Move,
    NotAllowed, ///< over something a drag cannot be dropped on
    Count
};

inline constexpr std::size_t kCursorShapeCount = static_cast<std::size_t>(CursorShape::Count);

} // namespace Assisi::Core
