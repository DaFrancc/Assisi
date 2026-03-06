/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "Assisi/Core/AssetSystem.hpp"

#include <array>
#include <cstdlib>
#include <fstream>

#ifdef _WIN32
#    include <windows.h>
#else
#    include <unistd.h>
#endif

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
    try
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
        auto absolute = std::filesystem::weakly_canonical(gAssetRoot / *relative);

        /* Prevent escaping the asset root. */
        if (!absolute.generic_string().starts_with(gAssetRoot.generic_string()))
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

bool AssetSystem::Exists(std::string_view vpath) noexcept
{
    try
    {
        /* Resolve the path and check existence. */
        auto resolved = Resolve(vpath);
        if (!resolved)
        {
            return false;
        }

        return std::filesystem::exists(*resolved);
    }
    catch (const std::exception &)
    {
        return false;
    }
}

std::expected<std::string, AssetError> AssetSystem::ReadText(std::string_view vpath) noexcept
{
    try
    {
        /* Resolve the asset path. */
        auto path = Resolve(vpath);
        if (!path)
        {
            return std::unexpected(path.error());
        }

        /* Open at end so we can size the buffer in one pass. */
        std::ifstream file(*path, std::ios::binary | std::ios::ate);
        if (!file)
        {
            return std::unexpected(AssetError::FileOpenFailed);
        }

        /* tellg() can fail (e.g., non-seekable streams); treat as read failure. */
        const auto end = file.tellg();
        if (end < 0)
        {
            return std::unexpected(AssetError::FileReadFailed);
        }

        /* Allocate exact size, then seek back and read. */
        std::string data(static_cast<size_t>(end), '\0');

        file.seekg(0);
        file.read(data.data(), static_cast<std::streamsize>(data.size()));

        /* For text reads we expect a full read; any stream failure is an error. */
        if (!file.good())
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

std::expected<std::vector<std::byte>, AssetError> AssetSystem::ReadBinary(std::string_view vpath) noexcept
{
    try
    {
        /* Resolve the asset path. */
        auto path = Resolve(vpath);
        if (!path)
        {
            return std::unexpected(path.error());
        }

        /* Open the file and seek to the end to determine size. */
        std::ifstream file(*path, std::ios::binary | std::ios::ate);
        if (!file)
        {
            return std::unexpected(AssetError::FileOpenFailed);
        }

        /* Allocate the buffer and read the file. */
        const auto size = static_cast<size_t>(file.tellg());
        file.seekg(0);

        std::vector<std::byte> data(size);
        file.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(size));

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

bool AssetSystem::IsInitialized() noexcept
{
    return gInitialized;
}

std::expected<std::filesystem::path, AssetError> AssetSystem::DiscoverRoot() noexcept
{
    try
    {
        /* Prefer an explicit environment override if present. */
#ifdef _WIN32
        char *env = nullptr;
        size_t len = 0;

        /* _dupenv_s allocates; we must free env when present. */
        if (_dupenv_s(&env, &len, "ASSISI_ASSET_ROOT") == 0 && env != nullptr)
        {
            std::filesystem::path envPath(env);
            free(env);

            if (std::filesystem::is_directory(envPath))
            {
                return envPath;
            }
        }
#else
        if (const char *env = std::getenv("ASSISI_ASSET_ROOT"))
        {
            std::filesystem::path envPath(env);
            if (std::filesystem::is_directory(envPath))
            {
                return envPath;
            }
        }
#endif

        /* Helper: walk upward from a starting directory looking for assets/. */
        auto walkUp = [](std::filesystem::path dir) -> std::optional<std::filesystem::path>
        {
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
            return std::nullopt;
        };

        /* Walk upward from the executable's directory first — works regardless of CWD. */
#ifdef _WIN32
        std::array<wchar_t, MAX_PATH> exeBuf = {};
        if (GetModuleFileNameW(nullptr, exeBuf.data(), MAX_PATH) != 0)
        {
            if (auto found = walkUp(std::filesystem::path(exeBuf.data()).parent_path()))
            {
                return *found;
            }
        }
#else
        std::array<char, 4096> exeBuf = {};
        const ssize_t exeLen = readlink("/proc/self/exe", exeBuf.data(), exeBuf.size() - 1);
        if (exeLen > 0)
        {
            if (auto found = walkUp(std::filesystem::path(exeBuf.data()).parent_path()))
            {
                return *found;
            }
        }
#endif

        /* Fall back to walking upward from the current working directory. */
        if (auto found = walkUp(std::filesystem::current_path()))
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

std::expected<std::filesystem::path, AssetError> AssetSystem::NormalizeVirtualPath(std::string_view vpath) noexcept
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
        auto normalized = std::filesystem::path(str).lexically_normal();
        for (const auto &part : normalized)
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