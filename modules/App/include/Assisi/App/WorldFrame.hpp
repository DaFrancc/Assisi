/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file WorldFrame.hpp
/// @brief The once-a-frame parts of one world's frame, around the fixed steps
///        App::StepWorld runs: the same functions for the editor and the game.

namespace Assisi::Core
{
class EventQueue;
} // namespace Assisi::Core

namespace Assisi::Render
{
class AssetCache;
} // namespace Assisi::Render

namespace Assisi::App
{

struct SystemContext;
struct World;

/// @brief The start of `ctx.world`'s frame, before any of the frame's fixed
/// steps: its PreUpdate systems. What was pushed into the world's events by
/// then waits for its next tick if the frame runs none.
void BeginWorldFrame(const SystemContext &ctx);

/// @brief `ctx.world`'s frame after its fixed steps: its Update systems.
void UpdateWorld(const SystemContext &ctx);

/// @brief Where PoseWorld spent its time, added to on every call that is given one.
struct PoseTimes
{
    double placing = 0.0;      ///< Placing every entity before the poses (s).
    double posing = 0.0;       ///< Turning poses into joint matrices (s).
    double placingAgain = 0.0; ///< Placing what the poses moved (s).
};

/// @brief The pose stage, after UpdateWorld and before PresentWorld: every
/// entity in @p world is placed, each SkinnedMesh's final pose is built from
/// its animated pose and turned into joint matrices, and what that moved is
/// placed again. What runs after it reads this frame's joints.
///
/// Its order is fixed: placing, the final poses, joint matrices, placing
/// again. The engine has no steps that adjust a pose after the animation yet;
/// they belong between the final poses and the joint matrices, in this order:
/// warping, limb IK, look-at, limb planting. Entities that follow joints
/// belong after the joint matrices, so the second placing puts them, and their
/// children, where this frame's joints are. Placing visits only what changed
/// since it last ran, so the second costs little.
///
/// Every program runs it, a dedicated server too, where no mesh loads and so
/// nothing is posed. For the world drawn this frame, pass the renderer's
/// @p cache, which keeps each character's posed vertices in the same pass
/// (Runtime::FinishPoses). Marks @p world posed until EndWorldFrame. @p times,
/// when given, is added to.
void PoseWorld(World &world, Render::AssetCache *cache = nullptr, PoseTimes *times = nullptr);

/// @brief PoseWorld, unless @p world has been posed this frame: for drawing a
/// world that ran no Update this frame, as in the editor's edit mode or while
/// paused, without posing one that did twice.
void EnsurePosed(World &world, Render::AssetCache *cache);

/// @brief What this machine shows and plays of `ctx.world`, after UpdateWorld:
/// its Ui systems, then its sounds (PlayWorldSounds), so a sound asked for in
/// any phase is heard this frame. A dedicated server, which shows and plays
/// nothing, does not run it.
void PresentWorld(const SystemContext &ctx);

/// @brief The end of @p world's frame, once the frame is drawn: its events end
/// with it (Core::EventQueue::EndFrame), keeping only what waits for its next
/// tick while it is @p ticking, and it is no longer posed. One that is not
/// ticking, as a paused world is not, keeps nothing and lets what waits in
/// @p universal go by too.
void EndWorldFrame(World &world, Core::EventQueue &universal, bool ticking);

} // namespace Assisi::App
