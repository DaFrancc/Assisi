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

namespace Assisi::NetSync
{

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
/// awake — the velocities.
///
/// Whole-value floats. The quantized encodings are a bandwidth knob, not a
/// correctness one (round-to-nearest cannot accumulate across corrections), so
/// they are chosen against measured divergence rather than guessed at now; R8
/// swaps the field encoders without touching this structure.
void WriteBodyState(const BodyState &state, Core::BitWriter &writer);

/// @brief Inverse of WriteBodyState. Returns false on a truncated or malformed
/// record, having invalidated the reader.
bool ReadBodyState(Core::BitReader &reader, BodyState &outState);

} // namespace Assisi::NetSync
