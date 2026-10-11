/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file NetClock.hpp
/// @brief Client-side clock: which server tick to stamp commands with, and how
/// fast to run so they keep arriving just in time.
///
/// A client's input for tick N has to *arrive before the server simulates tick
/// N*, or the server has nothing to apply and the player's input is lost. So the
/// client runs ahead of the server, by the trip its commands make plus a small
/// cushion of commands the server holds in hand.
///
/// The server measures that cushion where the commands are used: how many of
/// this client's commands it holds, each snapshot. The clock steers it by
/// running the client's fixed steps a little faster or slower — at most
/// `maxDilation` either way — so the cushion fills or drains a tick at a time.
///
/// The tick it stamps commands with never goes back. Going back would stamp new
/// commands with ticks the server already holds commands for, and it drops them
/// as copies. It jumps forward only when every command it could send is already
/// too late: at first contact, and when the server has run on past it.

#include <cstdint>

namespace Assisi::NetSync
{

/// @brief What a server reports to a client each snapshot, for clock steering.
struct ClockFeedback
{
    /// The tick the server had simulated when it built this snapshot.
    std::uint64_t serverTick = 0;

    /// How many of this client's commands were buffered ahead of that tick.
    /// Zero means the server had nothing to apply — input was dropped.
    std::uint32_t inputBufferDepth = 0;

    /// Ticks since the last report on which this client's queue was empty.
    /// Distinguishes "shallow but keeping up" from "actually missing input".
    std::uint32_t starvedTicks = 0;
};

/// @brief Tuning for the client's clock.
struct NetClockConfig
{
    /// The most the clock runs faster or slower than real time, as a fraction:
    /// 0.1 is between 0.9 and 1.1 times. Small enough that a player does not
    /// see their own game speed change while the cushion settles.
    double maxDilation = 0.1;

    /// How much faster or slower per tick the cushion is off by. A cushion two
    /// ticks off runs five percent off; four or more runs at the limit.
    double dilationPerErrorTick = 0.025;

    /// Commands the server should have in hand beyond the one it is applying.
    /// One is the minimum that survives a single late packet; two is the usual
    /// comfortable setting on a jittery connection.
    std::uint32_t targetBufferDepth = 2;

    /// How far the cushion may be off, in ticks, and the clock still run at
    /// rate one. The depth a snapshot sees moves by a tick with where in a step
    /// it was measured; steering on that would never settle.
    std::uint32_t deadZoneTicks = 1;

    /// Ceiling on the lead the clock starts or jumps to, so a pathological RTT
    /// estimate cannot push the client arbitrarily far into the future. At 60 Hz
    /// this is half a second.
    std::uint32_t maxLead = 30;
};

/// @brief Decides the tick a client stamps its commands with, and how fast its
/// fixed steps should run.
///
/// Owned by the client. Fed the server's feedback and the transport's RTT;
/// asked for the tick to stamp outgoing commands with, and for the rate the
/// application runs its fixed steps at.
class NetClock
{
public:
    explicit NetClock(double tickRateHz, NetClockConfig config = {});

    /// @brief Take a snapshot's clock feedback.
    ///
    /// @param roundTripMs The transport's current RTT estimate.
    ///
    /// Only a snapshot newer than the last one counts, so the same one handed
    /// over again, or an older one arriving late, changes nothing. The first
    /// leads by the round trip, a frame and the cushion: the snapshot is already
    /// a trip down old, and a command still has a trip up to make. After that
    /// the queue depth the server saw sets the rate. Feedback about ticks before
    /// the first command the server could have had says nothing about the lead
    /// and is not acted on.
    void OnSnapshot(const ClockFeedback &feedback, std::int32_t roundTripMs);

    /// @brief Advance one local tick. Call once per fixed step.
    void Tick();

    /// @brief The tick to stamp the command sampled this step with. Never less
    /// than it was.
    [[nodiscard]] std::uint64_t CommandTick() const { return _clientTick; }

    /// @brief The server tick the client believes is current — what remote
    /// entity interpolation is anchored to. Between snapshots it advances a tick
    /// per client tick, so while the clock runs fast or slow it drifts from the
    /// server's by up to `maxDilation` of the ticks since the last snapshot.
    [[nodiscard]] std::uint64_t EstimatedServerTick() const;

    /// @brief How far ahead of the server the client was at the last snapshot.
    [[nodiscard]] std::uint32_t Lead() const { return _lead; }

    /// @brief How fast the client's fixed steps should run against real time:
    /// 1 as fast, below 1 slower, above 1 faster. 1 until the first feedback.
    [[nodiscard]] double Rate() const { return _rate; }

    /// @brief How many times the clock has jumped forward after first contact.
    /// A number that keeps climbing in a steady run means the client keeps
    /// falling behind the server — a stalling client, or a connection whose
    /// round trip keeps growing.
    [[nodiscard]] std::uint32_t CorrectionCount() const { return _corrections; }

    /// @brief Whether any feedback has arrived yet. Before it has, the clock is
    /// free-running and CommandTick() is only locally meaningful.
    [[nodiscard]] bool HasFeedback() const { return _hasFeedback; }

    /// @brief Reset to a pre-handshake state, for a rejoin.
    void Reset();

private:
    /// @brief Ticks that fit in @p milliseconds, rounded up — rounding down
    /// would systematically under-lead, and under-leading is the failure that
    /// drops input.
    [[nodiscard]] std::uint32_t MillisecondsToTicks(std::int32_t milliseconds) const;

    /// @brief Stamps commands from @p serverTick plus the round trip, a frame and
    /// the cushion, at rate one.
    void JumpTo(std::uint64_t serverTick, std::uint32_t roundTripTicks);

    /// @brief The rate that steers a queue of @p feedback's depth to the cushion.
    [[nodiscard]] double RateFor(const ClockFeedback &feedback) const;

    NetClockConfig _config;
    double _tickRateHz;
    double _rate = 1.0;

    std::uint64_t _clientTick   = 0;
    std::uint64_t _serverTick   = 0; ///< Last reported, advanced locally between snapshots.
    std::uint64_t _serverTickAt = 0; ///< Client tick at which _serverTick was reported.

    /// The first tick stamped since the last jump. The server can have had no
    /// command for a tick before it, so starving there is not the lead's fault.
    std::uint64_t _firstCommandTick = 0;

    std::uint32_t _lead        = 0;
    std::uint32_t _corrections = 0;
    bool _hasFeedback = false;
};

} // namespace Assisi::NetSync
