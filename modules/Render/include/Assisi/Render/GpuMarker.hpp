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
/// This is the bridge between the two halves of a frame. Chiara knows what the
/// engine was *doing* but cannot see the GPU; RenderDoc and Nsight see the GPU
/// perfectly but only in Vulkan's vocabulary — 350 anonymous draws where the
/// engine sees eight passes. `ASSISI_PROFILE_GPU_SCOPE` emits both from one
/// name, so a slice in Perfetto and a labelled range in RenderDoc are
/// guaranteed to mean the same thing. Using two macros with two string literals
/// would let them drift, which is the whole failure mode this avoids.
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
/// Gated on ASSISI_ENABLE_GPU_MARKERS, which defaults to following
/// ASSISI_ENABLE_CHIARA — the `-chiara` presets are the builds anyone profiles.
/// It is a separate option because the two are genuinely independent: a plain
/// Release build handed to RenderDoc still wants labels, and a Chiara capture on
/// a machine with no GPU tooling has no use for them.

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
#define ASSISI_PROFILE_GPU_SCOPE(commandList, name)                                                             \
        ASSISI_PROFILE_SCOPE(name);                                                                                 \
        ASSISI_GPU_MARKER((commandList), (name))
