/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Errors.hpp
/// @brief Why reading, writing, loading or cooking an asset failed.

#include <cstdint>
#include <string>
#include <string_view>

namespace Assisi::Core
{
/// @brief What went wrong with an asset, whatever kind of file it is.
enum class AssetErrorCode : std::uint8_t
{
    NotInitialized,      ///< AssetSystem::Initialize() has not been called yet.
    RootNotFound,        ///< Automatic root discovery found no `assets/` directory.
    InvalidRoot,         ///< The provided root path does not point to an existing directory.
    InvalidVirtualPath,  ///< The virtual path is empty, absolute, or contains `..` traversal.
    RootEscape,          ///< The resolved path would escape the asset root directory.
    FileNotFound,        ///< Nothing exists at the path.
    IsDirectory,         ///< The path is a directory, not a file.
    PermissionDenied,    ///< The file exists, but this process may not open it.
    FileOpenFailed,      ///< The file exists but could not be opened, for none of the reasons above.
    FileReadFailed,      ///< The file was opened but reading its contents failed.
    FileWriteFailed,     ///< The file could not be created or written (writable user root).
    UnknownAssetId,      ///< An AssetProvider was asked for an id it does not serve.
    UnsupportedEncoding, ///< The data is valid, but in a form this build cannot read: an MP3, an encrypted pak slice.
    CorruptAsset,        ///< The file reads, but its contents are not what its format says they should be.
    CorruptArchive,      ///< A pak's header, index or slice does not read as the format says it should.
    WrongType,           ///< The asset was asked for as a type its kind does not load into.
    Count,
};

/// @brief A short human-readable description, for a log line.
[[nodiscard]] std::string_view ToString(AssetErrorCode code) noexcept;

/// @brief Why an asset operation failed: a code to act on, and optionally a
///        detail for the log from whatever found the problem, such as a
///        decoder's own description of it.
///
/// `detail` is a view, so it must point at text that is never freed: a string
/// literal, or the `ToString` of an error enum. Where the asset is lives with
/// whoever logs the error, which already knows the path or id.
struct AssetError
{
    std::string_view detail;
    AssetErrorCode code;

    AssetError(AssetErrorCode errorCode, std::string_view errorDetail = {}) noexcept
        : detail(errorDetail), code(errorCode)
    {
    }

    [[nodiscard]] bool operator==(AssetErrorCode other) const noexcept { return code == other; }
};

/// @brief The code's description, followed by the detail in parentheses when
///        there is one: "the file's contents are corrupt (the audio could not
///        be decoded)".
[[nodiscard]] std::string Describe(const AssetError &error);
} // namespace Assisi::Core
