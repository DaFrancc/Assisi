/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file NetClock.hpp
/// @brief Client-side clock estimation: which server tick to target, and how
/// far ahead of the server to run.
///
/// The problem it solves: a client's input for tick N has to *arrive before the
/// server simulates tick N*, or the server has nothing to apply and the player
/// sees their input dropped. So the client cannot run at server time — it must
/// run slightly ahead, by roughly the one-way trip plus a frame of slack.
///
/// Two numbers come back from the server to steer this: the tick it stamped its
/// last snapshot with, and how deep that client's input queue was when it got
/// there. Queue depth is the honest signal — it is measured where the commands
/// are actually consumed, and it says directly whether the lead is too small
/// (starvation: input dropped) or too large (needless added latency).
///
/// v1 responds by snapping: when the measured lead drifts far enough from the
/// target, it jumps. Overwatch's smoother answer — scaling the client's clock
/// rate by a few percent so the buffer drains or fills gradually — is deferred,
/// but every input it needs is produced here, so adopting it later changes only
/// the response, not the protocol.

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

/// @brief Tuning for how far ahead a client runs. All in ticks.
struct NetClockConfig
{
    /// Commands the server should have in hand beyond the one it is applying.
    /// One is the minimum that survives a single late packet; two is the usual
    /// comfortable setting on a jittery connection.
    std::uint32_t targetBufferDepth = 2;

    /// How far the estimated lead may drift from the target before the clock
    /// snaps. Correcting every tick would jitter the simulation for no benefit;
    /// tolerating a large drift defeats the point.
    std::uint32_t correctionThreshold = 2;

    /// Ceiling on the lead, so a pathological RTT estimate cannot push the
    /// client arbitrarily far into the future. At 60 Hz this is half a second.
    std::uint32_t maxLead = 30;
};

/// @brief Estimates the tick a client should be simulating.
///
/// Owned by the client. Fed the transport's RTT once per frame and the server's
/// feedback once per snapshot; asked for the tick to stamp outgoing commands
/// with.
class NetClock
{
public:
    explicit NetClock(double tickRateHz, NetClockConfig config = {});

    /// @brief Take a snapshot's clock feedback.
    ///
    /// @param roundTripMs The transport's current RTT estimate.
    ///
    /// The lead is (½ RTT, converted to ticks) + one command frame + the target
    /// buffer depth, then adjusted by what the server actually observed: a
    /// starved queue means the lead is too small regardless of what the RTT
    /// says, and an overfull one means it is larger than this connection needs.
    /// Measurement beats arithmetic — the RTT estimate does not know about
    /// jitter, scheduling, or a client whose frame rate is uneven.
    void OnSnapshot(const ClockFeedback &feedback, std::int32_t roundTripMs);

    /// @brief Advance one local tick. Call once per fixed step.
    void Tick();

    /// @brief The tick to stamp the command sampled this step with.
    [[nodiscard]] std::uint64_t CommandTick() const { return _clientTick; }

    /// @brief The server tick the client believes is current — what remote
    /// entity interpolation is anchored to.
    [[nodiscard]] std::uint64_t EstimatedServerTick() const;

    /// @brief How far ahead of the server the client is currently running.
    [[nodiscard]] std::uint32_t Lead() const { return _lead; }

    /// @brief The lead the clock is aiming for, from the last feedback.
    [[nodiscard]] std::uint32_t TargetLead() const { return _targetLead; }

    /// @brief How many times the clock has snapped rather than tracked
    /// smoothly. A number that keeps climbing in a steady run means the
    /// correction threshold is too tight, or the connection is genuinely
    /// unstable — either way it is the first thing to look at.
    [[nodiscard]] std::uint32_t CorrectionCount() const { return _corrections; }

    /// @brief Whether any feedback has arrived yet. Before it has, the clock is
    /// free-running and CommandTick() is only locally meaningful.
    [[nodiscard]] bool HasFeedback() const { return _hasFeedback; }

    /// @brief Reset to a pre-handshake state (used on reconnect, which in v1 is
    /// a full rejoin).
    void Reset();

private:
    /// @brief Ticks that fit in @p milliseconds, rounded up — rounding down
    /// would systematically under-lead, and under-leading is the failure that
    /// drops input.
    [[nodiscard]] std::uint32_t MillisecondsToTicks(std::int32_t milliseconds) const;

    double _tickRateHz;
    NetClockConfig _config;

    std::uint64_t _clientTick   = 0;
    std::uint64_t _serverTick   = 0; ///< Last reported, advanced locally between snapshots.
    std::uint64_t _serverTickAt = 0; ///< Client tick at which _serverTick was reported.
    std::uint32_t _lead         = 0;
    std::uint32_t _targetLead   = 0;
    std::uint32_t _corrections  = 0;
    bool _hasFeedback  = false;
};

} // namespace Assisi::NetSync
