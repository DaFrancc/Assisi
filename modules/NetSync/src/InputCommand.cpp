/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/InputCommand.hpp>

#include <algorithm>
#include <cmath>

namespace Assisi::NetSync
{
namespace
{

/// Upper bound on how many commands one packet may claim to contain. A
/// corrupted or hostile length field is otherwise a request to allocate
/// whatever the attacker typed. Generous enough that no honest redundancy
/// setting comes near it.
constexpr std::uint32_t kMaxCommandsPerPacket = 32;

/// How far ahead of the last applied tick the server will hold commands. A
/// client legitimately runs ahead by about half its RTT plus a frame; anything
/// beyond this is either a badly desynced clock or an attempt to flood the
/// queue, and in both cases the excess is worthless by the time it is due.
constexpr std::uint64_t kMaxQueueLookahead = 64;

} // namespace

void WriteInputCommand(const InputCommand &command, Core::BitWriter &writer)
{
    // Varint on the tick: it starts small and grows, so a fixed 64 bits would
    // spend eight bytes per command on a number that is usually two.
    writer.WriteVarUInt64(command.tick);
    writer.WriteUInt32(command.actionBits);
    writer.WriteFloat(command.moveX);
    writer.WriteFloat(command.moveY);
    writer.WriteFloat(command.lookYaw);
    writer.WriteFloat(command.lookPitch);
    writer.WriteBits(command.subTickFraction, 8);
}

InputCommand ReadInputCommand(Core::BitReader &reader)
{
    InputCommand command;
    command.tick            = reader.ReadVarUInt64();
    command.actionBits      = reader.ReadUInt32();
    command.moveX           = reader.ReadFloat();
    command.moveY           = reader.ReadFloat();
    command.lookYaw         = reader.ReadFloat();
    command.lookPitch       = reader.ReadFloat();
    command.subTickFraction = static_cast<std::uint8_t>(reader.ReadBits(8));
    return command;
}

bool ClampInputCommand(InputCommand &command, const InputLimits &limits)
{
    bool clean = true;

    // NaN is not merely out of range — every comparison against it is false, so
    // it would slip past a naive clamp and then poison whatever it is added to.
    // Treat it as the absence of input.
    const auto sanitize = [&clean](float &value)
    {
        if (!std::isfinite(value))
        {
            value = 0.f;
            clean = false;
        }
    };
    sanitize(command.moveX);
    sanitize(command.moveY);
    sanitize(command.lookYaw);
    sanitize(command.lookPitch);

    // Clamp movement by *magnitude*, not per axis: clamping x and y separately
    // would let a diagonal through at sqrt(2) times the intended speed, which is
    // the oldest speed hack there is.
    const float magnitude = std::sqrt(command.moveX * command.moveX + command.moveY * command.moveY);
    if (magnitude > limits.maxMoveMagnitude && magnitude > 0.f)
    {
        const float scale = limits.maxMoveMagnitude / magnitude;
        command.moveX *= scale;
        command.moveY *= scale;
        clean = false;
    }

    const auto clampAbs = [&clean](float &value, float limit)
    {
        const float clamped = std::clamp(value, -limit, limit);
        if (clamped != value)
        {
            value = clamped;
            clean = false;
        }
    };
    clampAbs(command.lookYaw, limits.maxLookYaw);
    clampAbs(command.lookPitch, limits.maxLookPitch);

    return clean;
}

// ---------------------------------------------------------------------------

InputCommandBuffer::InputCommandBuffer(std::size_t redundancy)
    : _redundancy(std::max<std::size_t>(redundancy, 1))
{
}

void InputCommandBuffer::Push(const InputCommand &command)
{
    _commands.push_back(command);
    while (_commands.size() > _redundancy)
        _commands.pop_front();
}

const InputCommand *InputCommandBuffer::Latest() const
{
    return _commands.empty() ? nullptr : &_commands.back();
}

void InputCommandBuffer::WritePacket(Core::BitWriter &writer) const
{
    writer.WriteVarUInt32(static_cast<std::uint32_t>(_commands.size()));
    for (const InputCommand &command : _commands)
        WriteInputCommand(command, writer);
}

bool InputCommandBuffer::ReadPacket(Core::BitReader &reader, std::vector<InputCommand> &out)
{
    const std::uint32_t count = reader.ReadVarUInt32();
    if (!reader.Ok() || count > kMaxCommandsPerPacket)
    {
        // Latch the failure even when the count itself decoded fine: an absurd
        // count is a protocol error, and letting the reader stay "Ok" would
        // invite the caller to keep parsing whatever follows.
        reader.Invalidate();
        return false;
    }

    // Decode into a scratch buffer first: a truncated packet must leave the
    // caller's vector exactly as it found it, not half-filled with commands
    // from a message that turned out to be garbage.
    std::vector<InputCommand> decoded;
    decoded.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const InputCommand command = ReadInputCommand(reader);
        if (!reader.Ok())
            return false;
        decoded.push_back(command);
    }

    out.insert(out.end(), decoded.begin(), decoded.end());
    return true;
}

// ---------------------------------------------------------------------------

bool InputCommandQueue::Accept(const InputCommand &command)
{
    // Already simulated: the redundant send means we see most commands two or
    // three times, and every repeat after the first is noise.
    if (_hasApplied && command.tick <= _lastAppliedTick)
        return false;

    const std::uint64_t horizon = _hasApplied ? _lastAppliedTick : command.tick;
    if (command.tick > horizon + kMaxQueueLookahead)
        return false;

    // Sorted insert. The queue is a handful of entries deep by construction, so
    // a linear scan is both the simplest and the fastest thing here.
    const auto position = std::lower_bound(_commands.begin(), _commands.end(), command.tick,
                                           [](const InputCommand &entry, std::uint64_t tick)
                                           { return entry.tick < tick; });
    if (position != _commands.end() && position->tick == command.tick)
        return false; // duplicate

    _commands.insert(position, command);
    return true;
}

const InputCommand *InputCommandQueue::Consume(std::uint64_t tick)
{
    // Anything older than the tick being simulated arrived too late to matter;
    // dropping it here is what keeps a stalled connection from accumulating a
    // queue it will never work through.
    while (!_commands.empty() && _commands.front().tick < tick)
        _commands.pop_front();

    if (_commands.empty() || _commands.front().tick != tick)
    {
        ++_starvedTicks;
        return nullptr;
    }

    // Copied out before popping: the caller gets a pointer that stays valid for
    // the rest of the tick, which a pointer into the deque would not.
    _lastApplied = _commands.front();
    _commands.pop_front();
    _lastAppliedTick = tick;
    _hasApplied      = true;
    return &_lastApplied;
}

void InputCommandQueue::Clear()
{
    _commands.clear();
    _lastAppliedTick = 0;
    _hasApplied      = false;
    _starvedTicks    = 0;
}

} // namespace Assisi::NetSync
