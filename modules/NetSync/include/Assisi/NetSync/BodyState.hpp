/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file BodyState.hpp
/// @brief The authoritative motion state of one simulated body, and its wire
/// format.
///
/// Under local simulation both machines run the same physics and the wire's job
/// is to *re-anchor* the client, not to stream it a movie. What a re-anchor has
/// to carry is settled by what the client does next: it extrapolates. A
/// correction that places a body but not its motion re-diverges within a frame,
/// so the velocities are part of the state — with an at-rest bit making the
/// common case (a settled world) cheap, since a sleeping body has no velocities
/// worth sending and the bit says so in one bit rather than six floats.
///
/// The sleep bit is *replicated state*, not a hint. It is sent on the transition
/// and, like anything else, resent until acked — which is what makes "the server
/// stopped talking about this body" safe.

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/NetSync/NetProtocol.hpp>

#include <cstdint>
#include <string_view>

namespace Assisi::NetSync
{

/// @brief How body state is packed onto the wire.
///
/// Bandwidth knob, not a correctness one. Round-to-nearest quantization does not
/// accumulate across corrections — each re-anchor lands within half a quantum,
/// independently of the last — so precision here is a display-quality decision.
/// (`BitStream` rounds to nearest for exactly this reason.)
///
/// **Both ends must agree**, which is why it is inside `NetProtocolHash`: two
/// builds quantizing the same position over different ranges corrupt each other
/// *silently*, which is the one failure mode a handshake exists to prevent.
///
/// The defaults are the published state-synchronization shape fitted to a
/// level-sized world: ~2 mm of position resolution over a ±256 m box, a
/// smallest-three quaternion at 2+9+9+9 bits, and linear velocity at ~62 mm/s.
/// The usual caution applies — state-sync extrapolation wants *finer* position
/// quantization than snapshot interpolation does — so these are a starting point
/// to be moved against measured divergence, not a settled answer.
struct BodyQuantization
{
    /// Symmetric world half-extent, in metres. A body outside it is clamped to
    /// the boundary rather than wrapped: a clamped correction is visibly wrong
    /// in the right direction, where a wrapped one teleports across the map.
    float positionExtent = 256.f;
    /// Bits per position component. 18 over a 512 m span is ~1.95 mm.
    std::uint32_t positionBits = 18;

    /// Symmetric bound on a linear velocity component, m/s.
    float linearVelocityMax = 128.f;
    /// Bits per linear velocity component. 12 over ±128 is ~62 mm/s.
    ///
    /// Deliberately coarser than the position. Velocity is only used to
    /// extrapolate until the next correction, so its error is bounded by one
    /// correction interval: 62 mm/s over 50 ms is 3 mm of drift, under the
    /// position quantum it would be corrected against anyway.
    std::uint32_t linearVelocityBits = 12;

    /// Symmetric bound on an angular velocity component, rad/s.
    float angularVelocityMax = 64.f;
    /// Bits per angular velocity component. 11 over ±64 is ~62 mrad/s, and the
    /// same reasoning as the linear one applies.
    std::uint32_t angularVelocityBits = 11;

    bool operator==(const BodyQuantization &) const = default;
};

/// @brief How a client hides the jump when a correction snaps its simulation.
///
/// Purely view-side, and deliberately **not** part of the protocol hash: two
/// machines that smooth differently still agree about every byte and every
/// simulated pose. Only the pictures differ, and the picture is a local choice.
///
/// **Convergence is bounded in time, not by a per-frame rate**, and that is a
/// correction to what shipped first. The published state-synchronization
/// constants (0.95 per frame under 25 cm, 0.85 over 1 m) assume corrections are
/// *occasional*. They are not always: any source of divergence that reappears
/// every interval — a contact-rich pile, a bouncing body, a gameplay rule whose
/// contact instant lands a physics step apart on the two machines — injects a
/// fresh error every 50 ms while 0.95/frame removes only ~14% of the old one in
/// that time. The offset converges on several times the per-correction error
/// instead of on zero, and the body renders steadily behind a simulation that is
/// perfectly correct underneath. Measured on a bouncing body here: ~0.4 m of
/// visual lag from a mean divergence of ~0.01 m.
///
/// Every shipped system solves this the same way — converge within a fixed
/// wall-clock window, restarted by each correction:
///   - Unreal's CharacterMovementComponent smooths the mesh over
///     `NetworkSimulatedSmoothLocationTime` = 0.1 s (0.033 s for rotation);
///   - Source smooths prediction error over `cl_smoothtime`, 0.1 s in TF2;
///   - Unreal's Chaos render interpolation uses a fixed
///     `ErrorCorrectionDuration`, and its predictive-interpolation physics
///     replication converges over roughly one send interval.
/// A deadline at or under the correction interval means the previous offset is
/// paid off before the next one lands, so steady-state lag collapses from
/// several times the per-correction error to about one of them.
///
/// The decay is also exponential *in dt* rather than per frame, so the feel does
/// not change between a 30 Hz and a 144 Hz display — a per-frame factor is a
/// completely different time constant at each (Driscoll, "Frame Rate Independent
/// Damping Using Lerp").
struct ViewSmoothing
{
    /// Seconds to converge a small position error. Tracks Unreal's
    /// CharacterMovementComponent and Source's `cl_smoothtime`.
    float positionCorrectionTime = 0.1f;
    /// ...and a large one, blended between across the two distances below. The
    /// time-domain form of the published 0.95/0.85 two-rate split, but **equal to
    /// the slow window by default**: shortening it is how you turn a large
    /// correction back into the pop this exists to hide (a 1 m error over a
    /// 0.05 s window moves a third of a metre in one 60 Hz frame), and anything
    /// genuinely too far to smooth is caught by `hardSnapDistance` instead. Left
    /// separately configurable because it costs nothing to leave the lever there.
    float positionCorrectionTimeFast = 0.1f;
    float smallErrorDistance         = 0.25f; ///< At or below this, take the slower time.
    float largeErrorDistance         = 1.0f;  ///< At or above this, the faster one.

    /// Seconds to converge an orientation error. Shorter than position, as
    /// Unreal's 0.033 s rotation smoothing is.
    float rotationCorrectionTime = 0.05f;

    /// Below this much divergence, drop the offset instead of smoothing it: the
    /// correction is too small to see, so hiding it buys nothing and only delays
    /// convergence. (Unity's Netcode for Entities does the same, smoothing only
    /// inside a band and snapping below it.)
    float snapBelowDistance = 0.02f;

    /// Past this, drop the offset and let the correction show. Smoothing a
    /// teleport reads worse than admitting it.
    float hardSnapDistance = 2.5f;
};

/// @brief The parameters this process encodes and decodes body state with.
[[nodiscard]] const BodyQuantization &Quantization();

/// @brief The view-side smoothing this process applies to corrections.
[[nodiscard]] const ViewSmoothing &Smoothing();

/// @brief Replace them. Safe at any time — nothing on the wire depends on it.
void SetSmoothing(const ViewSmoothing &smoothing);

/// @brief Replace them. Process-global and set once at startup, before any
/// session exists — changing it mid-session would mean the two ends disagreeing
/// about bytes already in flight, and the hash they agreed on at handshake would
/// no longer describe either of them.
void SetQuantization(const BodyQuantization &quantization);

/// @brief Apply the `networking` block of a game config, if it has one.
///
/// Absent keys keep their defaults and a malformed block warns and changes
/// nothing — a config typo should not be able to make this build refuse to pair
/// with every other one without saying why. Call once at startup.
///
/// Per-game rather than per-level, deliberately: a level-header form would be
/// more precise (a small arena wants finer position quantization than an open
/// world) and is more machinery, so it waits until a level actually needs it.
void LoadQuantizationFromConfig(std::string_view configPath = "game.json");

/// @brief Apply the `smoothing` block of a game config, if it has one. Same
/// absent-keys-keep-defaults, malformed-block-warns contract as above.
///
/// Separate from the quantization loader because the two are separate kinds of
/// thing: quantization is protocol and both ends must agree on it, smoothing is
/// presentation and nobody else can tell.
void LoadSmoothingFromConfig(std::string_view configPath = "game.json");

/// @brief One body's authoritative state, as it crosses the wire.
struct BodyState
{
    NetId netId = InvalidNetId;
    glm::vec3 position{};
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 linearVelocity{};  ///< Omitted on the wire when asleep.
    glm::vec3 angularVelocity{}; ///< Ditto.
    bool asleep = false;
};

/// @brief Write one body record: id, the at-rest bit, pose, and — only when
/// awake — the velocities, all at the resolutions Quantization() names.
void WriteBodyState(const BodyState &state, Core::BitWriter &writer);

/// @brief Inverse of WriteBodyState. Returns false on a truncated or malformed
/// record, having invalidated the reader.
bool ReadBodyState(Core::BitReader &reader, BodyState &outState);

} // namespace Assisi::NetSync
