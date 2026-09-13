/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file GpuMarker.hpp
/// @brief Debug-utils labels on the command list, named identically to the
///        Chiara scope that encloses them.
///
/// ```cpp
/// ASSISI_PROFILE_GPU_SCOPE(frame.commandList, "scene");
/// ```
///
/// `ASSISI_PROFILE_GPU_SCOPE` emits the Chiara scope and the GPU label from one
/// name, so a slice in Perfetto and a labelled range in RenderDoc are guaranteed
/// to mean the same thing; two macros with two string literals would drift.
///
/// **Only wrap code that records commands.** A marker around CPU-only work
/// (transform propagation, cull-table building) produces an empty range that
/// reads, wrongly, as a pass that cost nothing. Use plain ASSISI_PROFILE_SCOPE
/// there.
///
/// **Never span the command list's open or close.** `beginMarker`/`endMarker`
/// record *into* the list, so a marker that opens before BeginFrame() or closes
/// after EndFrame() has submitted is recorded into a list that no longer
/// accepts commands. That is why `begin-frame` and `end-frame` keep plain CPU
/// scopes.
///
/// Gated on ASSISI_ENABLE_GPU_MARKERS, which defaults to ASSISI_ENABLE_CHIARA
/// but stays its own option: a plain Release build handed to RenderDoc still
/// wants labels, and a Chiara capture on a machine with no GPU tooling does not.

#include <Assisi/Chiara/Profile.hpp>

#if defined(ASSISI_ENABLE_GPU_MARKERS)
#    include <nvrhi/nvrhi.h>

namespace Assisi::Render
{

/// @brief Opens a debug-utils label on construction, closes it on destruction.
/// A null command list is a no-op, so call sites need no guard.
class GpuMarkerScope
{
public:
    GpuMarkerScope(nvrhi::ICommandList *commandList, const char *name) : _commandList(commandList)
    {
        if (_commandList != nullptr)
        {
            _commandList->beginMarker(name);
        }
    }

    ~GpuMarkerScope()
    {
        if (_commandList != nullptr)
        {
            _commandList->endMarker();
        }
    }

    GpuMarkerScope(const GpuMarkerScope &)            = delete;
    GpuMarkerScope &operator=(const GpuMarkerScope &) = delete;
    GpuMarkerScope(GpuMarkerScope &&)                 = delete;
    GpuMarkerScope &operator=(GpuMarkerScope &&)      = delete;

private:
    nvrhi::ICommandList *_commandList = nullptr;
};

} // namespace Assisi::Render

/// @brief Labels the enclosing block on the GPU timeline only.
#    define ASSISI_GPU_MARKER(commandList, name)                                                                \
        const ::Assisi::Render::GpuMarkerScope ASSISI_CHIARA_UNIQUE(gpuMarker_)                                 \
        {                                                                                                       \
            (commandList), (name)                                                                               \
        }

#else // !ASSISI_ENABLE_GPU_MARKERS

#    define ASSISI_GPU_MARKER(commandList, name) ((void)sizeof(commandList), (void)sizeof(name))

#endif // ASSISI_ENABLE_GPU_MARKERS

/// @brief Times the enclosing block on the CPU (Chiara) and labels it on the GPU
/// (debug-utils), from a single name. The preferred form at any site that
/// records commands.
///
/// Note what this does **not** measure: Chiara times the CPU spent recording the
/// commands, and the marker is a label for RenderDoc/Nsight. Neither is GPU
/// execution time. For a pass whose cost is on the GPU, add
/// ASSISI_PROFILE_GPU_PASS instead.
#define ASSISI_PROFILE_GPU_SCOPE(commandList, name)                                                             \
        ASSISI_PROFILE_SCOPE(name);                                                                                 \
        ASSISI_GPU_MARKER((commandList), (name))

namespace Assisi::Render
{

/// @brief Opens a GPU timer around a pass on construction, closes it on
/// destruction. Inert unless per-pass timing has been switched on.
///
/// Separate from GpuMarkerScope because the two answer different questions and
/// have different costs. A marker is free and always on; a timer forces a
/// render-pass break (nvrhi's begin/endTimerQuery each call endRenderPass), so
/// it is opt-in — see VulkanContext's per-pass timing section.
///
/// Resolves the context itself rather than taking one, so a pass site that only
/// has a command list needs no new plumbing.
class GpuPassTimerScope
{
public:
    explicit GpuPassTimerScope(const char *name);
    ~GpuPassTimerScope();

    GpuPassTimerScope(const GpuPassTimerScope &)            = delete;
    GpuPassTimerScope &operator=(const GpuPassTimerScope &) = delete;
    GpuPassTimerScope(GpuPassTimerScope &&)                 = delete;
    GpuPassTimerScope &operator=(GpuPassTimerScope &&)      = delete;

private:
    bool _opened = false;
};

} // namespace Assisi::Render

/// @brief Everything ASSISI_PROFILE_GPU_SCOPE does, plus a GPU **timer** when
/// per-pass timing is enabled. Use at the pass boundaries whose GPU cost the
/// measurement ledger quotes; use the plain scope everywhere else.
///
/// Must not nest — a timer spanning another timer measures the render-pass break
/// between them as much as the work. The inner one is dropped with a warning.
#define ASSISI_PROFILE_GPU_PASS(commandList, name)                                                              \
        ASSISI_PROFILE_GPU_SCOPE((commandList), (name));                                                            \
        const ::Assisi::Render::GpuPassTimerScope ASSISI_CHIARA_UNIQUE(gpuPassTimer_) { (name) }
