/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file SoundKind.cpp
/// @brief Registers the sound kind: its formats, and decoding a file into the
///        clip the mixer plays.

#include <Assisi/Audio/AudioError.hpp>
#include <Assisi/Audio/Clip.hpp>
#include <Assisi/Audio/SoundAsset.hpp>
#include <Assisi/Core/AssetKind.hpp>

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <utility>

namespace Assisi::Audio
{

namespace
{

/// The cooked payload is the source file, so loading is decoding it.
std::expected<PcmClip, Core::AssetError> LoadSound(std::span<const std::byte> payload)
{
    std::expected<PcmClip, AudioError> clip = DecodeClip(payload);
    if (!clip)
    {
        return std::unexpected(ToAssetError(clip.error()));
    }
    return std::move(*clip);
}

bool Register()
{
    // No engine kind reads these formats, so preferring them gives every new
    // sound file this kind without a tie to break.
    return Core::AssetKindRegistry::Instance().Register(
        Core::MakeAssetKind<PcmClip>(std::string{kSoundKindName},
                                     {Core::AssetFormat{.extension = ".wav", .preferred = true},
                                      Core::AssetFormat{.extension = ".flac", .preferred = true},
                                      Core::AssetFormat{.extension = ".ogg", .preferred = true}},
                                     LoadSound));
}

[[maybe_unused]] const bool kRegistered = Register();

} // namespace

} // namespace Assisi::Audio
