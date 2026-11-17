/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/FramePass.hpp>

#include <Assisi/Core/Assert.hpp>

#include <algorithm>
#include <cstring>

namespace Assisi::Render
{

Recorder::Recorder(nvrhi::ICommandList *commandList, std::span<const char *const> timers, RecorderTiming timing)
    : _timers(timers), _commandList(commandList), _untimed(timing == RecorderTiming::Untimed)
{
    if (timing != RecorderTiming::Lazy)
    {
        Open(_timers.front());
    }
}

Recorder::~Recorder()
{
    Close();
}

nvrhi::ICommandList *Recorder::CommandList()
{
    if (!_recorded)
    {
        Open(_timers.front());
    }
    return _commandList;
}

void Recorder::Begin(const char *timer)
{
    ASSISI_ASSERT(std::ranges::any_of(_timers, [timer](const char *declared) { return std::strcmp(declared, timer) == 0; }),
                  "a pass opens only the timers its APASS declares");
    Close();
    Open(timer);
}

void Recorder::Open(const char *timer)
{
    // The marker before the timer and closed after it, as the profiling macros
    // nest them, so a capture shows the timer inside its label.
#if defined(ASSISI_ENABLE_GPU_MARKERS)
    if (_commandList != nullptr)
    {
        _commandList->beginMarker(timer);
        _markerOpen = true;
    }
#endif
    if (!_untimed)
    {
        _timer.emplace(timer);
    }
    _recorded = true;
}

void Recorder::Close()
{
    _timer.reset();
    if (_markerOpen)
    {
        _commandList->endMarker();
        _markerOpen = false;
    }
}

} // namespace Assisi::Render
