/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CameraBenchmark.hpp
/// @brief Flies the camera along a level's route for a fixed time, so every
///        run renders the same views.
///
/// The phases, in order:
///
/// - **Settling** until every asset the world references has loaded. Streaming
///   work is not rendering work, and a route that set off while meshes were
///   still arriving would measure different scenes at the same moment of two
///   runs.
/// - **Warming up** for a fixed number of frames at the route's first pose.
///   Pipelines compile and the first uploads land here, where nothing is
///   measured.
/// - **Running** for the requested length, flying the whole route in that time.
/// - **Finished**, holding the last pose.
///
/// The clock is passed in rather than read, so the phases can be tested without
/// waiting for them.

#include <Assisi/Runtime/CameraPath.hpp>

#include <cstdint>
#include <vector>

namespace Assisi::App
{

/// @brief Frames drawn at the first pose before the route starts.
inline constexpr std::int32_t kBenchmarkWarmupFrames = 120;

/// @brief How long a benchmark runs when nothing asks otherwise, in seconds.
inline constexpr double kDefaultBenchmarkSeconds = 15.0;

/// @brief Frames a shot holds its pose before its picture is taken, so work a
/// new view starts, such as a cascade refit or a shadow bake spread over a few
/// frames, has finished by the frame that is kept.
inline constexpr std::int32_t kBenchmarkShotSettleFrames = 8;

/// @brief Where a benchmark is. See the file comment.
enum class BenchmarkPhase : std::uint8_t
{
    Settling,
    WarmingUp,
    Running,
    Finished,
    Count
};

/// @brief Drives one benchmark run. See the file comment.
///
/// With shots, the run does not fly the route in time. It stops at @p shotCount
/// evenly spaced points along it instead, first and last included, and holds
/// each for kBenchmarkShotSettleFrames. The poses are exact, so two builds'
/// pictures of the same shot are of the same view, which a timed flight cannot
/// promise.
class CameraBenchmark
{
public:
    /// @p route must not be empty, and @p runSeconds must be positive: the route
    /// is scaled to fit that time whatever its authored length. A @p shotCount of
    /// zero flies the route in time.
    CameraBenchmark(std::vector<Runtime::CameraRouteLeg> route, double runSeconds, std::int32_t shotCount = 0);

    /// @brief Moves on by one frame.
    ///
    /// @p assetsLoaded is whether the world has finished loading; @p nowSeconds
    /// is a monotonic clock. Returns the phase for this frame.
    BenchmarkPhase Advance(bool assetsLoaded, double nowSeconds);

    /// @brief The pose for the current frame.
    [[nodiscard]] Runtime::CameraAim Aim() const;

    [[nodiscard]] BenchmarkPhase Phase() const { return _phase; }

    /// @brief Seconds into the run, clamped to its length. 0 before it starts.
    [[nodiscard]] double ElapsedSeconds() const { return _elapsed; }

    [[nodiscard]] double RunSeconds() const { return _runSeconds; }

    /// @brief The shot whose picture this frame is, or -1 when this frame's is
    /// not wanted.
    [[nodiscard]] std::int32_t ShotThisFrame() const;

private:
    std::vector<Runtime::CameraRouteLeg> _route;
    double _runSeconds = 0.0;
    double _startSeconds = 0.0;
    double _elapsed = 0.0;

    /// Authored route seconds per second of run.
    float _routeScale = 0.f;
    std::int32_t _warmupFramesLeft = kBenchmarkWarmupFrames;
    std::int32_t _shotCount = 0;

    /// Frames spent in the running phase of a shots run.
    std::int32_t _shotFrame = 0;
    BenchmarkPhase _phase = BenchmarkPhase::Settling;
};

} // namespace Assisi::App
