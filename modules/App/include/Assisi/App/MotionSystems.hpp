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

} // namespace Assisi::App
