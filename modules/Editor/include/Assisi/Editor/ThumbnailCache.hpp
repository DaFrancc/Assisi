/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ThumbnailCache.hpp
/// @brief The asset browser's image thumbnails: files decoded by path on a
///        worker and shown through ImGui.
///
/// Apart from the scene's asset cache on purpose. Thumbnails browse files that
/// may be referenced by nothing, never take a bindless slot, and are dropped on
/// a directory change rather than on a level load. They are also the one place
/// the renderer's textures are decoded straight from an image file, which is an
/// editor's business and not a shipped game's.

#include <atomic>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include <nvrhi/nvrhi.h>

#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/Core/JobSystem.hpp>
#include <Assisi/Render/Texture.hpp>

namespace Assisi::Editor
{

class ThumbnailCache
{
public:
    /// @brief Bind to a device and the job system decodes run on. With no job
    ///        system (a headless test) a thumbnail decodes synchronously.
    void Initialize(nvrhi::IDevice *device, Core::JobSystem *jobs);

    /// @brief The thumbnail for @p path, or null while it decodes or after it failed.
    ///
    /// The first resolve kicks a worker decode and returns null; once decoded and
    /// uploaded on the main thread a later resolve returns the texture. A failure
    /// is remembered, so a broken file is not re-decoded every frame. Always linear:
    /// ImGui draws it straight, and sampling it as sRGB would show it too dark.
    const Render::Texture *Resolve(const Core::AssetPath &path);

    /// @brief Whether a decode for @p path is in flight — so the browser shows a
    ///        loading indicator only while one really is.
    [[nodiscard]] bool IsLoading(const Core::AssetPath &path) const { return _loading.contains(path); }

    /// @brief Invoked once per resident thumbnail about to be freed, with the
    /// texture still valid, so the caller can drop its ImGui binding. Without it a
    /// freed texture whose address a later one reuses would alias a stale binding.
    using ReleaseFn = std::function<void (nvrhi::ITexture *)>;

    /// @brief Drop every thumbnail and cancel in-flight decodes. Waits for the GPU
    ///        to idle first — a navigation-time stall, not a per-frame one.
    void Clear(const ReleaseFn &onRelease = {});

private:
    nvrhi::IDevice *_device = nullptr;
    Core::JobSystem *_jobs  = nullptr;

    /// A default-constructed (invalid) entry marks a decode that failed.
    std::unordered_map<Core::AssetPath, Render::Texture> _thumbnails;
    std::unordered_set<Core::AssetPath> _loading;

    /// Bumped by Clear; a decode whose captured epoch no longer matches is dropped.
    /// Atomic because the worker reads it to skip a decode nobody will show.
    std::atomic<std::uint64_t> _epoch = 0;
};

} // namespace Assisi::Editor
