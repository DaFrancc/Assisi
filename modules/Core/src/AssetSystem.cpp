/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "Assisi/Core/AssetSystem.hpp"

#include "Assisi/Core/Logger.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace Assisi::Core
{
namespace fs = std::filesystem;

std::string_view ToString(AssetErrorCode code) noexcept
{
    switch (code)
    {
    case AssetErrorCode::NotInitialized:
        return "the asset system is not initialized";
    case AssetErrorCode::RootNotFound:
        return "no asset root was found";
    case AssetErrorCode::InvalidRoot:
        return "the asset root is not a directory";
    case AssetErrorCode::InvalidVirtualPath:
        return "the path is empty, absolute, or climbs out with '..'";
    case AssetErrorCode::RootEscape:
        return "the path leaves the asset root";
    case AssetErrorCode::FileNotFound:
        return "the file does not exist";
    case AssetErrorCode::IsDirectory:
        return "the path is a directory, not a file";
    case AssetErrorCode::PermissionDenied:
        return "permission to open the file is denied";
    case AssetErrorCode::FileOpenFailed:
        return "the file could not be opened";
    case AssetErrorCode::FileReadFailed:
        return "the file could not be read";
    case AssetErrorCode::FileWriteFailed:
        return "the file could not be written";
    case AssetErrorCode::UnknownAssetId:
        return "no asset has that id";
    case AssetErrorCode::UnsupportedEncoding:
        return "the data is stored in a form this build cannot read";
    case AssetErrorCode::CorruptAsset:
        return "the file's contents are corrupt";
    case AssetErrorCode::CorruptArchive:
        return "the package is corrupt";
    case AssetErrorCode::WrongType:
        return "the asset was asked for as a type it does not load as";
    case AssetErrorCode::Count_:
        break;
    }
    return "unknown";
}

std::string ToString(const AssetError &error)
{
    if (error.detail.empty())
    {
        return std::string{ToString(error.code)};
    }
    return std::format("{} ({})", ToString(error.code), error.detail);
}

/// Every `std::filesystem` call in this file goes through the `std::error_code`
/// overload rather than the throwing one. They are the same functions — the ec
/// form is `noexcept` and reports by value — and choosing it is what makes the
/// `noexcept` on every function here true rather than aspirational.
///
/// Getting this wrong terminates: a `noexcept` function calling the throwing
/// `weakly_canonical` / `is_directory` turns a permission error or an unmounted
/// volume into `std::terminate` rather than a bad return.
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

/* Why a file that would not open did not: asked of the filesystem afterwards,
   because a stream that fails to open says only that it failed. */
AssetErrorCode WhyUnopenable(const fs::path &path) noexcept
{
    std::error_code ec;
    const fs::file_status status = fs::status(path, ec);
    if (ec == std::errc::permission_denied)
    {
        return AssetErrorCode::PermissionDenied;
    }
    switch (status.type())
    {
    case fs::file_type::not_found:
        return AssetErrorCode::FileNotFound;
    case fs::file_type::directory:
        return AssetErrorCode::IsDirectory;
    case fs::file_type::regular:
        // It is there and it is a file, so the open was refused.
        return AssetErrorCode::PermissionDenied;
    default:
        return AssetErrorCode::FileOpenFailed;
    }
}

/* Reads an entire file into a byte buffer sized in one seek-to-end pass. */
BytesResult ReadFileBytes(const fs::path &path) noexcept
{
    try
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
        {
            return std::unexpected(WhyUnopenable(path));
        }
        /* Some libstdc++ versions open a directory as though it were a file and
           put its end at the largest offset there is. */
        std::error_code ec;
        if (fs::is_directory(path, ec))
        {
            return std::unexpected(AssetErrorCode::IsDirectory);
        }

        /* tellg() can fail (e.g., non-seekable streams); treat as read failure. */
        const std::streampos end = file.tellg();
        if (end < 0)
        {
            return std::unexpected(AssetErrorCode::FileReadFailed);
        }

        std::vector<std::byte> data(static_cast<size_t>(end));
        file.seekg(0);
        file.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));

        /* Detect read failures (EOF is acceptable exactly at end-of-file). */
        if (!file.good() && !file.eof())
        {
            return std::unexpected(AssetErrorCode::FileReadFailed);
        }

        return data;
    }
    catch (const std::exception &e)
    {
        Log::Warn("AssetSystem: exception reading '{}': {}", path.string(), e.what());
        return std::unexpected(AssetErrorCode::FileReadFailed);
    }
}

/* The name a write is staged under before it takes the target's place: beside
   the target, so the rename stays on one filesystem, and always the same, so
   the next save overwrites one a crash left behind. */
fs::path StagingPathFor(const fs::path &path)
{
    fs::path staging = path;
    staging += ".tmp";
    return staging;
}

#ifdef _WIN32
/* The most handed to one WriteFile call, whose length is a DWORD. */
constexpr std::size_t kMostBytesPerWrite = std::size_t{1} << 30;

/* Writes all of data to path, truncating it, and has it on the disk before it
   closes it; false if any of it failed. A rename of a file only part on the
   disk could, after a power cut, leave the target holding that part. */
bool WriteDurably(const fs::path &path, std::span<const std::byte> data)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    bool written = true;
    for (std::size_t at = 0; written && at < data.size();)
    {
        const DWORD chunk = static_cast<DWORD>(std::min(data.size() - at, kMostBytesPerWrite));
        DWORD wrote = 0;
        written = WriteFile(file, data.data() + at, chunk, &wrote, nullptr) != 0 && wrote == chunk;
        at += wrote;
    }
    const bool flushed = written && FlushFileBuffers(file) != 0;
    const bool closed = CloseHandle(file) != 0;
    return flushed && closed;
}

/* Puts staging in path's place in one step, on the disk before it returns. */
bool ReplaceDurably(const fs::path &staging, const fs::path &path, std::error_code &ec)
{
    if (MoveFileExW(staging.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0)
    {
        ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
    }
    return true;
}
#else
/* Writes all of data to path, truncating it, and has it on the disk before it
   closes it; false if any of it failed. A rename of a file only part on the
   disk could, after a power cut, leave the target holding that part. */
bool WriteDurably(const fs::path &path, std::span<const std::byte> data)
{
    /* The mode umask trims, as an ofstream's would be. */
    constexpr mode_t kNewFileMode = 0666;
    const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, kNewFileMode);
    if (file < 0)
    {
        return false;
    }
    bool written = true;
    for (std::size_t at = 0; written && at < data.size();)
    {
        const ssize_t wrote = ::write(file, data.data() + at, data.size() - at);
        if (wrote < 0)
        {
            written = errno == EINTR;
            continue;
        }
        at += static_cast<std::size_t>(wrote);
    }
    const bool synced = written && ::fsync(file) == 0;
    const bool closed = ::close(file) == 0;
    return synced && closed;
}

/* Has the entries of dir on the disk: a rename is a change to the directory
   holding the file, and is not kept through a power cut until it is synced. */
void SyncDirectory(const fs::path &dir)
{
    const int handle = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (handle < 0)
    {
        Log::Warn("AssetSystem: cannot open '{}' to keep a save through a power cut.", dir.string());
        return;
    }
    /* EINVAL is a filesystem that keeps no directory to sync, which has
       nothing more to do. */
    if (::fsync(handle) != 0 && errno != EINVAL)
    {
        Log::Warn("AssetSystem: cannot sync '{}'; a save there may not survive a power cut.", dir.string());
    }
    ::close(handle);
}

/* Puts staging in path's place in one step, on the disk before it returns. */
bool ReplaceDurably(const fs::path &staging, const fs::path &path, std::error_code &ec)
{
    fs::rename(staging, path, ec);
    if (ec)
    {
        return false;
    }
    SyncDirectory(path.has_parent_path() ? path.parent_path() : fs::path{"."});
    return true;
}
#endif

/* Removes a staging file a failed write made, if it made one. */
void RemoveStaging(const fs::path &staging) noexcept
{
    std::error_code ec;
    fs::remove(staging, ec);
}

/* Writes bytes to path, creating parent directories. The bytes go to a staging
   file first, on the disk in full before it is renamed over the target and the
   rename on the disk before this returns, so the file is always the whole old
   contents or the whole new ones: a crash, a power cut or a failure part-way
   leaves the target as it was. */
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
            Log::Warn("AssetSystem: cannot create the directory for '{}' ({}).", path.string(), ec.message());
            return std::unexpected(AssetErrorCode::FileWriteFailed);
        }
    }

    /* The try covers the path below, which can throw from its own allocations. */
    fs::path staging;
    try
    {
        staging = StagingPathFor(path);
        if (!WriteDurably(staging, data))
        {
            RemoveStaging(staging);
            return std::unexpected(AssetErrorCode::FileWriteFailed);
        }
    }
    catch (const std::exception &e)
    {
        Log::Warn("AssetSystem: exception writing '{}': {}", path.string(), e.what());
        RemoveStaging(staging);
        return std::unexpected(AssetErrorCode::FileWriteFailed);
    }

    std::error_code ec;
    if (!ReplaceDurably(staging, path, ec))
    {
        Log::Warn("AssetSystem: cannot put '{}' in place ({}); it is left as it was.", path.string(), ec.message());
        RemoveStaging(staging);
        return std::unexpected(AssetErrorCode::FileWriteFailed);
    }
    return {};
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
        return std::unexpected(AssetErrorCode::InvalidRoot);
    }

    gAssetRoot = std::move(canonical);
    gInitialized = true;

    return {};
}

VoidResult AssetSystem::SetRoot(const fs::path &root) noexcept
{
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec)
    {
        return std::unexpected(AssetErrorCode::InvalidRoot);
    }

    fs::path canonical = fs::weakly_canonical(root, ec);
    if (ec)
    {
        Log::Warn("AssetSystem: cannot canonicalize '{}' ({}).", root.string(), ec.message());
        return std::unexpected(AssetErrorCode::InvalidRoot);
    }

    gAssetRoot = std::move(canonical);
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
    const PathResult relative = NormalizeVirtualPath(vpath);
    if (!relative)
    {
        return std::unexpected(relative.error());
    }

    std::error_code ec;
    const fs::path absolute = fs::weakly_canonical(root / *relative, ec);
    if (ec)
    {
        return std::unexpected(AssetErrorCode::FileReadFailed);
    }

    // Prevent escaping the root. Compare path components rather than string
    // prefixes: a raw starts_with would accept a sibling like "<root>-evil/".
    // lexically_relative yields a path starting with ".." (or empty) when
    // 'absolute' is not contained within the root. Purely lexical — it consults
    // no filesystem, so it has no error_code form and needs none.
    const fs::path rel = absolute.lexically_relative(root);
    if (rel.empty() || *rel.begin() == "..")
    {
        return std::unexpected(AssetErrorCode::RootEscape);
    }

    return absolute;
}

PathResult AssetSystem::Resolve(std::string_view vpath) noexcept
{
    if (!IsInitialized())
    {
        return std::unexpected(AssetErrorCode::NotInitialized);
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
        return std::unexpected(AssetErrorCode::InvalidRoot);
    }

    fs::path canonical = fs::weakly_canonical(root, ec);
    if (ec)
    {
        Log::Warn("AssetSystem: cannot canonicalize the user root '{}' ({}).", root.string(), ec.message());
        return std::unexpected(AssetErrorCode::InvalidRoot);
    }

    gUserRoot = std::move(canonical);
    gUserRootInitialized = true;

    return {};
}

const fs::path &AssetSystem::GetUserRoot() noexcept
{
    EnsureUserRoot();
    return gUserRoot;
}

std::optional<fs::path> AssetSystem::ExecutablePath() noexcept
{
    return ExePath();
}

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
    /* How far up to look before giving up. A developer build runs the executable
       from inside the build tree and the tree it must find is the repository's
       own, so the walk has to span that depth: a test binary sits six directories
       below the root (out/build/<preset>/modules/<module>/tests) and so needs
       seven probes. The margin above that costs a handful of stats on paths that
       do not exist. */
    constexpr int32_t kMaxWalkUpDepth = 10;

    auto walkUp = [](fs::path dir) -> std::optional<fs::path>
    {
        for (int32_t i = 0; i < kMaxWalkUpDepth; ++i)
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
    return std::unexpected(AssetErrorCode::RootNotFound);
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
            return std::unexpected(AssetErrorCode::InvalidVirtualPath);
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
                return std::unexpected(AssetErrorCode::InvalidVirtualPath);
            }
        }

        return normalized;
    }
    catch (const std::exception &)
    {
        return std::unexpected(AssetErrorCode::InvalidVirtualPath);
    }
}
} // namespace Assisi::Core
