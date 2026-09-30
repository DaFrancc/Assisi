/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SoundAsset.hpp
/// @brief The "sound" asset kind: a `.wav`, `.flac` or `.ogg` file that loads as
///        a PcmClip.
///
/// Registered by SoundKind.cpp, cooked by SoundCook.cpp. Both name the kind
/// through these constants, so the cook step cannot name a kind that is not
/// the one registered.

#include <Assisi/Audio/AudioError.hpp>
#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Core/Errors.hpp>

#include <string_view>

namespace Assisi::Audio
{

/// @brief What a sound's sidecar writes as its kind.
inline constexpr std::string_view kSoundKindName = "sound";

inline constexpr Core::AssetKindId kSoundKind{kSoundKindName};

/// @brief A decoder's error as an asset error: a sound this build cannot read is
///        UnsupportedEncoding, any other failure means the file is corrupt, and
///        the decoder's description is the detail.
[[nodiscard]] inline Core::AssetError ToAssetError(AudioError error) noexcept
{
    const Core::AssetErrorCode code = error == AudioError::UnsupportedEncoding
                                          ? Core::AssetErrorCode::UnsupportedEncoding
                                          : Core::AssetErrorCode::CorruptAsset;
    return Core::AssetError{code, ToString(error)};
}

} // namespace Assisi::Audio
