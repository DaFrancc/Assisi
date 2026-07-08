/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "Assisi/Core/AssetSystem.hpp"

#include <array>
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

// Shortened spellings for the deeply-scoped result types this file returns.
using PathResult = std::expected<fs::path, AssetError>;
using BytesResult = std::expected<std::vector<std::byte>, AssetError>;
using VoidResult = std::expected<void, AssetError>;

/* Read-only asset root, cached after Initialize()/SetRoot(). */
static fs::path gAssetRoot;
static bool gInitialized = false;

/* Writable user-data root, cached on first use (lazy; see EnsureUserRoot). */
static fs::path gUserRoot;
static bool gUserRootInitialized = false;

namespace
{
/* Absolute directory containing the running executable, if it can be found.
   CWD-independent, so both root discovery and the user-root default anchor
   here rather than on wherever the process happened to be launched from. */
std::optional<fs::path> ExeDir() noexcept
{
    try
    {
#ifdef _WIN32
        std::array<wchar_t, MAX_PATH> buf = {};
        if (GetModuleFileNameW(nullptr, buf.data(), MAX_PATH) != 0)
        {
            return fs::path(buf.data()).parent_path();
        }
#else
        std::array<char, 4096> buf = {};
        const ssize_t len = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
        if (len > 0)
        {
            return fs::path(std::string(buf.data(), static_cast<size_t>(len))).parent_path();
        }
#endif
    }
    catch (const std::exception &)
    {
    }
    return std::nullopt;
}

/* Reads an ASSISI_* env var as a path, returning it only when it names a directory. */
std::optional<fs::path> EnvDir(const char *name) noexcept
{
    try
    {
#ifdef _WIN32
        char *env = nullptr;
        size_t len = 0;
        /* _dupenv_s allocates; we must free env when present. */
        if (_dupenv_s(&env, &len, name) == 0 && env != nullptr)
        {
            fs::path envPath(env);
            free(env);
            if (fs::is_directory(envPath))
            {
                return envPath;
            }
        }
#else
        if (const char *env = std::getenv(name))
        {
            fs::path envPath(env);
            if (fs::is_directory(envPath))
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
    catch (const std::exception &)
    {
        return std::unexpected(AssetError::FileReadFailed);
    }
}

/* Writes bytes to path, creating parent directories and truncating any existing file. */
VoidResult WriteFileBytes(const fs::path &path, std::span<const std::byte> data) noexcept
{
    try
    {
        if (path.has_parent_path())
        {
            fs::create_directories(path.parent_path());
        }

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
    catch (const std::exception &)
    {
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

    gAssetRoot = fs::weakly_canonical(*root);
    gInitialized = true;

    return {};
}

VoidResult AssetSystem::SetRoot(const fs::path &root) noexcept
{
    if (!fs::is_directory(root))
    {
        return std::unexpected(AssetError::InvalidRoot);
    }

    gAssetRoot = fs::weakly_canonical(root);
    gInitialized = true;

    return {};
}

const fs::path &AssetSystem::GetRoot() noexcept
{
    /* Preconditions: AssetSystem is initialized. */
    return gAssetRoot;
}

PathResult AssetSystem::ResolveUnder(const fs::path &root, std::string_view vpath) noexcept
{
    try
    {
        const PathResult relative = NormalizeVirtualPath(vpath);
        if (!relative)
        {
            return std::unexpected(relative.error());
        }

        const fs::path absolute = fs::weakly_canonical(root / *relative);

        // Prevent escaping the root. Compare path components rather than string
        // prefixes: a raw starts_with would accept a sibling like "<root>-evil/".
        // lexically_relative yields a path starting with ".." (or empty) when
        // 'absolute' is not contained within the root.
        const fs::path rel = absolute.lexically_relative(root);
        if (rel.empty() || *rel.begin() == "..")
        {
            return std::unexpected(AssetError::RootEscape);
        }

        return absolute;
    }
    catch (const std::exception &)
    {
        return std::unexpected(AssetError::FileReadFailed);
    }
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
    try
    {
        const PathResult resolved = Resolve(vpath);
        return resolved && fs::exists(*resolved);
    }
    catch (const std::exception &)
    {
        return false;
    }
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

    try
    {
        if (const std::optional<fs::path> env = EnvDir("ASSISI_USER_ROOT"))
        {
            gUserRoot = fs::weakly_canonical(*env);
        }
        else if (const std::optional<fs::path> exe = ExeDir())
        {
            gUserRoot = fs::weakly_canonical(*exe);
        }
        else
        {
            gUserRoot = fs::current_path();
        }
    }
    catch (const std::exception &)
    {
        gUserRoot = fs::path(".");
    }

    gUserRootInitialized = true;
}

VoidResult AssetSystem::SetUserRoot(const fs::path &root) noexcept
{
    if (!fs::is_directory(root))
    {
        return std::unexpected(AssetError::InvalidRoot);
    }

    gUserRoot = fs::weakly_canonical(root);
    gUserRootInitialized = true;

    return {};
}

const fs::path &AssetSystem::GetUserRoot() noexcept
{
    EnsureUserRoot();
    return gUserRoot;
}

PathResult AssetSystem::ResolveUser(std::string_view vpath) noexcept
{
    EnsureUserRoot();
    return ResolveUnder(gUserRoot, vpath);
}

bool AssetSystem::UserExists(std::string_view vpath) noexcept
{
    try
    {
        const PathResult resolved = ResolveUser(vpath);
        return resolved && fs::exists(*resolved);
    }
    catch (const std::exception &)
    {
        return false;
    }
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
    try
    {
        /* Prefer an explicit environment override if present. */
        if (const std::optional<fs::path> env = EnvDir("ASSISI_ASSET_ROOT"))
        {
            return *env;
        }

        /* Helper: walk upward from a starting directory looking for assets/.
           Lambda type is unnameable, so this is the one place auto stays. */
        auto walkUp = [](fs::path dir) -> std::optional<fs::path>
        {
            for (int i = 0; i < 10; ++i)
            {
                const fs::path candidate = dir / "assets";
                if (fs::is_directory(candidate))
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
        if (const std::optional<fs::path> found = walkUp(fs::current_path()))
        {
            return *found;
        }

        return std::unexpected(AssetError::RootNotFound);
    }
    catch (const std::exception &)
    {
        return std::unexpected(AssetError::RootNotFound);
    }
}

PathResult AssetSystem::NormalizeVirtualPath(std::string_view vpath) noexcept
{
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
