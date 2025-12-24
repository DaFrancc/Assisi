/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

namespace Assisi::Core
{
enum class AssetError
{
    NotInitialized,
    RootNotFound,
    InvalidRoot,
    InvalidVirtualPath,
    RootEscape,
    FileOpenFailed,
    FileReadFailed
};
} // namespace Assisi::Core
