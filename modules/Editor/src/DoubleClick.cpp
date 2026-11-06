/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/DoubleClick.hpp>

namespace Assisi::Editor
{

bool DoubleClick::Press(double seconds, glm::vec2 cursor)
{
    const bool completes = _waiting && seconds - _seconds <= kDoubleClickSeconds &&
                           glm::length(cursor - _cursor) <= kDoubleClickPixels;
    _waiting = !completes;
    _seconds = seconds;
    _cursor = cursor;
    return completes;
}

} // namespace Assisi::Editor
