/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Ui.hpp
/// @brief The game UI: one per windowed application, driven by two steps a frame.
///
/// The application calls ProcessInput directly after polling input, before any
/// fixed update can read what the UI means to consume, and Sync directly before
/// rendering, so what is drawn reflects the frame's final state rather than the
/// previous one's. Each step expects the other to have run in between; calling
/// either twice in a row is a bug in the host loop and asserts.

#include <Assisi/Mondrian/DrawList.hpp>

#include <cstdint>

namespace Assisi::Mondrian
{

class Ui
{
public:
    /// @brief Consume this frame's input. Must precede Sync.
    void ProcessInput();

    /// @brief Lay out against @p viewport and rebuild the draw list from
    /// scratch. Must follow ProcessInput.
    void Sync(Extent viewport);

    /// @brief What the last Sync produced. Stays valid until the next Sync, so
    /// a redraw between frames shows the same thing without laying out again.
    [[nodiscard]] const DrawList &GetDrawList() const { return _drawList; }

private:
    /// Which step the host loop owes next.
    enum class FrameStep : uint8_t
    {
        AwaitingInput,
        AwaitingSync,
        Count
    };

    DrawList _drawList;
    FrameStep _nextStep = FrameStep::AwaitingInput;
};

} // namespace Assisi::Mondrian
