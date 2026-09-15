/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/BodyState.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Assisi::NetSync
{
namespace
{

/// Process-global, set once at startup. A pointer-free plain object rather than
/// a per-session copy because it is inside the protocol hash: two ends that
/// disagree refuse to pair, so "which session is this" never comes up.
BodyQuantization gQuantization;
ViewSmoothing gSmoothing;

/// Bits for the smallest-three quaternion: two to say which component was
/// dropped, then nine each for the rest.
constexpr std::uint32_t kQuatIndexBits     = 2;
constexpr std::uint32_t kQuatComponentBits = 9;

/// The remaining three components of a unit quaternion are bounded by
/// 1/sqrt(2) once the largest is the one dropped — which is the whole reason
/// smallest-three is cheaper than three raw floats at the same precision.
constexpr float kQuatComponentBound = 0.7071068f;

/// Smallest-three: drop the largest-magnitude component and send the other
/// three, flipping the sign of the whole quaternion first so the dropped one is
/// known to be positive (q and -q are the same rotation, so this is free).
void WriteQuaternion(const glm::quat &rotation, Core::BitWriter &writer)
{
    const std::array<float, 4> components{rotation.x, rotation.y, rotation.z, rotation.w};

    std::uint32_t largest = 0;
    for (std::uint32_t i = 1; i < 4; ++i)
    {
        if (std::abs(components[i]) > std::abs(components[largest]))
            largest = i;
    }

    const float sign = components[largest] < 0.f ? -1.f : 1.f;

    writer.WriteBits(largest, kQuatIndexBits);
    for (std::uint32_t i = 0; i < 4; ++i)
    {
        if (i == largest)
            continue;
        writer.WriteFloatQuantized(components[i] * sign, -kQuatComponentBound, kQuatComponentBound,
                                   kQuatComponentBits);
    }
}

glm::quat ReadQuaternion(Core::BitReader &reader)
{
    const std::uint32_t largest = reader.ReadBits(kQuatIndexBits);

    std::array<float, 4> components{};
    float sumOfSquares = 0.f;
    for (std::uint32_t i = 0; i < 4; ++i)
    {
        if (i == largest)
            continue;
        components[i] = reader.ReadFloatQuantized(-kQuatComponentBound, kQuatComponentBound, kQuatComponentBits);
        sumOfSquares += components[i] * components[i];
    }

    // Reconstructed from the unit-length constraint, which is what makes the
    // dropped component free rather than merely cheap. Clamped because the three
    // survivors have each been rounded, and their squares can sum a hair past 1.
    components[largest] = std::sqrt(std::max(0.f, 1.f - sumOfSquares));

    return glm::normalize(glm::quat{components[3], components[0], components[1], components[2]});
}

} // namespace

const BodyQuantization &Quantization() { return gQuantization; }

void SetQuantization(const BodyQuantization &quantization) { gQuantization = quantization; }

const ViewSmoothing &Smoothing() { return gSmoothing; }

void SetSmoothing(const ViewSmoothing &smoothing) { gSmoothing = smoothing; }

void WriteBodyState(const BodyState &state, Core::BitWriter &writer)
{
    const BodyQuantization &q = gQuantization;

    writer.WriteVarId(state.netId); // wire write
    writer.WriteBool(state.asleep);

    writer.WriteFloatQuantized(state.position.x, -q.positionExtent, q.positionExtent, q.positionBits);
    writer.WriteFloatQuantized(state.position.y, -q.positionExtent, q.positionExtent, q.positionBits);
    writer.WriteFloatQuantized(state.position.z, -q.positionExtent, q.positionExtent, q.positionBits);

    WriteQuaternion(state.rotation, writer);

    // A sleeping body has no motion by definition, so six values of zero would be
    // six values of nothing. This is where the "settled world costs headers"
    // property actually comes from.
    if (!state.asleep)
    {
        writer.WriteFloatQuantized(state.linearVelocity.x, -q.linearVelocityMax, q.linearVelocityMax,
                                   q.linearVelocityBits);
        writer.WriteFloatQuantized(state.linearVelocity.y, -q.linearVelocityMax, q.linearVelocityMax,
                                   q.linearVelocityBits);
        writer.WriteFloatQuantized(state.linearVelocity.z, -q.linearVelocityMax, q.linearVelocityMax,
                                   q.linearVelocityBits);
        writer.WriteFloatQuantized(state.angularVelocity.x, -q.angularVelocityMax, q.angularVelocityMax,
                                   q.angularVelocityBits);
        writer.WriteFloatQuantized(state.angularVelocity.y, -q.angularVelocityMax, q.angularVelocityMax,
                                   q.angularVelocityBits);
        writer.WriteFloatQuantized(state.angularVelocity.z, -q.angularVelocityMax, q.angularVelocityMax,
                                   q.angularVelocityBits);
    }
}

bool ReadBodyState(Core::BitReader &reader, BodyState &outState)
{
    const BodyQuantization &q = gQuantization;

    BodyState state;
    state.netId  = reader.ReadVarId<NetId>(); // wire read
    state.asleep = reader.ReadBool();

    state.position.x = reader.ReadFloatQuantized(-q.positionExtent, q.positionExtent, q.positionBits);
    state.position.y = reader.ReadFloatQuantized(-q.positionExtent, q.positionExtent, q.positionBits);
    state.position.z = reader.ReadFloatQuantized(-q.positionExtent, q.positionExtent, q.positionBits);

    state.rotation = ReadQuaternion(reader);

    if (!state.asleep)
    {
        state.linearVelocity.x =
            reader.ReadFloatQuantized(-q.linearVelocityMax, q.linearVelocityMax, q.linearVelocityBits);
        state.linearVelocity.y =
            reader.ReadFloatQuantized(-q.linearVelocityMax, q.linearVelocityMax, q.linearVelocityBits);
        state.linearVelocity.z =
            reader.ReadFloatQuantized(-q.linearVelocityMax, q.linearVelocityMax, q.linearVelocityBits);
        state.angularVelocity.x =
            reader.ReadFloatQuantized(-q.angularVelocityMax, q.angularVelocityMax, q.angularVelocityBits);
        state.angularVelocity.y =
            reader.ReadFloatQuantized(-q.angularVelocityMax, q.angularVelocityMax, q.angularVelocityBits);
        state.angularVelocity.z =
            reader.ReadFloatQuantized(-q.angularVelocityMax, q.angularVelocityMax, q.angularVelocityBits);
    }

    if (!reader.Ok() || state.netId == InvalidNetId)
    {
        reader.Invalidate();
        return false;
    }

    // These values are about to be handed to a physics engine that asserts on a
    // non-normalized quaternion and integrates NaN into every body it touches.
    // Quantization bounds the magnitudes by construction, so what is left to
    // catch is a bit-flip the transport did not — and "the simulation quietly
    // filled with NaN" is not a failure anyone traces back.
    const float lengthSq = state.rotation.x * state.rotation.x + state.rotation.y * state.rotation.y +
                           state.rotation.z * state.rotation.z + state.rotation.w * state.rotation.w;
    if (!std::isfinite(lengthSq) || lengthSq < 0.5f || lengthSq > 2.0f)
    {
        reader.Invalidate();
        return false;
    }

    outState = state;
    return true;
}

} // namespace Assisi::NetSync
