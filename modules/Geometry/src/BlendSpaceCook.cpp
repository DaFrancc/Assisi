/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file BlendSpaceCook.cpp
/// @brief The blend space kind's cook step: a space no player could play fails
///        the cook, and one it could ships as written.

#include <Assisi/Core/AssetKind.hpp>
#include <Assisi/Geometry/BlendSpace.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace Assisi::Geometry
{

namespace
{

/// Raise when the step would cook the same file differently.
constexpr uint32_t kBlendSpaceCookVersion = 1;

std::expected<std::vector<std::byte>, Core::AssetError> CookBlendSpace(std::span<const std::byte> source)
{
    if (const std::expected<BlendSpace, BlendSpaceError> space = ReadBlendSpace(source); !space)
    {
        return std::unexpected(ToAssetError(space.error()));
    }
    return std::vector<std::byte>{source.begin(), source.end()};
}

[[maybe_unused]] const bool kRegistered = Core::AssetKindRegistry::Instance().RegisterCookStep(
    Core::MakeAssetCookStep(kBlendSpaceKind, kBlendSpaceCookVersion, CookBlendSpace));

} // namespace

} // namespace Assisi::Geometry
