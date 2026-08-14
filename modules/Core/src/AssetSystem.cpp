/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "Assisi/Core/AssetSystem.hpp"

#include "Assisi/Core/Logger.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>

#ifdef _WIN32
#    include <windows.h>
#else
#    include <unistd.h>
#endif

namespace Assisi::Core
{
namespace fs = std::filesystem;

/// Every `std::filesystem` call in this file goes through the `std::error_code`
/// overload rather than the throwing one. They are the same functions — the ec
/// form is `noexcept` and reports by value — and choosing it is what makes the
/// `noexcept` on every function here true rather than aspirational.
///
/// Three functions used to get this wrong in the direction that terminates:
/// Initialize, SetRoot and SetUserRoot were `noexcept` and called
/// `weakly_canonical` / `is_directory` with no handler at all, so a permission
/// error or an unmounted volume was `std::terminate`, not a bad return.
///
/// A few `try` blocks survive below and each says why: constructing an
/// `fs::path` from an OS or environment string performs an encoding conversion
/// that has no ec overload, and the byte read/write paths use iostreams.

// Shortened spellings for the deeply-scoped result types this file returns.
using PathResult = std::expected<fs::path, AssetError>;
using BytesResult = std::expected<std::vector<std::byte>, AssetError>;
using VoidResult = std::expected<void, AssetError>;

/* Read-only asset root, cached after Initialize()/SetRoot(). */
static fs::path gAssetRoot;

/* Durable mirror destination for newly minted sidecars; empty = mirroring off
   (the default, and the right setting for a shipped build). See
   AssetSystem::SetAuthoringRoot. */
static fs::path gAuthoringRoot;
static bool gInitialized = false;

/* Writable user-data root, cached on first use (lazy; see EnsureUserRoot). */
static fs::path gUserRoot;
static bool gUserRootInitialized = false;

namespace
{
/* Absolute path of the running executable, if it can be found. */
std::optional<fs::path> ExePath() noexcept
{
    try
    {
#ifdef _WIN32
        std::array<wchar_t, MAX_PATH> buf = {};
        if (GetModuleFileNameW(nullptr, buf.data(), MAX_PATH) != 0)
        {
            return fs::path(buf.data());
        }
#else
        std::array<char, 4096> buf = {};
        const ssize_t len = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
        if (len > 0)
        {
            return fs::path(std::string(buf.data(), static_cast<size_t>(len)));
        }
#endif
    }
    catch (const std::exception &)
    {
    }
    return std::nullopt;
}

/* Absolute directory containing the running executable, if it can be found.
   CWD-independent, so both root discovery and the user-root default anchor
   here rather than on wherever the process happened to be launched from. */
std::optional<fs::path> ExeDir() noexcept
{
    const std::optional<fs::path> exe = ExePath();
    if (!exe)
    {
        return std::nullopt;
    }
    /* parent_path() is lexical — it touches no filesystem and cannot fail for a
       path we already hold. */
    return exe->parent_path();
}

/* Reads an ASSISI_* env var as a path, returning it only when it names a directory. */
std::optional<fs::path> EnvDir(const char *name) noexcept
{
    /* The try is for the fs::path construction below, which converts an
       environment string into the platform's native encoding and has no
       error_code form. is_directory does, and uses it. */
    try
    {
        std::error_code ec;
#ifdef _WIN32
        char *env = nullptr;
        size_t len = 0;
        /* _dupenv_s allocates; we must free env when present. */
        if (_dupenv_s(&env, &len, name) == 0 && env != nullptr)
        {
            fs::path envPath(env);
            free(env);
            if (fs::is_directory(envPath, ec) && !ec)
            {
                return envPath;
            }
        }
#else
        if (const char *env = std::getenv(name))
        {
            fs::path envPath(env);
            if (fs::is_directory(envPath, ec) && !ec)
            {
                return envPath;
            }
        }
#endif
    }
    catch (const std::exception &)
    {
    }
    return std::nullopt;
}

/* Reads an entire file into a byte buffer sized in one seek-to-end pass. */
BytesResult ReadFileBytes(const fs::path &path) noexcept
{
    try
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
        {
            return std::unexpected(AssetError::FileOpenFailed);
        }

        /* tellg() can fail (e.g., non-seekable streams); treat as read failure. */
        const std::streampos end = file.tellg();
        if (end < 0)
        {
            return std::unexpected(AssetError::FileReadFailed);
        }

        std::vector<std::byte> data(static_cast<size_t>(end));
        file.seekg(0);
        file.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));

        /* Detect read failures (EOF is acceptable exactly at end-of-file). */
        if (!file.good() && !file.eof())
        {
            return std::unexpected(AssetError::FileReadFailed);
        }

        return data;
    }
    catch (const std::exception &e)
    {
        Log::Warn("AssetSystem: exception reading '{}': {}", path.string(), e.what());
        return std::unexpected(AssetError::FileReadFailed);
    }
}

/* Writes bytes to path, creating parent directories and truncating any existing file. */
VoidResult WriteFileBytes(const fs::path &path, std::span<const std::byte> data) noexcept
{
    if (path.has_parent_path())
    {
        /* Already-exists is not an error and reports as ec-clear + false, so the
           return value is deliberately ignored — only ec decides. */
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (ec)
        {
            Log::Warn("AssetSystem: cannot create the directory for '{}' ({}).", path.string(),
                      ec.message());
            return std::unexpected(AssetError::FileWriteFailed);
        }
    }

    /* The try covers the ofstream below, which reports through exceptions when a
       stream's exception mask is set and can throw from its own allocations. */
    try
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            return std::unexpected(AssetError::FileWriteFailed);
        }

        file.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!file.good())
        {
            return std::unexpected(AssetError::FileWriteFailed);
        }

        return {};
    }
    catch (const std::exception &e)
    {
        Log::Warn("AssetSystem: exception writing '{}': {}", path.string(), e.what());
        return std::unexpected(AssetError::FileWriteFailed);
    }
}
} // namespace

VoidResult AssetSystem::Initialize() noexcept
{
    if (gInitialized)
    {
        return {};
    }

    const PathResult root = DiscoverRoot();
    if (!root)
    {
        return std::unexpected(root.error());
    }

    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(*root, ec);
    if (ec)
    {
        Log::Warn("AssetSystem: cannot canonicalize the asset root '{}' ({}).", root->string(), ec.message());
        return std::unexpected(AssetError::InvalidRoot);
    }

    gAssetRoot   = std::move(canonical);
    gInitialized = true;

    return {};
}

VoidResult AssetSystem::SetRoot(const fs::path &root) noexcept
{
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec)
    {
        return std::unexpected(AssetError::InvalidRoot);
    }

    fs::path canonical = fs::weakly_canonical(root, ec);
    if (ec)
    {
        Log::Warn("AssetSystem: cannot canonicalize '{}' ({}).", root.string(), ec.message());
        return std::unexpected(AssetError::InvalidRoot);
    }

    gAssetRoot   = std::move(canonical);
    gInitialized = true;

    return {};
}

const fs::path &AssetSystem::GetRoot() noexcept
{
    /* Preconditions: AssetSystem is initialized. */
    return gAssetRoot;
}

void AssetSystem::SetAuthoringRoot(const fs::path &root) noexcept
{
    if (root.empty())
    {
        gAuthoringRoot.clear();
        return;
    }

    std::error_code ec;
    gAuthoringRoot = fs::weakly_canonical(root, ec);
    if (ec)
    {
        /* Mirroring off rather than pointed somewhere unresolved — but said out
           loud, because a silently disabled authoring mirror looks exactly like a
           working one until somebody goes looking for the sidecars. */
        Log::Warn("AssetSystem: cannot use '{}' as the authoring root ({}); mirroring is off.",
                  root.string(), ec.message());
        gAuthoringRoot.clear();
        return;
    }

    if (gAuthoringRoot == gAssetRoot)
    {
        /* Same tree — mirroring would be a self-copy. */
        gAuthoringRoot.clear();
    }
}

const fs::path &AssetSystem::GetAuthoringRoot() noexcept
{
    return gAuthoringRoot;
}

PathResult AssetSystem::ResolveUnder(const fs::path &root, std::string_view vpath) noexcept
{
    const PathResult relative = NormalizeVirtualPath(vpath);
    if (!relative)
    {
        return std::unexpected(relative.error());
    }

    std::error_code ec;
    const fs::path absolute = fs::weakly_canonical(root / *relative, ec);
    if (ec)
    {
        return std::unexpected(AssetError::FileReadFailed);
    }

    // Prevent escaping the root. Compare path components rather than string
    // prefixes: a raw starts_with would accept a sibling like "<root>-evil/".
    // lexically_relative yields a path starting with ".." (or empty) when
    // 'absolute' is not contained within the root. Purely lexical — it consults
    // no filesystem, so it has no error_code form and needs none.
    const fs::path rel = absolute.lexically_relative(root);
    if (rel.empty() || *rel.begin() == "..")
    {
        return std::unexpected(AssetError::RootEscape);
    }

    return absolute;
}

PathResult AssetSystem::Resolve(std::string_view vpath) noexcept
{
    if (!IsInitialized())
    {
        return std::unexpected(AssetError::NotInitialized);
    }
    return ResolveUnder(gAssetRoot, vpath);
}

bool AssetSystem::Exists(std::string_view vpath) noexcept
{
    const PathResult resolved = Resolve(vpath);
    if (!resolved)
    {
        return false;
    }
    std::error_code ec;
    return fs::exists(*resolved, ec) && !ec;
}

std::expected<std::string, AssetError> AssetSystem::ReadText(std::string_view vpath) noexcept
{
    const PathResult path = Resolve(vpath);
    if (!path)
    {
        return std::unexpected(path.error());
    }

    const BytesResult bytes = ReadFileBytes(*path);
    if (!bytes)
    {
        return std::unexpected(bytes.error());
    }
    return std::string(reinterpret_cast<const char *>(bytes->data()), bytes->size());
}

BytesResult AssetSystem::ReadBinary(std::string_view vpath) noexcept
{
    const PathResult path = Resolve(vpath);
    if (!path)
    {
        return std::unexpected(path.error());
    }
    return ReadFileBytes(*path);
}

// --- Writable user root -----------------------------------------------------

void AssetSystem::EnsureUserRoot() noexcept
{
    if (gUserRootInitialized)
    {
        return;
    }

    std::error_code ec;
    if (const std::optional<fs::path> env = EnvDir("ASSISI_USER_ROOT"))
    {
        gUserRoot = fs::weakly_canonical(*env, ec);
    }
    else if (const std::optional<fs::path> exe = ExeDir())
    {
        gUserRoot = fs::weakly_canonical(*exe, ec);
    }
    else
    {
        gUserRoot = fs::current_path(ec);
    }

    if (ec)
    {
        /* The process still needs somewhere to write. "." is where it was
           launched from, which is wrong often enough to be worth saying. */
        Log::Warn("AssetSystem: cannot resolve a user-data root ({}); falling back to the working "
                  "directory.",
                  ec.message());
        gUserRoot = fs::path(".");
    }

    gUserRootInitialized = true;
}

VoidResult AssetSystem::SetUserRoot(const fs::path &root) noexcept
{
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec)
    {
        return std::unexpected(AssetError::InvalidRoot);
    }

    fs::path canonical = fs::weakly_canonical(root, ec);
    if (ec)
    {
        Log::Warn("AssetSystem: cannot canonicalize the user root '{}' ({}).", root.string(), ec.message());
        return std::unexpected(AssetError::InvalidRoot);
    }

    gUserRoot            = std::move(canonical);
    gUserRootInitialized = true;

    return {};
}

const fs::path &AssetSystem::GetUserRoot() noexcept
{
    EnsureUserRoot();
    return gUserRoot;
}

std::optional<fs::path> AssetSystem::ExecutablePath() noexcept { return ExePath(); }

PathResult AssetSystem::ResolveUser(std::string_view vpath) noexcept
{
    EnsureUserRoot();
    return ResolveUnder(gUserRoot, vpath);
}

bool AssetSystem::UserExists(std::string_view vpath) noexcept
{
    const PathResult resolved = ResolveUser(vpath);
    if (!resolved)
    {
        return false;
    }
    std::error_code ec;
    return fs::exists(*resolved, ec) && !ec;
}

std::expected<std::string, AssetError> AssetSystem::ReadUserText(std::string_view vpath) noexcept
{
    const PathResult path = ResolveUser(vpath);
    if (!path)
    {
        return std::unexpected(path.error());
    }

    const BytesResult bytes = ReadFileBytes(*path);
    if (!bytes)
    {
        return std::unexpected(bytes.error());
    }
    return std::string(reinterpret_cast<const char *>(bytes->data()), bytes->size());
}

BytesResult AssetSystem::ReadUserBinary(std::string_view vpath) noexcept
{
    const PathResult path = ResolveUser(vpath);
    if (!path)
    {
        return std::unexpected(path.error());
    }
    return ReadFileBytes(*path);
}

VoidResult AssetSystem::WriteText(std::string_view vpath, std::string_view data) noexcept
{
    const PathResult path = ResolveUser(vpath);
    if (!path)
    {
        return std::unexpected(path.error());
    }
    return WriteFileBytes(*path, std::as_bytes(std::span(data.data(), data.size())));
}

VoidResult AssetSystem::WriteBinary(std::string_view vpath, std::span<const std::byte> data) noexcept
{
    const PathResult path = ResolveUser(vpath);
    if (!path)
    {
        return std::unexpected(path.error());
    }
    return WriteFileBytes(*path, data);
}

bool AssetSystem::IsInitialized() noexcept
{
    return gInitialized;
}

PathResult AssetSystem::DiscoverRoot() noexcept
{
    /* Prefer an explicit environment override if present. */
    if (const std::optional<fs::path> env = EnvDir("ASSISI_ASSET_ROOT"))
    {
        return *env;
    }

    /* Helper: walk upward from a starting directory looking for assets/.
       Lambda type is unnameable, so this is the one place auto stays.
       A directory it cannot stat is simply not a match — the walk carries on
       upward rather than abandoning discovery over one unreadable parent. */
    auto walkUp = [](fs::path dir) -> std::optional<fs::path>
                  {
                      for (int32_t i = 0; i < 10; ++i)
                      {
                          std::error_code ec;
                          const fs::path candidate = dir / "assets";
                          if (fs::is_directory(candidate, ec) && !ec)
                          {
                              return candidate;
                          }
                          if (!dir.has_parent_path())
                          {
                              break;
                          }
                          dir = dir.parent_path();
                      }
                      return std::nullopt;
                  };

    /* Walk upward from the executable's directory first — works regardless of CWD. */
    if (const std::optional<fs::path> exe = ExeDir())
    {
        if (const std::optional<fs::path> found = walkUp(*exe))
        {
            return *found;
        }
    }

    /* Fall back to walking upward from the current working directory. */
    std::error_code cwdEc;
    const fs::path cwd = fs::current_path(cwdEc);
    if (!cwdEc)
    {
        if (const std::optional<fs::path> found = walkUp(cwd))
        {
            return *found;
        }
    }
    return std::unexpected(AssetError::RootNotFound);
}

PathResult AssetSystem::NormalizeVirtualPath(std::string_view vpath) noexcept
{
    /* Wholly lexical — nothing here consults the filesystem, so there is no
       error_code overload to reach for. The try is for the fs::path construction,
       which converts a caller-supplied string into the native encoding and throws
       on Windows if it is not valid UTF-8. */
    try
    {
        /* Reject empty, absolute, or drive-qualified paths. */
        std::string str(vpath);
        if (str.empty() || str.front() == '/' || str.find(':') != std::string::npos)
        {
            return std::unexpected(AssetError::InvalidVirtualPath);
        }

        /* Normalize path separators so callers can pass Windows-style paths too. */
        for (char &chr : str)
        {
            if (chr == '\\')
            {
                chr = '/';
            }
        }

        /* Normalize and validate the path components (no parent traversal allowed). */
        const fs::path normalized = fs::path(str).lexically_normal();
        for (const fs::path &part : normalized)
        {
            if (part == "..")
            {
                return std::unexpected(AssetError::InvalidVirtualPath);
            }
        }

        return normalized;
    }
    catch (const std::exception &)
    {
        return std::unexpected(AssetError::InvalidVirtualPath);
    }
}
} // namespace Assisi::Core
