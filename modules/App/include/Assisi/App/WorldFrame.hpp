/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file WorldFrame.hpp
/// @brief The once-a-frame parts of one world's frame, around the fixed steps
///        App::StepWorld runs: the same functions for the editor and the game.

namespace Assisi::Core
{
class EventQueue;
} // namespace Assisi::Core

namespace Assisi::App
{

struct SystemContext;
struct World;

/// @brief The start of `ctx.world`'s frame, before any of the frame's fixed
/// steps: its PreUpdate systems. What was pushed into the world's events by
/// then waits for its next tick if the frame runs none.
void BeginWorldFrame(const SystemContext &ctx);

/// @brief `ctx.world`'s frame after its fixed steps: its Update systems, its Ui
/// systems, then its sounds (PlayWorldSounds), so a sound asked for in any
/// phase is heard this frame.
void UpdateWorld(const SystemContext &ctx);

/// @brief The end of @p world's frame, once the frame is drawn: its events end
/// with it (Core::EventQueue::EndFrame), keeping only what waits for its next
/// tick while it is @p ticking. One that is not ticking, as a paused world is
/// not, keeps nothing and lets what waits in @p universal go by too.
void EndWorldFrame(World &world, Core::EventQueue &universal, bool ticking);

} // namespace Assisi::App
