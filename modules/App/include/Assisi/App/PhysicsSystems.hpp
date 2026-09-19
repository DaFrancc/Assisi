/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PhysicsSystems.hpp
/// @brief Ready-made systems that turn the physics components in
///        Assisi::Physics into behaviour.
///
/// These live here rather than in Assisi::Physics because a system needs a
/// SystemContext, and App is the layer that owns one — Physics deliberately does
/// not depend on it. They are plain functions, never installed automatically: a
/// file names the ones it needs, so a world
/// that has no use for one does not run it.

#include <Assisi/Core/Reflect/Annotations.hpp>

#include <string_view>

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
/// motion in between. It turns contact reporting on for its own world, so a level
/// that names it does not also have to ask for reporting — one branch per fixed
/// step, and it survives a world that turns reporting back off. The cost is that
/// the very first step of a world's life has no contacts yet, which is one step
/// of a bounce nobody can see.
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
ASYSTEM(FixedUpdate, name = "Bounce") void BounceSystem(SystemContext &ctx);

/// @brief Action names CharacterInputSystem polls.
///
/// Constants rather than string literals at the call site, so a level's
/// `input.actions` table and the code that reads it cannot drift apart by a typo
/// that simply reads as the key doing nothing.
///
/// `string_view`, not `const char *`: the lookups take one, so a pointer would
/// have its length measured again on every call, for every character, every step.
inline constexpr std::string_view kActionMoveForward  = "MoveForward";
inline constexpr std::string_view kActionMoveBackward = "MoveBackward";
inline constexpr std::string_view kActionMoveLeft     = "MoveLeft";
inline constexpr std::string_view kActionMoveRight    = "MoveRight";
inline constexpr std::string_view kActionJump         = "Jump";
inline constexpr std::string_view kActionCrouch       = "Crouch";

/// @brief Turns every Physics::Character's intent into motion.
///
/// Reads the pair (Character, CharacterDescriptor): scales the character's `move`
/// direction by the descriptor's speed for its current stance, asks for the
/// stance it wants, and hands both to the controller.
///
/// Publishing the result is CharacterStateSystem's job, not this one's — see
/// there for why it cannot be done here.
///
/// **This is the only thing that has to run for a character to move.** Whatever
/// fills the intent — a keyboard, an AI, a replicated command — feeds this one
/// system, which is what lets a player and an NPC share a controller.
///
/// @par Requirements
/// Register it in **FixedUpdate**, which puts it immediately before its world's
/// physics step: the intent it writes is what that step simulates, with no frame
/// of wasted motion in between.
///
/// @par Behaviour worth knowing
/// - The stance is asked for every step, not on the edge of a keypress. Standing
///   up under something low fails silently and is retried, so a character stands
///   by itself once it walks clear.
/// - `Character::jump` is cleared once consumed, so a single press is one jump
///   however many steps pass before it can fire.
ASYSTEM(FixedUpdate, name = "CharacterMove") void CharacterMoveSystem(SystemContext &ctx);

/// @brief Publishes each Physics::Character's post-step state onto its component.
///
/// **PostFixedUpdate, and that is the whole point.** Ordering with
/// `after`/`before` only arranges systems *within* a phase, and every FixedUpdate
/// system runs before its world's physics step — so a refresh there could only
/// ever publish the step before the one it is about to cause. This phase is on
/// the far side of the step, so what it publishes is what just happened.
///
/// Per tick rather than per frame, so a frame that runs three fixed steps
/// publishes three times. PostUpdate would publish only the last of them, which
/// is enough for rendering and wrong for anything that must see every tick.
///
/// Reading `Character::state` is a convenience: PhysicsWorld::GetCharacterState
/// is live and answers correctly whenever it is called. The component copy exists
/// so an animation graph or a gameplay system can read the character's footing
/// without reaching for the physics world at all.
ASYSTEM(PostFixedUpdate, name = "CharacterState") void CharacterStateSystem(SystemContext &ctx);

/// @brief Drives every Physics::Character in the active world from the keyboard.
///
/// **A stand-in, and deliberately a crude one.** It steers every character in the
/// world at once, because nothing yet says which one the player is — possession
/// is what replaces this, and when it lands this system is what gets deleted. It
/// exists so a level can be walked around before that.
///
/// A server writes `Character::move` from replicated input commands instead of
/// polling devices, which is why the intent lives on the component rather than
/// being read from a device inside the controller: the two paths meet at the same
/// three fields.
///
/// Movement is in world axes — forward is −Z — since there is no camera to be
/// relative to yet.
///
/// @par Requirements
/// **Update, not FixedUpdate.** Devices are sampled once per frame, and a key
/// *press* is an edge that exists for exactly that frame. Read from a fixed step
/// it is both lost and duplicated: a frame fast enough to run no step never sees
/// the press at all, and a frame slow enough to run three sees the same one three
/// times. Once per frame is the only reading that matches how the edge is
/// produced. The controller consumes the intent on every step, which is where the
/// fixed rate belongs.
///
/// `activeWorldOnly`, and it null-checks the input anyway: a headless host has no
/// devices, so this is inert there even when a level names it.
ASYSTEM(Update, name = "CharacterInput", activeWorldOnly)
void CharacterInputSystem(SystemContext &ctx);

/// @brief Degrees of turn per pixel of mouse movement.
///
/// A constant rather than a descriptor field because look sensitivity is a
/// *player's* preference, not a property of a character — it belongs in the
/// options a person sets once, and authoring it per character would make every
/// NPC carry a number that means nothing to it. Here until there is somewhere
/// better to put it.
inline constexpr float kLookDegreesPerPixel = 0.1f;

/// @brief How far up or down a character may look.
///
/// Short of straight up, because at exactly 90 the forward direction becomes
/// parallel to the up axis and the view matrix's basis collapses — the image
/// rolls or vanishes at the moment the player is least able to explain why.
inline constexpr float kMaxPitchDegrees = 89.f;

/// @brief Turns the mouse into where a character is looking, and holds the
/// cursor while it does.
///
/// Yaw goes on the character, pitch on the camera parented to it — the standard
/// split, and the reason the controller writes position only and never rotates
/// the capsule: facing is gameplay's, and this is gameplay.
///
/// The camera is found by looking for a Camera whose Parent is the character, so
/// a character without one simply turns on the spot. Nothing here needs the
/// camera to be the *active* one; a character that is not being viewed still
/// faces where it was told to.
///
/// **A stand-in, like CharacterInputSystem, and it goes when possession lands.**
///
/// Looks only while the cursor is already captured, and never captures it itself.
/// Taking it would mean deciding when the player may have it back, which is the
/// host's business — an editor hands it over on Play and returns it on Escape or
/// F8, and a game build would simply hold it. A system that grabbed the cursor
/// every step would undo any release a step after it happened.
///
/// @par Requirements
/// **Update, before CharacterInput** — per frame, for the same reason as that
/// system and more so: `MouseDelta` is the movement accumulated since the last
/// poll, which is a whole frame's worth. Consuming it from a fixed step applies
/// one frame of movement once, twice or not at all depending on how many steps
/// the accumulator happens to run, so the same flick of the wrist turns a
/// different amount at 144 Hz than at 60. Once per frame turns exactly once.
///
/// Sensitivity is degrees per *pixel* and is deliberately not scaled by dt: the
/// delta is already a distance moved, not a rate, and multiplying it by a
/// timestep would make a fast frame turn less for the same hand movement.
///
/// `activeWorldOnly`, and it null-checks the input regardless: a headless host
/// has no mouse, and a server reads look from replicated commands instead.
ASYSTEM(Update, name = "CharacterLook", before = "CharacterInput", activeWorldOnly)
void CharacterLookSystem(SystemContext &ctx);

} // namespace Assisi::App
