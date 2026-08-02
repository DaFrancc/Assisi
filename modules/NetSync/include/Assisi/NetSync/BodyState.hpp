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
/// stopped talking about this body" safe. See docs/replication-plan-v4.md §3.3.

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
/// The defaults are the published state-synchronization shape
/// (https://gafferongames.com/post/snapshot_compression/) fitted to a level-sized
/// world: ~2 mm of position resolution over a ±256 m box, a smallest-three
/// quaternion at 2+9+9+9 bits, and velocities at ~16 mm/s. Gaffer's own caution
/// applies — state-sync extrapolation wants *finer* position quantization than
/// snapshot interpolation does — so these are a starting point to be moved
/// against measured divergence, not a settled answer.
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

/// @brief The parameters this process encodes and decodes body state with.
[[nodiscard]] const BodyQuantization &Quantization();

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

/// @brief One body's authoritative state, as it crosses the wire.
struct BodyState
{
    NetId     netId = InvalidNetId;
    glm::vec3 position{};
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 linearVelocity{};  ///< Omitted on the wire when asleep.
    glm::vec3 angularVelocity{}; ///< Ditto.
    bool      asleep = false;
};

/// @brief Write one body record: id, the at-rest bit, pose, and — only when
/// awake — the velocities, all at the resolutions Quantization() names.
void WriteBodyState(const BodyState &state, Core::BitWriter &writer);

/// @brief Inverse of WriteBodyState. Returns false on a truncated or malformed
/// record, having invalidated the reader.
bool ReadBodyState(Core::BitReader &reader, BodyState &outState);

} // namespace Assisi::NetSync
