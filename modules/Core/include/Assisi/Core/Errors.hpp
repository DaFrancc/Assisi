/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Errors.hpp
/// @brief Error codes used throughout the Assisi::Core asset pipeline.

namespace Assisi::Core
{
/// @brief Describes why an AssetSystem operation failed.
enum class AssetError
{
    NotInitialized,    ///< AssetSystem::Initialize() has not been called yet.
    RootNotFound,      ///< Automatic root discovery found no `assets/` directory.
    InvalidRoot,       ///< The provided root path does not point to an existing directory.
    InvalidVirtualPath, ///< The virtual path is empty, absolute, or contains `..` traversal.
    RootEscape,        ///< The resolved path would escape the asset root directory.
    FileOpenFailed,    ///< The file exists but could not be opened.
    FileReadFailed     ///< The file was opened but reading its contents failed.
};
} // namespace Assisi::Core