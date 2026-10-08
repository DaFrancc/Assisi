/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/FixedStepClock.hpp>

#include <algorithm>
#include <cmath>

namespace Assisi::App
{

double FrameTimeSnapper::Snap(double measuredSeconds, double intervalSeconds)
{
    if (intervalSeconds <= 2.0 * SnapToleranceSeconds)
    {
        return measuredSeconds;
    }

    const double whole = std::round(measuredSeconds / intervalSeconds);
    const double snapped = whole * intervalSeconds;
    if (whole < 1.0 || std::abs(measuredSeconds - snapped) > SnapToleranceSeconds)
    {
        return measuredSeconds;
    }

    // Only the drift beyond the dead zone goes back to the clock, and it goes
    // back with the frame that pushed it there. Its sign is the sign of the
    // drift, so a steady mismatch moves the accumulator one way, a sliver a
    // frame, and crosses a step boundary once rather than flickering over it.
    _driftSeconds += measuredSeconds - snapped;
    const double held = std::clamp(_driftSeconds, -DriftDeadZoneSeconds, DriftDeadZoneSeconds);
    const double repaid = _driftSeconds - held;
    _driftSeconds = held;
    return snapped + repaid;
}

FixedStepClock::FixedStepClock(double stepSeconds, uint32_t maxSteps) : _stepSeconds(stepSeconds), _maxSteps(maxSteps)
{
}

uint32_t FixedStepClock::Advance(double seconds)
{
    _accumulatorSeconds += seconds;
    uint32_t steps = 0;
    while (_accumulatorSeconds >= _stepSeconds && steps < _maxSteps)
    {
        _accumulatorSeconds -= _stepSeconds;
        ++steps;
    }
    if (_accumulatorSeconds >= _stepSeconds)
    {
        _accumulatorSeconds = std::fmod(_accumulatorSeconds, _stepSeconds);
    }
    return steps;
}

} // namespace Assisi::App
