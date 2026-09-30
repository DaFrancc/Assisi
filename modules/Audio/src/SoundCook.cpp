/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file SoundCook.cpp
/// @brief The sound kind's cook step: a file that would not decode fails the
///        cook, and one that decodes ships exactly as authored.
///
/// The source is shipped rather than the decoded samples because samples are
/// many times larger, and the game decodes on a worker either way.

#include <Assisi/Audio/AudioError.hpp>
#include <Assisi/Audio/Clip.hpp>
#include <Assisi/Audio/SoundAsset.hpp>
#include <Assisi/Core/AssetKind.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace Assisi::Audio
{

namespace
{

/// Raise when the step ships something different for the same source.
constexpr std::uint32_t kSoundCookVersion = 1;

std::expected<std::vector<std::byte>, Core::AssetError> CookSound(std::span<const std::byte> source)
{
    if (const std::expected<PcmClip, AudioError> clip = DecodeClip(source); !clip)
    {
        return std::unexpected(ToAssetError(clip.error()));
    }
    return std::vector<std::byte>{source.begin(), source.end()};
}

[[maybe_unused]] const bool kRegistered = Core::AssetKindRegistry::Instance().RegisterCookStep(
    Core::MakeAssetCookStep(kSoundKind, kSoundCookVersion, CookSound));

} // namespace

} // namespace Assisi::Audio
