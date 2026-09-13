/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file MotionSystems.hpp
/// @brief Ready-made systems that turn the motion components in
///        Assisi::Runtime into behaviour.
///
/// Same arrangement as PhysicsSystems.hpp and for the same reason: a system
/// needs a SystemContext, and App is the layer that owns one. Plain functions,
/// never installed automatically — a level names the ones it wants.

#include <Assisi/Core/Reflect/Annotations.hpp>

namespace Assisi::App
{

struct SystemContext;

/// @brief Moves every Runtime::Oscillator entity along its axis.
///
/// Evaluates the closed form `origin + axis * amplitude * sin(2pi * (t / period
/// + phase))` from the fixed-step tick, rather than integrating a step per
/// frame. Two consequences, both of which are the point:
///
/// - The path cannot drift. An integrator accumulates its own rounding, so a
///   mover left running long enough ends up somewhere its authoring never said.
/// - The pose at a given tick is the same in every run, and the same on every
///   machine, whatever the frame rate did in between. A measurement scene whose
///   movers were somewhere slightly different each run would fold that
///   difference into the frame times it exists to report.
///
/// @par Requirements
/// Register it in **FixedUpdate**, whose tick is the clock it reads. In Update
/// the tick repeats within a frame, so the movers would stall and jump rather
/// than travel.
///
/// Entities with a zero `axis` or a non-positive `periodSeconds` are left alone
/// — that is how a mover is parked without removing the component.
ASYSTEM(FixedUpdate) void OscillateSystem(SystemContext &ctx);

/// @brief Advances the scene's clock, daily and annual.
///
/// **Turns nothing and aims nothing.** All this does is move two numbers; where
/// the sun and the moon end up is derived from those numbers every frame by
/// Runtime::ResolveSky, which is what makes the first frame after a load or a
/// time jump correct with no tick in between, and what lets the editor scrub the
/// clock with play stopped.
///
/// That is the opposite trade from the daylight cycle this replaces, which
/// integrated the light's own aim and so consumed it: stopping mid-cycle left the
/// sun wherever it happened to be, and the level file recorded a direction rather
/// than an hour.
///
/// @par Requirements
/// Register it in **FixedUpdate**. The step is simulated time, so a day advances
/// at the same rate whatever the frame rate did.
///
/// Each clock is held by its own pause flag, so a frozen season under a running
/// day — the noon shadow swinging through the year — is a view rather than an
/// error.
ASYSTEM(FixedUpdate) void TimeOfDaySystem(SystemContext &ctx);

} // namespace Assisi::App
