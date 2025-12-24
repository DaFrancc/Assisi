/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "Assisi/Core/AssetSystem.hpp"

#include <cstdlib>
#include <fstream>

namespace Assisi::Core
{
/* Global asset root cached after initialization. */
static std::filesystem::path gAssetRoot;

/* Tracks whether the system has been initialized. */
static bool gInitialized = false;

std::expected<void, AssetError> AssetSystem::Initialize() noexcept
{
    /* Allow repeated initialization calls. */
    if (gInitialized)
    {
        return {};
    }

    /* Attempt automatic root discovery. */
    auto root = DiscoverRoot();
    if (!root)
    {
        return std::unexpected(root.error());
    }

    /* Canonicalize and store the root. */
    gAssetRoot = std::filesystem::weakly_canonical(*root);
    gInitialized = true;

    return {};
}

std::expected<void, AssetError> AssetSystem::SetRoot(const std::filesystem::path &root) noexcept
{
    /* Validate that the root exists and is a directory. */
    if (!std::filesystem::is_directory(root))
    {
        return std::unexpected(AssetError::InvalidRoot);
    }

    /* Canonicalize and store the root. */
    gAssetRoot = std::filesystem::weakly_canonical(root);
    gInitialized = true;

    return {};
}

const std::filesystem::path &AssetSystem::GetRoot() noexcept
{
    /* Preconditions: AssetSystem is initialized. */
    return gAssetRoot;
}

std::expected<std::filesystem::path, AssetError> AssetSystem::Resolve(std::string_view vpath) noexcept
{
    /* Ensure the system has been initialized. */
    if (!IsInitialized())
    {
        return std::unexpected(AssetError::NotInitialized);
    }

    /* Normalize and validate the virtual path. */
    auto relative = NormalizeVirtualPath(vpath);
    if (!relative)
    {
        return std::unexpected(relative.error());
    }

    /* Resolve and canonicalize the absolute path. */
    const auto absolute = std::filesystem::weakly_canonical(gAssetRoot / *relative);

    /* Prevent escaping the asset root. */
    if (absolute.generic_string().rfind(gAssetRoot.generic_string(), 0) != 0)
    {
        return std::unexpected(AssetError::RootEscape);
    }

    return absolute;
}

bool AssetSystem::Exists(std::string_view vpath) noexcept
{
    /* Resolve the path and check existence. */
    auto resolved = Resolve(vpath);
    if (!resolved)
    {
        return false;
    }

    return std::filesystem::exists(*resolved);
}

std::expected<std::string, AssetError> AssetSystem::ReadText(std::string_view vpath) noexcept
{
    /* Resolve the asset path. */
    auto path = Resolve(vpath);
    if (!path)
    {
        return std::unexpected(path.error());
    }

    /* Open at end so we can size the buffer in one pass. */
    std::ifstream in(*path, std::ios::binary | std::ios::ate);
    if (!in)
    {
        return std::unexpected(AssetError::FileOpenFailed);
    }

    /* tellg() can fail (e.g., non-seekable streams); treat as read failure. */
    const auto end = in.tellg();
    if (end < 0)
    {
        return std::unexpected(AssetError::FileReadFailed);
    }

    /* Allocate exact size, then seek back and read. */
    std::string data(static_cast<size_t>(end), '\0');

    in.seekg(0);
    in.read(data.data(), static_cast<std::streamsize>(data.size()));

    /* For text reads we expect a full read; any stream failure is an error. */
    if (!in.good())
    {
        return std::unexpected(AssetError::FileReadFailed);
    }

    return data;
}

std::expected<std::vector<std::byte>, AssetError> AssetSystem::ReadBinary(std::string_view vpath) noexcept
{
    /* Resolve the asset path. */
    auto path = Resolve(vpath);
    if (!path)
    {
        return std::unexpected(path.error());
    }

    /* Open the file and seek to the end to determine size. */
    std::ifstream in(*path, std::ios::binary | std::ios::ate);
    if (!in)
    {
        return std::unexpected(AssetError::FileOpenFailed);
    }

    /* Allocate the buffer and read the file. */
    const auto size = static_cast<size_t>(in.tellg());
    in.seekg(0);

    std::vector<std::byte> data(size);
    in.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(size));

    /* Detect read failures (EOF is acceptable exactly at end-of-file). */
    if (!in.good() && !in.eof())
    {
        return std::unexpected(AssetError::FileReadFailed);
    }

    return data;
}

bool AssetSystem::IsInitialized() noexcept
{
    return gInitialized;
}

std::expected<std::filesystem::path, AssetError> AssetSystem::DiscoverRoot() noexcept
{
    /* Prefer an explicit environment override if present. */
#if defined(_WIN32)
    char *env = nullptr;
    size_t len = 0;

    /* _dupenv_s allocates; we must free env when present. */
    if (_dupenv_s(&env, &len, "ASSISI_ASSET_ROOT") == 0 && env != nullptr)
    {
        std::filesystem::path p(env);
        free(env);

        if (std::filesystem::is_directory(p))
        {
            return p;
        }
    }
#else
    if (const char *env = std::getenv("ASSISI_ASSET_ROOT"))
    {
        std::filesystem::path p(env);
        if (std::filesystem::is_directory(p))
        {
            return p;
        }
    }
#endif

    /* Walk upward from the current working directory. */
    auto dir = std::filesystem::current_path();
    for (int i = 0; i < 10; ++i)
    {
        auto candidate = dir / "assets";
        if (std::filesystem::is_directory(candidate))
        {
            return candidate;
        }

        if (!dir.has_parent_path())
        {
            break;
        }

        dir = dir.parent_path();
    }

    return std::unexpected(AssetError::RootNotFound);
}

std::expected<std::filesystem::path, AssetError> AssetSystem::NormalizeVirtualPath(std::string_view vpath) noexcept
{
    /* Reject empty, absolute, or drive-qualified paths. */
    std::string s(vpath);
    if (s.empty() || s.front() == '/' || s.find(':') != std::string::npos)
    {
        return std::unexpected(AssetError::InvalidVirtualPath);
    }

    /* Normalize path separators so callers can pass Windows-style paths too. */
    for (char &c : s)
    {
        if (c == '\\')
        {
            c = '/';
        }
    }

    /* Normalize and validate the path components (no parent traversal allowed). */
    auto p = std::filesystem::path(s).lexically_normal();
    for (const auto &part : p)
    {
        if (part == "..")
        {
            return std::unexpected(AssetError::InvalidVirtualPath);
        }
    }

    return p;
}
} // namespace Assisi::Core