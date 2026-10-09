/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file BlendSpaceKind.cpp
/// @brief Registers the blend space kind: `.ablnd` files, loaded from their text.

#include <Assisi/Core/AssetKind.hpp>
#include <Assisi/Geometry/BlendSpace.hpp>

#include <expected>
#include <span>
#include <string>
#include <utility>

namespace Assisi::Geometry
{

namespace
{

std::expected<BlendSpace, Core::AssetError> LoadBlendSpace(std::span<const std::byte> payload)
{
    std::expected<BlendSpace, BlendSpaceError> space = ReadBlendSpace(payload);
    if (!space)
    {
        return std::unexpected(ToAssetError(space.error()));
    }
    return std::move(*space);
}

[[maybe_unused]] const bool kRegistered =
    Core::AssetKindRegistry::Instance().Register(Core::MakeAssetKind<BlendSpace>(
        std::string{kBlendSpaceKindName}, {Core::AssetFormat{.extension = ".ablnd", .preferred = true}},
        LoadBlendSpace));

} // namespace

} // namespace Assisi::Geometry
