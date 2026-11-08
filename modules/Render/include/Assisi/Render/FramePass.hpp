/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file FramePass.hpp
/// @brief What a pass declared with APASS works through: the resources it
///        named, and the recorder that hands it the command list.
///
/// A pass reaches a resource only through the Reads<> or Writes<> its Record
/// takes, and reflectgen orders the passes by those same lists, so what a pass
/// declares and what it touches cannot drift apart. It reaches the command list
/// only through its Recorder, which opens the pass's GPU timer the first time
/// it is asked, so a frame on which a pass has nothing to draw records nothing
/// and shows no timer.

#include <Assisi/Render/GpuMarker.hpp>
#include <Assisi/Render/RenderFrame.hpp>

#include <nvrhi/nvrhi.h>

#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>

namespace Assisi::Render
{

/// @brief Where a pass draws: the scene's framebuffer and its size, which the
/// resources that stand for the scene's colour and depth carry.
struct RenderTarget
{
    nvrhi::IFramebuffer *framebuffer = nullptr;
    nvrhi::ITexture *colorTexture = nullptr;
    nvrhi::ITexture *depthTexture = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

/// @brief The resources a pass reads, and nothing else.
template <typename... Resources>
class Reads
{
public:
    explicit Reads(const Resources &... resources) : _resources(resources...) {}

    /// @brief The resource of type @p T, which must be one this pass listed.
    template <typename T>
    [[nodiscard]] const T &Get() const
    {
        static_assert((std::is_same_v<T, Resources> || ...), "this pass reads only what its Reads<...> lists");
        return std::get<const T &>(_resources);
    }

private:
    std::tuple<const Resources &...> _resources;
};

/// @brief The resources a pass writes, which it may read too, and nothing else.
template <typename... Resources>
class Writes
{
public:
    explicit Writes(Resources &... resources) : _resources(resources...) {}

    /// @brief The resource of type @p T, which must be one this pass listed.
    template <typename T>
    [[nodiscard]] T &Get() const
    {
        static_assert((std::is_same_v<T, Resources> || ...), "this pass writes only what its Writes<...> lists");
        return std::get<T &>(_resources);
    }

private:
    std::tuple<Resources &...> _resources;
};

/// @brief A pass's way to the command list.
///
/// The first time a pass asks for the command list or a frame to draw into,
/// the recorder opens the pass's GPU marker and timer under the pass's first
/// timer name; a pass that never asks records nothing and shows no timer. A
/// pass that declares several timers opens each by name with Begin, which
/// closes the one before.
class Recorder
{
public:
    /// @param timers The pass's declared timers, in order; the first is opened
    ///               by the first CommandList or Frame.
    /// @param eager  Open the first timer now: a base pass shows in every frame
    ///               even when it has nothing to draw.
    Recorder(nvrhi::ICommandList *commandList, std::span<const char *const> timers, bool eager);
    ~Recorder();

    Recorder(const Recorder &) = delete;
    Recorder &operator=(const Recorder &) = delete;
    Recorder(Recorder &&) = delete;
    Recorder &operator=(Recorder &&) = delete;

    /// @brief The command list, with the pass's timer open.
    [[nodiscard]] nvrhi::ICommandList *CommandList();

    /// @brief The frame to draw into @p resource's target, with the pass's
    /// timer open. Only a resource the pass declared carries a target, so a pass
    /// draws only where it said it would.
    template <typename Resource>
    [[nodiscard]] RenderFrame Frame(const Resource &resource)
    {
        return Frame(resource.target);
    }

    /// @brief The frame to draw into @p target, for code a pass hands its
    /// recorder and its declared resource's target to.
    [[nodiscard]] RenderFrame Frame(const RenderTarget &target)
    {
        return RenderFrame{.commandList = CommandList(),
                           .colorTexture = target.colorTexture,
                           .depthTexture = target.depthTexture,
                           .framebuffer = target.framebuffer,
                           .width = target.width,
                           .height = target.height};
    }

    /// @brief Close the open timer and open @p timer, which must be one the
    /// pass declared.
    void Begin(const char *timer);

    /// @brief Whether the pass asked for the command list: whether it recorded.
    [[nodiscard]] bool Recorded() const { return _recorded; }

private:
    void Open(const char *timer);
    void Close();

    std::span<const char *const> _timers;
#if defined(ASSISI_ENABLE_GPU_MARKERS)
    std::optional<GpuMarkerScope> _marker;
#endif
    nvrhi::ICommandList *_commandList = nullptr;
    std::optional<GpuPassTimerScope> _timer;
    bool _recorded = false;
};

/// @brief Whether a pass has a hook of the given name, and whether that hook
/// has the shape the generated walk calls. reflectgen asserts both, so a hook
/// of the right name and the wrong shape fails the build instead of being
/// silently skipped.
template <typename P>
concept HasWanted = requires { &P::Wanted; };
template <typename P, typename... Frames>
concept WantedFits = requires(const P &pass, const Frames &... frames) {
    { pass.Wanted(frames...) }->std::same_as<bool>;
};
template <typename P>
concept HasRelease = requires { &P::Release; };
template <typename P>
concept ReleaseFits = requires(P &pass) {
    { pass.Release() }->std::same_as<void>;
};
template <typename P>
concept HasInitialize = requires { &P::Initialize; };
template <typename P, typename Init>
concept InitializeFits = requires(P &pass, const Init &init) {
    { pass.Initialize(init) }->std::same_as<bool>;
};
template <typename P>
concept HasOnTargetsChanged = requires { &P::OnTargetsChanged; };
template <typename P>
concept OnTargetsChangedFits = requires(P &pass, const nvrhi::FramebufferInfo &targets) {
    { pass.OnTargetsChanged(targets) }->std::same_as<bool>;
};

/// @brief Whether @p pass is on this frame: its Wanted, or always for a pass
/// with none. A per-view pass's Wanted may take the view after the frame, or
/// the frame alone when the view does not decide it.
template <typename P, typename Frame, typename... View>
[[nodiscard]] bool PassWanted(const P &pass, const Frame &frame, const View &... view)
{
    if constexpr (!HasWanted<P>)
    {
        return true;
    }
    else if constexpr (WantedFits<P, Frame, View...>)
    {
        return pass.Wanted(frame, view...);
    }
    else
    {
        return pass.Wanted(frame);
    }
}

/// @brief Free what @p pass holds while it is off, if it holds anything.
template <typename P>
void PassRelease(P &pass)
{
    if constexpr (HasRelease<P>)
    {
        pass.Release();
    }
}

/// @brief @p pass's Initialize, or success for a pass with none.
template <typename P, typename Init>
[[nodiscard]] bool PassInitialize(P &pass, const Init &init)
{
    if constexpr (HasInitialize<P>)
    {
        return pass.Initialize(init);
    }
    else
    {
        return true;
    }
}

/// @brief @p pass's OnTargetsChanged, or success for a pass with none.
template <typename P>
[[nodiscard]] bool PassOnTargetsChanged(P &pass, const nvrhi::FramebufferInfo &targets)
{
    if constexpr (HasOnTargetsChanged<P>)
    {
        return pass.OnTargetsChanged(targets);
    }
    else
    {
        return true;
    }
}

} // namespace Assisi::Render
