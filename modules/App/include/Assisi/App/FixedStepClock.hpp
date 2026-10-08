/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file FixedStepClock.hpp
/// @brief How a frame's time becomes a whole number of fixed steps: the frame
///        time snapped to the display's refresh, and the accumulator that turns
///        it into steps and a render blend.

#include <cstdint>

namespace Assisi::App
{

/// @brief Snaps a measured frame time to a whole number of refresh intervals.
///
/// A presented frame lasts a whole number of refreshes, but the time measured
/// for it wobbles around that by a fraction of a millisecond. Fed raw to the
/// accumulator, the wobble moves the blend every frame and, whenever the
/// accumulator sits near a step boundary, turns a run of one-step frames into
/// a zero followed by a two. Snapped, a 60 fps game on 60 Hz physics runs one
/// step every frame.
///
/// What snapping removes is kept as drift, so the game clock still tracks real
/// time. Drift inside a dead zone is held; only the part beyond it is handed
/// back, added to the frame that pushed it there. Wobble around a steady
/// refresh never leaves the dead zone, so it is never handed back at all, and
/// a refresh that differs slightly from the one reported (59.94 Hz reported as
/// 60) is paid back a sliver at a time, always in the same direction.
class FrameTimeSnapper
{
public:
    /// @brief How far a frame's time may sit from a whole number of intervals
    /// and still be snapped to it.
    ///
    /// Wider than the wobble a steady refresh shows, narrower than half the
    /// shortest interval snapped, so a frame time is never near two whole
    /// numbers at once. A frame further off than this is a hitch, and is
    /// real time.
    static constexpr double SnapToleranceSeconds = 0.001;

    /// @brief How much drift is held rather than paid back.
    ///
    /// At least twice the tolerance, so the wobble of a steady refresh, which
    /// sums to no more than one frame's error either way, never reaches it.
    /// Below one fixed step at any physics rate a game uses, so the game clock
    /// stays within a step of real time.
    static constexpr double DriftDeadZoneSeconds = 0.002;

    /// @brief The time the accumulator is given for a frame measured at
    /// @p measuredSeconds, against a refresh interval of @p intervalSeconds.
    ///
    /// An interval of zero or less means none is known, and the measured time
    /// is returned as it is. So is an interval too short to snap against, at
    /// most twice the tolerance, where the wobble is as large as the interval.
    [[nodiscard]] double Snap(double measuredSeconds, double intervalSeconds);

    /// @brief The real time snapping has removed and not yet paid back. Within
    /// the dead zone either way.
    [[nodiscard]] double DriftSeconds() const { return _driftSeconds; }

private:
    double _driftSeconds = 0.0;
};

/// @brief The fixed-step accumulator: time in, whole steps out, and what is
/// left over as the blend between the last two steps.
class FixedStepClock
{
public:
    /// @param stepSeconds  One fixed step. Positive.
    /// @param maxSteps     The most steps one Advance returns.
    FixedStepClock(double stepSeconds, uint32_t maxSteps);

    /// @brief Adds @p seconds and returns how many steps to run for it.
    ///
    /// Past @p maxSteps the remaining whole steps are dropped rather than
    /// carried: carried, they would be owed again next frame, which would run
    /// the cap again, and the game would never get back to one step a frame.
    [[nodiscard]] uint32_t Advance(double seconds);

    /// @brief How far into the next step the clock is, in [0, 1).
    [[nodiscard]] double Alpha() const { return _accumulatorSeconds / _stepSeconds; }

private:
    double _stepSeconds;
    double _accumulatorSeconds = 0.0;
    uint32_t _maxSteps;
};

} // namespace Assisi::App
