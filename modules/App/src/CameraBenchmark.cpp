/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/CameraBenchmark.hpp>

#include <algorithm>
#include <utility>

namespace Assisi::App
{

CameraBenchmark::CameraBenchmark(std::vector<Runtime::CameraRouteLeg> route, double runSeconds,
                                 std::int32_t shotCount)
    : _route(std::move(route)), _runSeconds(runSeconds), _shotCount(std::max(shotCount, 0))
{
    _routeScale = static_cast<float>(static_cast<double>(Runtime::CameraRouteSeconds(_route)) / _runSeconds);
}

BenchmarkPhase CameraBenchmark::Advance(bool assetsLoaded, double nowSeconds)
{
    switch (_phase)
    {
    case BenchmarkPhase::Settling:
        if (assetsLoaded)
        {
            _phase = BenchmarkPhase::WarmingUp;
        }
        break;
    case BenchmarkPhase::WarmingUp:
        --_warmupFramesLeft;
        if (_warmupFramesLeft <= 0)
        {
            _phase = BenchmarkPhase::Running;
            _startSeconds = nowSeconds;
        }
        break;
    case BenchmarkPhase::Running:
        if (_shotCount > 0)
        {
            ++_shotFrame;
            if (_shotFrame >= _shotCount * kBenchmarkShotSettleFrames)
            {
                _phase = BenchmarkPhase::Finished;
            }
            break;
        }
        _elapsed = std::min(nowSeconds - _startSeconds, _runSeconds);
        if (_elapsed >= _runSeconds)
        {
            _phase = BenchmarkPhase::Finished;
        }
        break;
    case BenchmarkPhase::Finished:
    case BenchmarkPhase::Count:
        break;
    }
    return _phase;
}

Runtime::CameraAim CameraBenchmark::Aim() const
{
    double seconds = _elapsed;
    if (_shotCount > 0)
    {
        // Evenly spaced with both ends included; a single shot is the middle.
        const std::int32_t shot = std::min(_shotFrame / kBenchmarkShotSettleFrames, _shotCount - 1);
        const double fraction =
            _shotCount > 1 ? static_cast<double>(shot) / static_cast<double>(_shotCount - 1) : 0.5;
        seconds = fraction * _runSeconds;
    }
    return Runtime::EvaluateCameraRoute(_route, static_cast<float>(seconds) * _routeScale);
}

std::int32_t CameraBenchmark::ShotThisFrame() const
{
    if (_shotCount == 0 || _phase != BenchmarkPhase::Running)
    {
        return -1;
    }
    const bool lastSettleFrame = _shotFrame % kBenchmarkShotSettleFrames == kBenchmarkShotSettleFrames - 1;
    return lastSettleFrame ? _shotFrame / kBenchmarkShotSettleFrames : -1;
}

} // namespace Assisi::App
