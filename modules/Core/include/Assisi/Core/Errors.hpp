/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Errors.hpp
/// @brief Error codes used throughout the Assisi::Core asset pipeline.

#include <cstdint>

namespace Assisi::Core
{
/// @brief Describes why an AssetSystem operation failed.
enum class AssetError : std::uint8_t
{
    NotInitialized,     ///< AssetSystem::Initialize() has not been called yet.
    RootNotFound,       ///< Automatic root discovery found no `assets/` directory.
    InvalidRoot,        ///< The provided root path does not point to an existing directory.
    InvalidVirtualPath, ///< The virtual path is empty, absolute, or contains `..` traversal.
    RootEscape,         ///< The resolved path would escape the asset root directory.
    FileOpenFailed,     ///< The file exists but could not be opened.
    FileReadFailed,     ///< The file was opened but reading its contents failed.
    FileWriteFailed,    ///< The file could not be created or written (writable user root).
    UnknownAssetId,     ///< An AssetProvider was asked for an id it does not serve.
    UnsupportedEncoding, ///< A pak slice is encrypted, uses a codec, or lives in an archive, this build cannot read.
    CorruptArchive,     ///< A pak's header, index or slice does not read as the format says it should.
};
} // namespace Assisi::Core