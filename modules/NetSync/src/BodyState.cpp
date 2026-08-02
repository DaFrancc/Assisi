/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/BodyState.hpp>

#include <cmath>

namespace Assisi::NetSync
{

void WriteBodyState(const BodyState &state, Core::BitWriter &writer)
{
    writer.WriteVarUInt32(state.netId);
    writer.WriteBool(state.asleep);

    writer.WriteFloat(state.position.x);
    writer.WriteFloat(state.position.y);
    writer.WriteFloat(state.position.z);

    writer.WriteFloat(state.rotation.x);
    writer.WriteFloat(state.rotation.y);
    writer.WriteFloat(state.rotation.z);
    writer.WriteFloat(state.rotation.w);

    // A sleeping body has no motion by definition, so six floats of zero would
    // be six floats of nothing. This is where the "settled world costs headers"
    // property actually comes from.
    if (!state.asleep)
    {
        writer.WriteFloat(state.linearVelocity.x);
        writer.WriteFloat(state.linearVelocity.y);
        writer.WriteFloat(state.linearVelocity.z);
        writer.WriteFloat(state.angularVelocity.x);
        writer.WriteFloat(state.angularVelocity.y);
        writer.WriteFloat(state.angularVelocity.z);
    }
}

bool ReadBodyState(Core::BitReader &reader, BodyState &outState)
{
    BodyState state;
    state.netId  = reader.ReadVarUInt32();
    state.asleep = reader.ReadBool();

    state.position.x = reader.ReadFloat();
    state.position.y = reader.ReadFloat();
    state.position.z = reader.ReadFloat();

    state.rotation.x = reader.ReadFloat();
    state.rotation.y = reader.ReadFloat();
    state.rotation.z = reader.ReadFloat();
    state.rotation.w = reader.ReadFloat();

    if (!state.asleep)
    {
        state.linearVelocity.x  = reader.ReadFloat();
        state.linearVelocity.y  = reader.ReadFloat();
        state.linearVelocity.z  = reader.ReadFloat();
        state.angularVelocity.x = reader.ReadFloat();
        state.angularVelocity.y = reader.ReadFloat();
        state.angularVelocity.z = reader.ReadFloat();
    }

    if (!reader.Ok() || state.netId == InvalidNetId)
    {
        reader.Invalidate();
        return false;
    }

    // These bytes are about to be handed to a physics engine that asserts on a
    // non-normalized quaternion and integrates NaN into every body it touches.
    // A bit-flip the transport did not catch is the realistic source, and "the
    // simulation quietly filled with NaN" is not a failure anyone traces back.
    const float lengthSq = state.rotation.x * state.rotation.x + state.rotation.y * state.rotation.y +
                           state.rotation.z * state.rotation.z + state.rotation.w * state.rotation.w;
    const bool finite = std::isfinite(state.position.x) && std::isfinite(state.position.y) &&
                        std::isfinite(state.position.z) && std::isfinite(lengthSq) &&
                        std::isfinite(state.linearVelocity.x) && std::isfinite(state.linearVelocity.y) &&
                        std::isfinite(state.linearVelocity.z) && std::isfinite(state.angularVelocity.x) &&
                        std::isfinite(state.angularVelocity.y) && std::isfinite(state.angularVelocity.z);
    if (!finite || lengthSq < 0.5f || lengthSq > 2.0f)
    {
        reader.Invalidate();
        return false;
    }

    outState = state;
    return true;
}

} // namespace Assisi::NetSync
