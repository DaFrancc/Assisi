/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/ThumbnailCache.hpp>

#include <Assisi/Editor/TextureFile.hpp>
#include <Assisi/Image/Decode.hpp>

#include <expected>
#include <string>

namespace Assisi::Editor
{

void ThumbnailCache::Initialize(nvrhi::IDevice *device, Core::JobSystem *jobs)
{
    _device = device;
    _jobs   = jobs;
}

const Render::Texture *ThumbnailCache::Resolve(const Core::AssetPath &path)
{
    if (path.Empty())
    {
        return nullptr;
    }

    // Resident already: a valid entry is ready to show; an invalid one records a
    // decode that failed, so a broken file is not re-kicked every frame.
    if (const auto it = _thumbnails.find(path); it != _thumbnails.end())
    {
        return it->second.IsValid() ? &it->second : nullptr;
    }

    // A decode for this path is already in flight — show the placeholder until it
    // publishes, without kicking a duplicate.
    if (_loading.contains(path))
    {
        return nullptr;
    }

    // No job system: degrade to a synchronous load rather than a thumbnail that
    // never resolves.
    if (_jobs == nullptr)
    {
        Render::Texture &texture = _thumbnails[path];
        if (!LoadTextureFile(texture, _device, path.View(), Image::ColorSpace::Linear))
        {
            return nullptr; // keep the invalid entry so a broken file isn't retried
        }
        return &texture;
    }

    _loading.insert(path);
    const std::uint64_t epoch                    = _epoch.load(std::memory_order_relaxed);
    const std::atomic<std::uint64_t> *liveEpoch  = &_epoch;
    const std::string vpath(path.View());

    _jobs
    ->Run(Core::Pool::Worker,
          [vpath, epoch, liveEpoch]() -> std::expected<Image::DecodedImage, Core::AssetError>
          {
              // Skip the decode if a directory change already superseded this
              // thumbnail. The error is never inspected — the continuation returns
              // on the epoch mismatch before it looks at the result.
              if (liveEpoch->load(std::memory_order_relaxed) != epoch)
              {
                  return std::unexpected(Core::AssetError::FileReadFailed);
              }
              return Image::DecodeImage(vpath, Image::ColorSpace::Linear);
          })
    .Then(Core::Pool::Main,
          [this, path, epoch](std::expected<Image::DecodedImage, Core::AssetError> decoded)
          {
              // Superseded: return before erasing, so a stale completion cannot drop
              // a live epoch's loading marker and re-kick a load.
              if (epoch != _epoch.load(std::memory_order_relaxed))
              {
                  return;
              }
              _loading.erase(path);
              Render::Texture &texture = _thumbnails[path];
              if (decoded)
              {
                  texture.UploadDecoded(_device, *decoded, std::string(path.View()).c_str());
              }
          });

    return nullptr; // loading — the browser shows a placeholder tile this frame
}

void ThumbnailCache::Clear(const ReleaseFn &onRelease)
{
    // A thumbnail may still be sampled by an in-flight frame's ImGui draw, so the
    // GPU drains before anything is freed; with it idle, the caller can drop each
    // texture's ImGui binding safely.
    _device->waitForIdle();
    if (onRelease)
    {
        for (auto &[path, texture] : _thumbnails)
        {
            if (texture.IsValid())
            {
                onRelease(texture.NativeTexture());
            }
        }
    }

    ++_epoch;
    _loading.clear();
    _thumbnails.clear();
}

} // namespace Assisi::Editor
