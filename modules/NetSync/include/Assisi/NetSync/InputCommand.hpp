/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file InputCommand.hpp
/// @brief The per-tick input frame a client sends and a server consumes.
///
/// This is protocol, not windowing, which is why it lives here and not in
/// Window: it is sampled *from* an ActionMap on the client, but the server that
/// applies it has no ActionMap, no keyboard, and no window. Gameplay systems
/// that control a player read an InputCommand; only editor and debug code polls
/// devices directly.
///
/// A command targets a specific `tick` — the fixed-step counter the server also
/// stamps snapshots with (Application::GetSimTick). That is what makes late or
/// out-of-order delivery recoverable: the server knows which step a command
/// belonged to instead of applying whatever arrived most recently.

#include <Assisi/Core/BitStream.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace Assisi::NetSync
{

/// @brief One tick's worth of player input.
///
/// Kept deliberately small: it is sent redundantly (see InputCommandBuffer),
/// so every byte here is paid for several times over.
struct InputCommand
{
    /// The fixed-step tick this input is meant to be applied on.
    std::uint64_t tick = 0;

    /// Pressed state of the bound digital actions, one bit each. The mapping
    /// from bit index to action name is part of the protocol hash, not of this
    /// struct — the server never learns the names.
    std::uint32_t actionBits = 0;

    /// Analog movement, in the local controller frame, each component nominally
    /// in [-1, 1]. Floats in v1; quantization is a later field-encoder change
    /// that does not move this struct.
    float moveX = 0.f;
    float moveY = 0.f;

    /// Look *delta* for this tick, in radians. A delta rather than an absolute
    /// orientation so the server can bound how far a client may turn in one
    /// tick — an absolute value gives it nothing to validate against.
    float lookYaw   = 0.f;
    float lookPitch = 0.f;

    /// @brief When within the tick the input actually occurred, quantized to
    /// 0-255 across the step.
    ///
    /// Reserved from day one and ignored by v1 servers. CS2-style sub-tick
    /// evaluation — integrating movement from the fractional moment rather than
    /// the tick boundary — is a purely server-side refinement later, and having
    /// the field already on the wire means it costs no format break when it
    /// happens. (The lesson from CS2's own rollout is that partial sub-tick,
    /// where hits are precise but visuals stay tick-quantized, feels *worse*
    /// than none: do it end to end or not at all.)
    std::uint8_t subTickFraction = 0;
};

/// @brief Serialize @p command. Must stay in exact correspondence with Read().
void WriteInputCommand(const InputCommand &command, Core::BitWriter &writer);

/// @brief Deserialize one command. On a truncated or malformed buffer the
/// reader latches its failure state and the returned value is meaningless —
/// check `reader.Ok()`, never the value.
[[nodiscard]] InputCommand ReadInputCommand(Core::BitReader &reader);

/// @brief Bounds a server applies to an untrusted command before acting on it.
///
/// Server authority is necessary but not sufficient: a server that faithfully
/// applies whatever InputCommand arrives is speed-hack food, because the client
/// chooses the numbers. These are the physical maximums of the controller,
/// which the server — not the client — owns.
struct InputLimits
{
    float maxMoveMagnitude = 1.f;    ///< Clamped, not rejected: a stick can legitimately saturate.
    float maxLookYaw       = 3.2f;   ///< Radians per tick. Slightly over π: a legal fast flick.
    float maxLookPitch     = 3.2f;
};

/// @brief Clamp @p command in place to @p limits.
/// @return false if anything had to be clamped — a signal worth logging or rate
/// limiting on, since well-behaved clients never trip it.
bool ClampInputCommand(InputCommand &command, const InputLimits &limits);

/// @brief A client's ring of recently-sampled commands, and the redundancy
/// policy for sending them.
///
/// Input goes out unreliably: a retransmit would arrive after the tick it was
/// for had already been simulated, which is worse than useless. Loss is covered
/// instead by *redundancy* — every packet repeats the last N commands, so a
/// single dropped packet costs nothing as long as the next one arrives. This is
/// the pattern Overwatch documented and the Quake lineage has used throughout.
class InputCommandBuffer
{
  public:
    /// @param redundancy How many recent commands each packet repeats. Three is
    ///   the usual starting point: it survives two consecutive losses and costs
    ///   well under a hundred bytes.
    explicit InputCommandBuffer(std::size_t redundancy = 3);

    /// @brief Record this tick's sampled input. Older commands past the
    /// redundancy window are dropped.
    void Push(const InputCommand &command);

    /// @brief Serialize the pending window: a count, then the commands oldest
    /// first. Writes a zero count (and nothing else) when empty, so the receiver
    /// has one shape to parse.
    void WritePacket(Core::BitWriter &writer) const;

    /// @brief Inverse of WritePacket. Appends to @p out. Returns false on a
    /// malformed or truncated buffer, having appended nothing usable — the
    /// caller should drop the packet, not partially apply it.
    static bool ReadPacket(Core::BitReader &reader, std::vector<InputCommand> &out);

    [[nodiscard]] std::size_t Size() const { return _commands.size(); }
    [[nodiscard]] std::size_t Redundancy() const { return _redundancy; }
    [[nodiscard]] bool        Empty() const { return _commands.empty(); }
    void                      Clear() { _commands.clear(); }

    /// @brief The most recent command, or nullptr if none has been pushed.
    [[nodiscard]] const InputCommand *Latest() const;

  private:
    std::deque<InputCommand> _commands;
    std::size_t              _redundancy;
};

/// @brief The server's per-connection queue of commands waiting to be applied.
///
/// Its job is to absorb jitter: commands arrive in bursts and gaps, but the
/// simulation consumes exactly one per tick. Duplicates (from the redundant
/// send above) and commands for ticks already simulated are discarded here, so
/// the simulation never sees the same input twice.
class InputCommandQueue
{
  public:
    /// @brief Offer a received command. Ignored if it duplicates one already
    /// queued or targets a tick at or before the last one applied.
    /// @return true if it was accepted into the queue.
    bool Accept(const InputCommand &command);

    /// @brief Take the command for @p tick, or nullptr if it has not arrived.
    ///
    /// Also drops everything older, so a command that showed up too late to be
    /// useful does not sit in the queue forever. A null return is *starvation*
    /// — see Depth() and the clock's response to it.
    const InputCommand *Consume(std::uint64_t tick);

    /// @brief How many commands are buffered ahead of the last applied tick.
    ///
    /// This is the number the server reports back to the client in snapshot
    /// headers: too small and the client is running behind (starvation, visible
    /// as dropped input), too large and it is running further ahead than the
    /// connection requires (needless added latency).
    [[nodiscard]] std::size_t Depth() const { return _commands.size(); }

    /// @brief The last tick actually handed to the simulation.
    [[nodiscard]] std::uint64_t LastAppliedTick() const { return _lastAppliedTick; }

    /// @brief How many ticks since construction found no command waiting. The
    /// starvation signal the adaptive clock will act on.
    [[nodiscard]] std::uint64_t StarvedTicks() const { return _starvedTicks; }

    void Clear();

  private:
    std::deque<InputCommand> _commands; ///< Sorted ascending by tick.
    std::uint64_t            _lastAppliedTick = 0;
    bool                     _hasApplied      = false;
    std::uint64_t            _starvedTicks    = 0;

    /// The most recent command handed out, kept alive so Consume() can return a
    /// pointer that outlives the queue entry it came from.
    InputCommand _lastApplied{};
};

} // namespace Assisi::NetSync
