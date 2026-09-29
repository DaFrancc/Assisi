/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CountingRenderer.hpp
/// @brief A renderer that plays one clip once and records what the device asked of it.

#include <Assisi/Audio/AudioFormat.hpp>
#include <Assisi/Audio/AudioRenderer.hpp>
#include <Assisi/Audio/Clip.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>

namespace Assisi::Audio::Testing
{

class CountingRenderer final : public AudioRenderer
{
public:
    explicit CountingRenderer(const PcmClip &clip) : _clip(clip) {}

    void Render(std::span<float> interleaved) noexcept override
    {
        if (interleaved.size() % kChannelCount != 0)
        {
            _partialFrameSpans.fetch_add(1, std::memory_order_relaxed);
        }

        const std::uint64_t cursor  = _cursor.load(std::memory_order_relaxed);
        const std::size_t start     = static_cast<std::size_t>(cursor) * kChannelCount;
        const std::size_t remaining = start < _clip.samples.size() ? _clip.samples.size() - start : 0;
        const std::size_t copied    = std::min(remaining, interleaved.size());
        std::copy_n(_clip.samples.begin() + static_cast<std::ptrdiff_t>(start), copied, interleaved.begin());
        std::fill(interleaved.begin() + static_cast<std::ptrdiff_t>(copied), interleaved.end(), 0.0f);

        _cursor.store(cursor + FrameCount(copied), std::memory_order_relaxed);
        _renderThread = std::this_thread::get_id();
        _framesRendered.fetch_add(FrameCount(interleaved.size()), std::memory_order_release);
    }

    /// @brief Every frame the device has asked for, clip or silence.
    [[nodiscard]] std::uint64_t FramesRendered() const noexcept
    {
        return _framesRendered.load(std::memory_order_acquire);
    }

    /// @brief How far into the clip playback has reached.
    [[nodiscard]] std::uint64_t ClipCursor() const noexcept { return _cursor.load(std::memory_order_relaxed); }

    [[nodiscard]] std::uint32_t PartialFrameSpans() const noexcept
    {
        return _partialFrameSpans.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::thread::id RenderThread() const noexcept { return _renderThread; }

private:
    const PcmClip &_clip;
    std::atomic<std::uint64_t> _framesRendered{0};
    std::atomic<std::uint64_t> _cursor{0};
    /// Read only after the device has stopped, which orders it after every write.
    std::thread::id _renderThread;
    std::atomic<std::uint32_t> _partialFrameSpans{0};
};

/// @brief Long enough that a loaded machine running the null backend in real
/// time never trips it, short enough that a callback that never runs fails fast.
inline constexpr std::chrono::milliseconds kPlaybackTimeout{5000};

/// @brief How often a test checks on the audio thread's progress.
inline constexpr std::chrono::milliseconds kPollInterval{5};

/// @brief Wait until @p renderer has rendered at least @p frames, or the timeout.
inline bool WaitForFrames(const CountingRenderer &renderer, std::uint64_t frames)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kPlaybackTimeout;
    while (renderer.FramesRendered() < frames)
    {
        if (std::chrono::steady_clock::now() > deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    return true;
}

} // namespace Assisi::Audio::Testing
