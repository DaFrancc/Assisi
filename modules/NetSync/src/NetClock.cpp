/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/NetClock.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Assisi::NetSync
{

NetClock::NetClock(double tickRateHz, NetClockConfig config)
    : _config(config), _tickRateHz(tickRateHz > 0.0 ? tickRateHz : 60.0)
{
}

std::uint32_t NetClock::MillisecondsToTicks(std::int32_t milliseconds) const
{
    if (milliseconds <= 0)
        return 0;
    const double ticks = (static_cast<double>(milliseconds) / 1000.0) * _tickRateHz;
    return static_cast<std::uint32_t>(std::ceil(ticks));
}

void NetClock::JumpTo(std::uint64_t serverTick, std::uint32_t roundTripTicks)
{
    const std::uint32_t lead = std::min(roundTripTicks + 1u + _config.targetBufferDepth, _config.maxLead);
    _clientTick       = serverTick + lead;
    _firstCommandTick = _clientTick;
    _lead             = lead;
    _rate             = 1.0;
}

double NetClock::RateFor(const ClockFeedback &feedback) const
{
    // Positive: the server holds more than the cushion, so slow down and let it
    // drain. An empty queue that also starved is further behind than its depth
    // says, by up to the cushion again.
    std::int32_t error = static_cast<std::int32_t>(feedback.inputBufferDepth) -
                         static_cast<std::int32_t>(_config.targetBufferDepth);
    if (feedback.inputBufferDepth == 0)
    {
        error -= static_cast<std::int32_t>(std::min(feedback.starvedTicks, _config.targetBufferDepth));
    }

    if (std::abs(error) <= static_cast<std::int32_t>(_config.deadZoneTicks))
    {
        return 1.0;
    }
    const double change = std::clamp(static_cast<double>(error) * _config.dilationPerErrorTick,
                                     -_config.maxDilation, _config.maxDilation);
    return 1.0 - change;
}

void NetClock::OnSnapshot(const ClockFeedback &feedback, std::int32_t roundTripMs)
{
    // The client hands over the last snapshot every tick, new or not, and a late
    // one can arrive after a newer one: only news counts.
    if (_hasFeedback && feedback.serverTick <= _serverTick)
    {
        return;
    }

    const std::uint32_t roundTripTicks = MillisecondsToTicks(roundTripMs);
    if (!_hasFeedback)
    {
        JumpTo(feedback.serverTick, roundTripTicks);
        _hasFeedback = true;
    }
    // Every command it could send now arrives after the server has run its
    // tick: no rate catches that up, only a jump.
    else if (_clientTick <= feedback.serverTick + roundTripTicks)
    {
        JumpTo(feedback.serverTick, roundTripTicks);
        ++_corrections;
    }
    else
    {
        _lead = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(_clientTick - feedback.serverTick, _config.maxLead));
        // Before the first command stamped since the last jump, the server could
        // have had nothing: its starving there is the jump's doing, not the lead's.
        if (feedback.serverTick >= _firstCommandTick)
        {
            _rate = RateFor(feedback);
        }
    }

    // Recorded last: EstimatedServerTick() extrapolates from (_serverTick,
    // _serverTickAt) as a pair, so the client tick must be the one after any
    // jump.
    _serverTick   = feedback.serverTick;
    _serverTickAt = _clientTick;
}

void NetClock::Tick()
{
    ++_clientTick;
    // The server is advancing too, at about the same rate; between snapshots the
    // best estimate of its tick is simply "the last one it told us, plus however
    // many ticks we have run since". EstimatedServerTick() derives that rather
    // than storing it, so a snapshot always overwrites an estimate instead of
    // compounding with it.
}

std::uint64_t NetClock::EstimatedServerTick() const
{
    if (!_hasFeedback)
        return 0;
    return _serverTick + (_clientTick - _serverTickAt);
}

void NetClock::Reset()
{
    _clientTick       = 0;
    _serverTick       = 0;
    _serverTickAt     = 0;
    _firstCommandTick = 0;
    _lead             = 0;
    _rate             = 1.0;
    _corrections      = 0;
    _hasFeedback      = false;
}

} // namespace Assisi::NetSync
