/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file VulkanContext.hpp
/// @brief Owns the Vulkan instance/device/swapchain and wraps them in an NVRHI device.
///
/// NVRHI does not create the window surface, physical device, logical device, or
/// swapchain — those are plain Vulkan API calls the application is responsible for.
/// This class owns that bring-up and hands NVRHI an already-created
/// VkInstance/VkPhysicalDevice/VkDevice.

#include <vulkan/vulkan.h>

#include <nvrhi/nvrhi.h>
#include <nvrhi/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <Assisi/Chiara/Chiara.hpp>
#include <Assisi/Render/RenderFrame.hpp>

namespace Assisi::Window
{
class WindowContext;
}

namespace Assisi::Render::Vulkan
{

/// @brief Owns the Vulkan instance/device/swapchain and the NVRHI device wrapping them.
///
/// Move-only; there is only ever meant to be one of these per window. Construct via
/// Create(), which returns nullptr on any failure (logging the reason).
class VulkanContext
{
public:
    /// @brief Creates the Vulkan instance, device, and swapchain for the given window.
    /// @return nullptr on failure.
    static std::unique_ptr<VulkanContext> Create(const Assisi::Window::WindowContext &window);

    ~VulkanContext();

    VulkanContext(const VulkanContext &) = delete;
    VulkanContext &operator=(const VulkanContext &) = delete;
    VulkanContext(VulkanContext &&) = delete;
    VulkanContext &operator=(VulkanContext &&) = delete;

    /// @brief Acquires the next swapchain image and opens a command list for the frame.
    /// @return std::nullopt if acquisition failed (e.g. window minimized to zero size).
    std::optional<RenderFrame> BeginFrame();

    /// @brief Closes and submits the command list opened by BeginFrame(), then presents.
    void EndFrame();

    /// @brief Recreates the swapchain (and its dependent NVRHI textures/framebuffers)
    /// at the new framebuffer size. Waits for the device to go idle first.
    void Resize(uint32_t width, uint32_t height);

    /// @brief Selects vsync (FIFO) or no-vsync (IMMEDIATE) presentation, recreating
    /// the swapchain at its current extent. No-op if already in the requested state.
    /// If the device doesn't support IMMEDIATE, no-vsync falls back to FIFO (a warning
    /// is logged) — so IsVSyncEnabled() may report true even after SetVSync(false).
    /// Waits for the device to go idle first.
    void SetVSync(bool enabled);

    /// @brief Whether the swapchain is currently presenting with vsync (FIFO).
    [[nodiscard]] bool IsVSyncEnabled() const { return _vsyncEnabled; }

    /// @brief GPU execution time of the most recently completed frame, in
    /// milliseconds, from a timer query spanning the whole command list. 0 until
    /// the first frame has finished on the GPU.
    [[nodiscard]] float GetLastGpuFrameTimeMs() const { return _lastGpuFrameMs; }

    /// @brief Wall-clock time the last BeginFrame() spent blocked on the
    /// frames-in-flight throttle (waiting for the GPU to finish an earlier
    /// frame), in milliseconds. Lets callers subtract GPU-stall time from a CPU
    /// frame-time figure; 0 when the CPU didn't have to wait (GPU-idle, i.e.
    /// CPU-bound).
    [[nodiscard]] double GetLastGpuWaitMs() const { return _lastGpuWaitMs; }

    /// @name Per-pass GPU timing
    /// Off by default, and that is a correctness requirement rather than a
    /// performance preference. nvrhi's `beginTimerQuery`/`endTimerQuery` each
    /// call `endRenderPass()` (see nvrhi's vulkan-queries.cpp), so bracketing a
    /// pass **splits the render pass it sits in**. Left always-on, the shipping
    /// frame would carry a different render-pass structure than the one being
    /// measured, and the measurement would be of a frame nobody ships.
    ///
    /// So: whole-frame cost (GetLastGpuFrameTimeMs) stays the primary number and
    /// is always available, because its single bracket spans the entire command
    /// list and splits nothing. Per-pass numbers are opt-in, for the times the
    /// question is *which* pass moved.
    ///@{

    /// @brief Turn per-pass timing on or off. Takes effect on the next frame;
    /// timings from before a disable stay readable until then.
    void SetPassTimingEnabled(bool enabled) { _passTimingEnabled = enabled; }

    [[nodiscard]] bool IsPassTimingEnabled() const { return _passTimingEnabled; }

    /// @brief Open a timer around the pass named @p name, on this frame's
    /// command list. No-op when pass timing is off, when called outside a frame,
    /// or once the per-frame slot capacity is exhausted (a one-time warning).
    ///
    /// @warning Must not nest. A nested pair would time a range that contains
    /// another render-pass break, which measures the breaks rather than the
    /// work. Prefer Render::GpuPassTimerScope, which pairs the calls for you.
    void BeginPassTimer(const char *name);

    /// @brief Close the timer opened by the matching BeginPassTimer().
    void EndPassTimer();

    /// @brief One pass's resolved GPU time.
    struct PassTiming
    {
        const char *name = nullptr; ///< The literal handed to BeginPassTimer; not owned.
        float milliseconds = 0.f;
    };

    /// @brief The most recently *resolved* frame's per-pass timings — which is
    /// kFramesInFlight frames behind the one being recorded, since a query
    /// cannot be read until its frame has finished on the GPU. Empty while pass
    /// timing is off. The span is invalidated by the next BeginFrame().
    [[nodiscard]] std::span<const PassTiming> GetPassTimings() const;
    ///@}

    [[nodiscard]] nvrhi::IDevice *GetDevice() const { return _nvrhiDevice; }

    /// @brief Max anisotropy to request when creating samplers: >1 when the
    /// device supports anisotropic filtering (clamped to the device limit), or
    /// 1.0 (isotropic — feature off) when it doesn't. Samplers should read this
    /// rather than hardcode a value, since requesting anisotropy the device
    /// didn't enable is a validation error.
    [[nodiscard]] float GetMaxAnisotropy() const { return _maxAnisotropy; }

    /// @brief Color/depth formats and sample count of the swapchain framebuffers,
    /// for constructing pipelines compatible with them ahead of the first frame.
    [[nodiscard]] nvrhi::FramebufferInfo GetFramebufferInfo() const
    {
        return _framebuffers.at(0)->getFramebufferInfo();
    }

    /// @name Raw Vulkan handles
    /// For integrating libraries that talk raw Vulkan directly instead of going
    /// through NVRHI — currently just Dear ImGui's Vulkan backend (see DebugUI.cpp).
    /// Prefer nvrhi::IDevice / getNativeObject() for anything else.
    ///@{
    [[nodiscard]] VkInstance GetVkInstance() const { return _instance; }
    [[nodiscard]] VkPhysicalDevice GetVkPhysicalDevice() const { return _physicalDevice; }
    [[nodiscard]] VkDevice GetVkDevice() const { return _device; }
    [[nodiscard]] VkQueue GetVkGraphicsQueue() const { return _graphicsQueue; }
    [[nodiscard]] uint32_t GetVkGraphicsQueueFamily() const { return _graphicsQueueFamily; }
    [[nodiscard]] uint32_t GetSwapchainImageCount() const { return static_cast<uint32_t>(_swapchainImages.size()); }
    [[nodiscard]] VkFormat GetSwapchainFormat() const { return _swapchainFormat; }

    /// Depth-stencil format chosen for the swapchain depth buffer. Queried
    /// against the device at swapchain creation (D24S8 -> D32S8 -> D32), so
    /// consumers must read it rather than assume D24S8. May be a depth-only
    /// format (D32_SFLOAT) with no stencil aspect.
    [[nodiscard]] VkFormat GetDepthFormat() const { return _depthFormat; }
    ///@}

    /// @brief Highest MSAA sample count this device supports for *both* colour and
    /// depth render targets (their masks intersected), so a caller can clamp a
    /// requested count to one that will actually create. Always at least 1.
    ///
    /// nvrhi exposes no sample-count capability query, so this reads
    /// VkPhysicalDeviceLimits directly. A requested count must be clamped against
    /// it before createTexture, which otherwise fails on a device that cannot do
    /// the requested count — and msaaSamples comes from user-editable JSON.
    [[nodiscard]] uint32_t GetMaxUsableSampleCount() const;

private:
    VulkanContext() = default;

    void DestroySwapchainResources();
    [[nodiscard]] bool CreateSwapchainResources(uint32_t width, uint32_t height);

    /// @brief Resets to a consistent no-swapchain state (`_swapchain ==
    /// VK_NULL_HANDLE`, all swapchain resources released) after a partial
    /// CreateSwapchainResources() failure that occurred once the old swapchain had
    /// already been torn down. BeginFrame()'s null-swapchain guard then catches it
    /// and a later Resize() can retry cleanly.
    void ResetToNoSwapchain();

    /// @brief The present mode CreateSwapchainResources() should use for the current
    /// vsync state: FIFO when vsync is on (always supported), otherwise IMMEDIATE if
    /// the device offers it, falling back to FIFO. The fallback writes _vsyncEnabled
    /// back to true so IsVSyncEnabled() reports the mode actually in use.
    [[nodiscard]] VkPresentModeKHR ChoosePresentMode();

    /// Installs the validation debug messenger in debug builds; no-op in
    /// release and when the debug-utils extension wasn't enabled.
    void CreateDebugMessenger();

    VkInstance _instance = VK_NULL_HANDLE;
    VkSurfaceKHR _surface = VK_NULL_HANDLE;
    VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;
    VkDevice _device = VK_NULL_HANDLE;
    VkQueue _graphicsQueue = VK_NULL_HANDLE;
    uint32_t _graphicsQueueFamily = 0;

    VkSwapchainKHR _swapchain = VK_NULL_HANDLE;
    VkFormat _swapchainFormat = VK_FORMAT_UNDEFINED;
    VkFormat _depthFormat = VK_FORMAT_UNDEFINED;
    bool _vsyncEnabled = true; // FIFO by default; see ChoosePresentMode()
    // Set when acquire/present reports VK_ERROR_OUT_OF_DATE_KHR. The window resize
    // callback only fires on a dimension change, so a *same-size* stale event
    // (display-mode / refresh-rate change, monitor hot-plug, compositor restart)
    // would otherwise never rebuild the swapchain and rendering would freeze
    // forever. BeginFrame() rebuilds at the top of the next frame when this is set.
    // Touched only on the render thread (BeginFrame/EndFrame), so a plain bool.
    bool _swapchainStale = false;
    float _maxAnisotropy = 1.0f; // >1 once anisotropic filtering is enabled; see GetMaxAnisotropy()

    VkDebugUtilsMessengerEXT _debugMessenger = VK_NULL_HANDLE;
    VkExtent2D _swapchainExtent{};
    std::vector<VkImage> _swapchainImages;

    nvrhi::vulkan::DeviceHandle _nvrhiDeviceHandle;
    nvrhi::vulkan::IDevice *_nvrhiDevice = nullptr;
    std::vector<nvrhi::TextureHandle> _swapchainTextures;
    nvrhi::TextureHandle _depthTexture;
    std::vector<nvrhi::FramebufferHandle> _framebuffers;

    // Frames-in-flight synchronization. BeginFrame() throttles to kFramesInFlight
    // by waiting only on the event query of the frame that last used this slot,
    // not the whole device — so the CPU records frame N+1 while the GPU still
    // executes frame N. The event query gates reuse of everything the prior frame
    // touched outside NVRHI's own tracking: the per-slot image-available
    // semaphore and the raw ImGui vertex/index buffers it round-robins.
    //
    // Image-available semaphores are per in-flight slot (acquire runs before the
    // image index is known). Render-finished semaphores are per swapchain image —
    // a present may still be reading an image when the same slot comes around, so
    // reusing one signal semaphore across images would race the presentation
    // engine. They therefore live and die with the swapchain (see
    // Create/DestroySwapchainResources).
    static constexpr uint32_t kFramesInFlight = 2;

    std::array<VkSemaphore, kFramesInFlight> _imageAvailableSemaphores{};
    std::array<nvrhi::EventQueryHandle, kFramesInFlight> _frameQueries;
    std::array<bool, kFramesInFlight> _frameQueryPending{};
    std::vector<VkSemaphore> _renderFinishedSemaphores;
    uint64_t _frameCounter = 0;

    // Per-slot GPU timer queries bracketing the whole command list, for the
    // debug frame-time readout. Read in BeginFrame() once the slot's event
    // query proves the frame finished (so the read never blocks). _lastGpuWaitMs
    // records how long the CPU stalled this frame waiting on the GPU/present
    // pipeline — the frames-in-flight event-query wait plus the swapchain WSI
    // blocking (vkAcquireNextImageKHR, submit, and vkQueuePresentKHR). Callers
    // subtract it from the CPU frame time so those idle waits (which under
    // vsync/back-pressure spike to a couple ms) aren't mislabeled as CPU work.
    std::array<nvrhi::TimerQueryHandle, kFramesInFlight> _timerQueries;
    float _lastGpuFrameMs = 0.0f;
    double _lastGpuWaitMs = 0.0;

    // Per-pass timing. Capacity is fixed and generous against the ~8 passes the
    // frame records today; a pass past it is dropped with one warning rather
    // than growing the pool mid-frame, since allocating a query between
    // BeginFrame and EndFrame would be a hitch inside the thing being measured.
    static constexpr uint32_t kMaxTimedPasses = 24;

    struct PassTimerSlot
    {
        std::array<nvrhi::TimerQueryHandle, kMaxTimedPasses> queries;
        std::array<const char *, kMaxTimedPasses> names{};
        uint32_t used = 0;    ///< Timers opened this frame.
        bool pending = false; ///< Recorded and awaiting resolve.
        /// The capture clock when this slot's frame began recording. A pass
        /// resolves kFramesInFlight frames after it ran, so without this the
        /// trace would file every pass under whichever frame happened to read
        /// it — the wrong one, always, and by a fixed amount that looks like a
        /// measurement.
        std::uint64_t frameBeginTicks = 0;
    };

    std::array<PassTimerSlot, kFramesInFlight> _passTimers;
    std::vector<PassTiming> _resolvedPassTimings;
    /// The row the GPU's passes are drawn on. Null until the first frame that
    /// has timings to put there, and for the whole of a build without Chiara.
    Chiara::Track *_gpuTrack = nullptr;
    /// One gap past the last frame's block on that track. A block never begins
    /// before it, so two frames' passes cannot occupy the same instant however
    /// long the GPU took over either of them.
    std::uint64_t _gpuTrackEnd = 0;

    /// @brief Draw the just-resolved pass timings onto the GPU track, inside the
    /// frame they belong to.
    ///
    /// The widths are measured; the offsets are not. A pass's duration is all
    /// the device reports, so the slices are packed end-to-end from
    /// @p frameBeginTicks and their positions within the frame are arbitrary.
    /// That is why the track is named for what it holds rather than "gpu" —
    /// proportion and order are real, alignment is not, and a row that did not
    /// say so would be read as a measurement it is not.
    void EmitGpuTrackSlices(std::uint64_t frameBeginTicks);
    bool _passTimingEnabled = false;
    bool _passTimingActive =
        false; ///< The enable this frame opened with, so a mid-frame flip cannot unbalance the pairs.
    uint32_t _openPassTimer = kMaxTimedPasses; ///< Index of the open timer, or capacity when none.
    bool _passCapacityWarned = false;

    nvrhi::CommandListHandle _commandList;

    uint32_t _currentImageIndex = 0;
};

} // namespace Assisi::Render::Vulkan
