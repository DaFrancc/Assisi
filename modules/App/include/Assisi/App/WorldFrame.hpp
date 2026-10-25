/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file WorldFrame.hpp
/// @brief The once-a-frame parts of one world's frame, around the fixed steps
///        App::StepWorld runs: the same functions for the editor and the game.

namespace Assisi::App
{

struct SystemContext;

/// @brief The start of `ctx.world`'s frame, before any of the frame's fixed
/// steps: its PreUpdate systems.
void BeginWorldFrame(const SystemContext &ctx);

/// @brief `ctx.world`'s frame after its fixed steps: its Update systems, its Ui
/// systems, then its sounds (PlayWorldSounds), so a sound asked for in any
/// phase is heard this frame.
void UpdateWorld(const SystemContext &ctx);

} // namespace Assisi::App
