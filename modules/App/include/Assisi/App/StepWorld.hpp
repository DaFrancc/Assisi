/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file StepWorld.hpp
/// @brief One fixed step of one world: the same function for the editor, the
///        game, a dedicated server, and a client stepping a tick again.

#include <Assisi/ECS/Entity.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

#include <span>

namespace Assisi::App
{

struct SystemContext;

/// @brief Steps `ctx.world` by `ctx.dt` at tick `ctx.simTick`: its FixedUpdate
/// systems, the physics step, then its PostFixedUpdate systems, all inside one
/// ECS::FixedStepScope and all under @p ctx.
///
/// What a step does around this belongs to the host that calls it: polling the
/// network and handing players their controls before, sending a snapshot or
/// a command after. Nothing here asks which host it is in, which is what makes
/// a step the same on every machine.
///
/// Live (`ctx.replaying` false), the physics step is the whole PhysicsWorld
/// update and @p replayedCharacters and @p followers must be empty. Replaying,
/// it steps only the characters of @p replayedCharacters, as a live step would,
/// with each of @p followers moving as it did live and everything else as it
/// stands (PhysicsWorld::StepCharacters), and only the systems marked
/// `replayed` run.
void StepWorld(const SystemContext &ctx, std::span<const ECS::Entity> replayedCharacters = {},
               std::span<const Physics::FollowerStep> followers = {});

} // namespace Assisi::App
