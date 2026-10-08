/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Geometry/AnimationImport.hpp>

#include <Assisi/Geometry/AnimationFile.hpp>

#include <fastgltf/core.hpp>

#include <filesystem>
#include <variant>

#include "AnimationPayload.hpp"
#include "GltfAnimation.hpp"

namespace Assisi::Geometry
{

std::expected<std::vector<std::byte>, Core::AssetError> CookAnimation(std::span<const std::byte> glb)
{
    fastgltf::Expected<fastgltf::GltfDataBuffer> data = fastgltf::GltfDataBuffer::FromBytes(glb.data(), glb.size());
    if (data.error() != fastgltf::Error::None)
    {
        return std::unexpected(ToAssetError(AnimationReadError::ParseFailed));
    }
    fastgltf::Parser parser;
    fastgltf::Expected<fastgltf::Asset> asset = parser.loadGltf(data.get(), std::filesystem::path{});
    if (asset.error() != fastgltf::Error::None)
    {
        return std::unexpected(ToAssetError(AnimationReadError::ParseFailed));
    }
    if (asset.get().animations.size() != 1)
    {
        return std::unexpected(ToAssetError(AnimationReadError::NotOneClip));
    }
    for (const fastgltf::Buffer &buffer : asset.get().buffers)
    {
        if (std::holds_alternative<fastgltf::sources::URI>(buffer.data))
        {
            return std::unexpected(ToAssetError(AnimationReadError::ExternalBuffer));
        }
    }

    const std::expected<GltfClip, AnimationReadError> clip = ReadGltfAnimation(asset.get(), 0);
    if (!clip)
    {
        return std::unexpected(ToAssetError(clip.error()));
    }
    return WriteAnimationPayload(clip->clip);
}

} // namespace Assisi::Geometry
