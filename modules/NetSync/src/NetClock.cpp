/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/NetClock.hpp>

#include <algorithm>
#include <cmath>

namespace Assisi::NetSync
{

NetClock::NetClock(double tickRateHz, NetClockConfig config)
    : _tickRateHz(tickRateHz > 0.0 ? tickRateHz : 60.0), _config(config)
{
}

std::uint32_t NetClock::MillisecondsToTicks(std::int32_t milliseconds) const
{
    if (milliseconds <= 0)
        return 0;
    const double ticks = (static_cast<double>(milliseconds) / 1000.0) * _tickRateHz;
    return static_cast<std::uint32_t>(std::ceil(ticks));
}

void NetClock::OnSnapshot(const ClockFeedback &feedback, std::int32_t roundTripMs)
{
    // Half the round trip is the one-way trip a command still has to make, plus
    // one tick for the frame it is sampled in, plus the cushion we want the
    // server to be holding when it gets there.
    const std::uint32_t oneWayTicks = MillisecondsToTicks(roundTripMs) / 2u + (roundTripMs > 0 ? 1u : 0u);
    std::uint32_t target      = oneWayTicks + 1u + _config.targetBufferDepth;

    if (_hasFeedback)
    {
        // Correct the arithmetic with what actually happened. Starvation is the
        // asymmetric case: a lead that is too short loses input outright, while
        // one that is too long only costs latency, so grow decisively and shrink
        // one tick at a time.
        if (feedback.starvedTicks > 0 || feedback.inputBufferDepth == 0)
        {
            target = std::max(target, _targetLead + 1u + feedback.starvedTicks);
        }
        else if (feedback.inputBufferDepth > _config.targetBufferDepth)
        {
            const std::uint32_t excess = feedback.inputBufferDepth - _config.targetBufferDepth;
            target                     = target > excess ? target - 1u : target;
        }
    }

    _targetLead = std::clamp(target, 1u, _config.maxLead);

    _serverTick = feedback.serverTick;

    // Where the client actually is relative to the server, as opposed to where
    // it intended to be.
    const std::uint32_t measuredLead =
        _clientTick > feedback.serverTick
            ? static_cast<std::uint32_t>(std::min<std::uint64_t>(_clientTick - feedback.serverTick, _config.maxLead))
            : 0u;

    const std::uint32_t drift = measuredLead > _targetLead ? measuredLead - _targetLead : _targetLead - measuredLead;

    if (!_hasFeedback)
    {
        // First contact: adopt the target outright. There is no history to be
        // smooth about, and starting at whatever the free-running counter
        // happened to reach would be arbitrary.
        _clientTick  = feedback.serverTick + _targetLead;
        _lead        = _targetLead;
        _hasFeedback = true;
        _serverTickAt = _clientTick;
        return;
    }

    if (drift >= _config.correctionThreshold)
    {
        _clientTick = feedback.serverTick + _targetLead;
        _lead       = _targetLead;
        ++_corrections;
    }
    else
    {
        _lead = measuredLead;
    }

    // Recorded last, after any snap: EstimatedServerTick() extrapolates from
    // (_serverTick, _serverTickAt) as a pair, so capturing the client tick
    // before a correction moved it would make the extrapolation count the jump
    // as elapsed time.
    _serverTickAt = _clientTick;
}

void NetClock::Tick()
{
    ++_clientTick;
    // The server is advancing too, at the same rate; between snapshots the best
    // estimate of its tick is simply "the last one it told us, plus however many
    // ticks we have run since". EstimatedServerTick() derives that rather than
    // storing it, so a snapshot always overwrites an estimate instead of
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
    _clientTick   = 0;
    _serverTick   = 0;
    _serverTickAt = 0;
    _lead         = 0;
    _targetLead   = 0;
    _corrections  = 0;
    _hasFeedback  = false;
}

} // namespace Assisi::NetSync
