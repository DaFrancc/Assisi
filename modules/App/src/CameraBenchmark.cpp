/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/CameraBenchmark.hpp>

#include <algorithm>
#include <utility>

namespace Assisi::App
{

CameraBenchmark::CameraBenchmark(std::vector<Runtime::CameraRouteLeg> route, double runSeconds)
    : _route(std::move(route)), _runSeconds(runSeconds)
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
    return Runtime::EvaluateCameraRoute(_route, static_cast<float>(_elapsed) * _routeScale);
}

} // namespace Assisi::App
