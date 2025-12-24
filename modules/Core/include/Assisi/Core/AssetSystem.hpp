/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "Assisi/Core/Errors.hpp"

namespace Assisi::Core
{
class AssetSystem
{
  public:
    /**
     * @brief Initializes the asset system by discovering and caching the asset root.
     *
     * The root is discovered using the internal discovery workflow (environment override, then
     * upward search for an `assets/` directory). This function is idempotent: calling it multiple
     * times after successful initialization is a no-op.
     *
     * @return std::expected<void, AssetError>
     *   - Success: the system is ready to resolve and load assets.
     *   - Failure: an AssetError describing why initialization failed.
     *
     * @note After successful initialization, GetRoot() is valid and Resolve/Read* will work.
     */
    static std::expected<void, AssetError> Initialize() noexcept;

    /**
     * @brief Sets the asset root explicitly and marks the system initialized.
     *
     * @param root Absolute or relative filesystem path to the asset root directory.
     *
     * @return std::expected<void, AssetError>
     *   - Success: root is accepted and cached.
     *   - Failure: AssetError::InvalidRoot if the path is not an existing directory.
     *
     * @note The stored root is canonicalized (weakly) to normalize path comparisons.
     */
    static std::expected<void, AssetError> SetRoot(const std::filesystem::path &root) noexcept;

    /**
     * @brief Returns the cached asset root directory.
     *
     * @return const std::filesystem::path& Reference to the cached root.
     *
     * @warning Precondition: the system must be initialized (Initialize() or SetRoot()).
     */
    static const std::filesystem::path &GetRoot() noexcept;

    /**
     * @brief Resolves a virtual asset path to an absolute filesystem path under the asset root.
     *
     * Virtual paths are normalized (separator normalization, lexical normalization, and component
     * validation) and then joined with the cached root. The resulting path is canonicalized and
     * validated to ensure it does not escape the root (e.g., via `..` tricks).
     *
     * @param vpath Virtual path relative to the asset root (e.g., "textures/white.png").
     *
     * @return std::expected<std::filesystem::path, AssetError>
     *   - Success: an absolute path pointing to the resolved asset location.
     *   - Failure: NotInitialized, InvalidVirtualPath, RootEscape, etc.
     */
    static std::expected<std::filesystem::path, AssetError> Resolve(std::string_view vpath) noexcept;

    /**
     * @brief Checks whether a virtual asset path resolves to an existing filesystem entry.
     *
     * @param vpath Virtual path relative to the asset root.
     * @return true If resolution succeeds and the resolved path exists.
     * @return false If resolution fails or the resolved path does not exist.
     */
    static bool Exists(std::string_view vpath) noexcept;

    /**
     * @brief Reads an entire file as UTF-8 text (returned as a std::string).
     *
     * The file is opened in binary mode to avoid platform newline translation, then the full
     * contents are read into memory.
     *
     * @param vpath Virtual path relative to the asset root.
     *
     * @return std::expected<std::string, AssetError>
     *   - Success: file contents.
     *   - Failure: FileOpenFailed, FileReadFailed, or a resolution-related error.
     */
    static std::expected<std::string, AssetError> ReadText(std::string_view vpath) noexcept;

    /**
     * @brief Reads an entire file as raw bytes.
     *
     * The file is opened in binary mode, sized by seeking to end, then read into a byte buffer.
     *
     * @param vpath Virtual path relative to the asset root.
     *
     * @return std::expected<std::vector<std::byte>, AssetError>
     *   - Success: file contents as bytes.
     *   - Failure: FileOpenFailed, FileReadFailed, or a resolution-related error.
     */
    static std::expected<std::vector<std::byte>, AssetError> ReadBinary(std::string_view vpath) noexcept;

  private:
    /**
     * @brief Returns whether the asset system has been initialized.
     *
     * @return true If Initialize() or SetRoot() has successfully completed.
     * @return false Otherwise.
     */
    static bool IsInitialized() noexcept;

    /**
     * @brief Attempts to discover the asset root directory automatically.
     *
     * Discovery order:
     *  1) Environment variable override (ASSISI_ASSET_ROOT) if it exists and points to a directory.
     *  2) Walk upward from the current working directory searching for a child directory named `assets`.
     *
     * @return std::expected<std::filesystem::path, AssetError>
     *   - Success: discovered root path.
     *   - Failure: AssetError::RootNotFound if discovery fails.
     */
    static std::expected<std::filesystem::path, AssetError> DiscoverRoot() noexcept;

    /**
     * @brief Normalizes and validates a virtual path.
     *
     * This rejects:
     *  - empty paths
     *  - absolute paths (leading '/')
     *  - drive-qualified paths (contains ':')
     *  - parent traversal components ("..") after lexical normalization
     *
     * It also normalizes separators so callers can use either '/' or '\\'.
     *
     * @param vpath Virtual path string to normalize.
     *
     * @return std::expected<std::filesystem::path, AssetError>
     *   - Success: normalized relative path.
     *   - Failure: AssetError::InvalidVirtualPath for invalid inputs.
     */
    static std::expected<std::filesystem::path, AssetError> NormalizeVirtualPath(std::string_view vpath) noexcept;
};
} // namespace Assisi::Core