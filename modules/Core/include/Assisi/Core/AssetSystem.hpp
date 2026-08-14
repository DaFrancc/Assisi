/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AssetSystem.hpp
/// @brief Virtual-path filesystem for engine assets and per-user writable data.
///
/// The engine has a single filesystem story with two mounts, both addressed by
/// the same virtual-path scheme and both escape-protected:
///
///   - **Asset root** — read-only shipped content (models, textures, shaders,
///     levels). Discovered by Initialize()/SetRoot(). Reached via Resolve(),
///     ReadText(), ReadBinary(), Exists().
///   - **User root** — read-write per-user data (saves, options, logs, crash
///     dumps, screenshots). Reached via ResolveUser(), ReadUserText(),
///     ReadUserBinary(), WriteText(), WriteBinary(), UserExists().
///
/// The split is deliberate: a shipped game's install directory is frequently
/// not writable (Program Files, app bundles, read-only mounts), so runtime
/// writes must not target the asset tree. The user root defaults to the
/// executable's directory (deterministic and CWD-independent — see GetUserRoot)
/// and can be redirected with ASSISI_USER_ROOT or SetUserRoot() (e.g. to a
/// platform per-user data directory).
///
/// All public functions are static; `AssetSystem` acts as a process-wide
/// singleton service. Call Initialize() (or SetRoot()) once before using the
/// asset-root readers; the user root initializes lazily on first use, so the
/// writable API is usable before Initialize() (the logger relies on this).
///
/// @note Intentional service-locator: each root is a single process-wide
/// resource, and SetRoot()/SetUserRoot() keep them test-controllable (see the
/// Core tests, which point them at temp dirs) — so the shared state does not
/// block testing. The static API is a deliberate convenience over threading a
/// handle through every asset load.

#include <cstddef>
#include <expected>
#include <filesystem>
#include <span>
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
     * @brief Sets the *authoring* root — the durable copy of the asset tree that
     *        newly minted sidecars must also be written to.
     *
     * A dev build runs against a staged copy of the assets sitting next to the
     * executable, because generated files (compiled `.spv`) only exist there. That
     * copy is disposable: anything minted into it (an asset's `.aast` GUID) is lost
     * on the next clean build and regenerated with a *different* GUID, so any
     * reference stored by GUID silently stops resolving. Pointing this at the
     * source asset tree makes minted sidecars durable and version-controllable.
     *
     * @param root Path to the durable asset tree, or empty to disable mirroring.
     *
     * @note Read paths are unaffected — Resolve/Read* always use GetRoot(). This
     *       only adds a second destination when a sidecar is created.
     * @note Leave unset for shipped builds, where the staged copy IS the durable
     *       tree and there is no source tree to mirror into.
     */
    static void SetAuthoringRoot(const std::filesystem::path &root) noexcept;

    /**
     * @brief The authoring root set by SetAuthoringRoot, or an empty path if
     *        sidecar mirroring is disabled (the default).
     */
    static const std::filesystem::path &GetAuthoringRoot() noexcept;

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

    // --- Writable user root ---------------------------------------------------

    /**
     * @brief Sets the writable user-data root explicitly and marks it initialized.
     *
     * @param root Absolute or relative path to a per-user writable directory.
     *
     * @return std::expected<void, AssetError>
     *   - Success: root is accepted and cached.
     *   - Failure: AssetError::InvalidRoot if the path is not an existing directory.
     *
     * @note Overrides both the ASSISI_USER_ROOT default and any prior lazy
     *   initialization. Use this to redirect writes to a platform per-user
     *   location on a shipped game.
     */
    static std::expected<void, AssetError> SetUserRoot(const std::filesystem::path &root) noexcept;

    /**
     * @brief Returns the writable user-data root, initializing it on first use.
     *
     * Lazy initialization order: ASSISI_USER_ROOT (if it names a directory),
     * else the executable's directory, else the current working directory. This
     * does not require the asset root to be initialized.
     *
     * @return const std::filesystem::path& Reference to the cached user root.
     */
    static const std::filesystem::path &GetUserRoot() noexcept;

    /**
     * @brief Absolute path of the running executable, or nullopt if the
     * platform would not say.
     *
     * For relaunching *this* build as a second process — the editor spawning
     * play-in-editor clients. argv[0] is not a substitute: it is whatever the
     * caller typed, which may be a bare name resolved through PATH, a relative
     * path from a working directory that has since changed, or a symlink.
     */
    [[nodiscard]] static std::optional<std::filesystem::path> ExecutablePath() noexcept;

    /**
     * @brief Resolves a virtual path against the writable user root.
     *
     * Same normalization and escape protection as Resolve(); the target need
     * not exist (e.g. a save file about to be written).
     *
     * @param vpath Virtual path relative to the user root (e.g. "saves/slot1.sav").
     * @return std::expected<std::filesystem::path, AssetError> Absolute path, or a resolution error.
     */
    static std::expected<std::filesystem::path, AssetError> ResolveUser(std::string_view vpath) noexcept;

    /**
     * @brief Checks whether a virtual path under the user root exists.
     */
    static bool UserExists(std::string_view vpath) noexcept;

    /**
     * @brief Reads a file under the user root as UTF-8 text.
     *
     * @param vpath Virtual path relative to the user root.
     * @return std::expected<std::string, AssetError> Contents, or a read/resolution error.
     */
    static std::expected<std::string, AssetError> ReadUserText(std::string_view vpath) noexcept;

    /**
     * @brief Reads a file under the user root as raw bytes.
     *
     * @param vpath Virtual path relative to the user root.
     * @return std::expected<std::vector<std::byte>, AssetError> Contents, or a read/resolution error.
     */
    static std::expected<std::vector<std::byte>, AssetError> ReadUserBinary(std::string_view vpath) noexcept;

    /**
     * @brief Writes UTF-8 text to a file under the user root, creating parent directories.
     *
     * Any existing file is truncated. Written in binary mode to avoid newline translation.
     *
     * @param vpath Virtual path relative to the user root.
     * @param data  Text to write.
     * @return std::expected<void, AssetError> Success, or FileWriteFailed / a resolution error.
     */
    static std::expected<void, AssetError> WriteText(std::string_view vpath, std::string_view data) noexcept;

    /**
     * @brief Writes raw bytes to a file under the user root, creating parent directories.
     *
     * Any existing file is truncated.
     *
     * @param vpath Virtual path relative to the user root.
     * @param data  Bytes to write.
     * @return std::expected<void, AssetError> Success, or FileWriteFailed / a resolution error.
     */
    static std::expected<void, AssetError> WriteBinary(std::string_view vpath,
                                                       std::span<const std::byte> data) noexcept;

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

    /**
     * @brief Normalizes @p vpath and joins it under @p root, rejecting escapes.
     *
     * Shared spine of Resolve() and ResolveUser(): the only difference between
     * the two mounts is which cached root they resolve against.
     */
    static std::expected<std::filesystem::path, AssetError> ResolveUnder(const std::filesystem::path &root,
                                                                         std::string_view vpath) noexcept;

    /**
     * @brief Lazily initializes the writable user root (ASSISI_USER_ROOT, else
     * the executable's directory, else the current working directory).
     */
    static void EnsureUserRoot() noexcept;
};
} // namespace Assisi::Core