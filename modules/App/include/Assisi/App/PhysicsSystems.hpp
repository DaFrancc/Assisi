/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PhysicsSystems.hpp
/// @brief Ready-made systems that turn the physics components in
///        Assisi::Physics into behaviour.
///
/// These live here rather than in Assisi::Physics because a system needs a
/// SystemContext, and App is the layer that owns one — Physics deliberately does
/// not depend on it. They are plain functions, never installed automatically: a
/// profile registers the ones its levels need (see WorldManager::RegisterProfile
/// and docs/world-system-binding-design-notes.md), so a world that has no use for
/// one does not run it.

namespace Assisi::App
{

struct SystemContext;

/// @brief Minimum closing speed (m/s) an impact must carry for BounceSystem to
/// respond to it at all. Anything gentler is left for the solver to absorb.
///
/// @warning At the current 1 mm/s this rejects only numerically-negligible
/// contacts. It is **not** enough to stop a settling body pumping itself, and the
/// numbers below are the reason — read them before tuning this either way.
///
/// A body settling onto a surface keeps exchanging fresh contacts at a few tenths
/// of a metre per second, and down there the solver's own penetration push-out
/// contributes more speed than the rebound multiplier removes. Measured on a box
/// settling at `rebound = 0.8` — a *lossy* bounce, which should decay
/// monotonically — successive impacts instead grew: 0.65, 0.78, 0.84, 0.96 m/s.
/// Below `rebound = 1` that stays sub-perceptual and the body does eventually
/// stop. At `rebound > 1` the same feedback compounds, and a box nudged at
/// 0.98 m/s climbs to 18.8 m and is still accelerating (TestBounce.cpp exercises
/// exactly this).
///
/// Raising this to **1.0** suppresses that entirely, and is the figure Jolt uses
/// for its own restitution cutoff (`PhysicsSettings::mMinVelocityForRestitution`)
/// for the same reason — the two would then give up at the same point instead of
/// one reviving what the other let rest. The cost is that impacts from drops under
/// roughly 5 cm stop bouncing at all. That trade is a game-feel decision, which is
/// why the value is a named constant here rather than buried in the system.
inline constexpr float kMinBounceSpeed = 0.001f;

/// @brief Ricochets Physics::Bounce entities off whatever their rigid body hits.
///
/// Reflects the body's incoming linear velocity about the contact normal and
/// scales it by the component's `rebound`. Nothing else is touched — no torque,
/// no spin change, no positional correction — so it composes with whatever else
/// is driving the body.
///
/// @par Requirements
/// Register it in **FixedUpdate**, which puts it immediately before its world's
/// physics step: it consumes the contacts the previous step found, and the
/// velocity it writes is the one the next step simulates, with no frame of wasted
/// motion in between. The world's PhysicsWorld must have contact reporting on
/// (PhysicsWorld::SetContactReporting) — without it there are no contacts and this
/// does nothing at all, silently. Registering it alongside `.RequireAny<Physics::Bounce>()`
/// keeps it out of the schedule entirely in worlds that have no bouncers.
///
/// @par Behaviour worth knowing
/// - Only *new* contacts bounce. A body already resting on a surface reports
///   nothing, so it stays put instead of being relaunched every step.
/// - Only *approaching* bodies bounce, so a speculative contact against something
///   the body is already moving away from cannot drive it back into the surface.
/// - Only impacts of at least @ref kMinBounceSpeed bounce, which is what lets a
///   bouncy body settle at all — see that constant for why the threshold is not
///   optional.
/// - One bounce per entity per step: a body landing in a corner touches two
///   surfaces, and reflecting twice would send it back where it came from.
void BounceSystem(SystemContext &ctx);

} // namespace Assisi::App
