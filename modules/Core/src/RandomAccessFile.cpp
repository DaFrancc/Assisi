/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/RandomAccessFile.hpp>

#include <utility>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <fcntl.h>
#    include <sys/stat.h>
#    include <unistd.h>

#    include <cerrno>
#endif

namespace Assisi::Core
{

namespace
{

#if defined(_WIN32)
const RandomAccessFile::NativeHandle kInvalidHandle = static_cast<std::intptr_t>(-1);
#else
constexpr std::intptr_t kInvalidHandle = -1;
#endif

/// The most one platform read call is asked for. Both platforms cap a single read
/// below the 64-bit range (Windows at 32 bits, Linux near 2 GiB), so a larger
/// range is read in pieces.
constexpr std::size_t kMaxReadChunk = std::size_t{1} << 30U;

} // namespace

RandomAccessFile::RandomAccessFile(NativeHandle handle, std::uint64_t size) noexcept : _size(size), _handle(handle)
{
}

RandomAccessFile::~RandomAccessFile()
{
    Close();
}

RandomAccessFile::RandomAccessFile(RandomAccessFile &&other) noexcept
    : _size(other._size), _handle(std::exchange(other._handle, kInvalidHandle))
{
}

RandomAccessFile &RandomAccessFile::operator=(RandomAccessFile &&other) noexcept
{
    if (this != &other)
    {
        Close();
        _size   = other._size;
        _handle = std::exchange(other._handle, kInvalidHandle);
    }
    return *this;
}

#if defined(_WIN32)

std::expected<RandomAccessFile, AssetError> RandomAccessFile::Open(const std::filesystem::path &path)
{
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return std::unexpected(AssetError::FileOpenFailed);
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(handle, &size) == 0)
    {
        CloseHandle(handle);
        return std::unexpected(AssetError::FileOpenFailed);
    }
    return RandomAccessFile(static_cast<NativeHandle>(reinterpret_cast<std::intptr_t>(handle)),
                            static_cast<std::uint64_t>(size.QuadPart));
}

std::expected<void, AssetError> RandomAccessFile::ReadAt(std::uint64_t offset, std::span<std::byte> out) const
{
    if (offset > _size || out.size() > _size - offset)
    {
        return std::unexpected(AssetError::FileReadFailed);
    }

    const HANDLE handle = reinterpret_cast<HANDLE>(_handle);
    std::size_t done    = 0;
    while (done < out.size())
    {
        const std::size_t chunk = std::min(out.size() - done, kMaxReadChunk);
        const std::uint64_t at  = offset + done;

        // An OVERLAPPED offset on a synchronous handle is a positioned read that
        // does not depend on, or leave behind, a file pointer another thread uses.
        OVERLAPPED overlapped{};
        overlapped.Offset     = static_cast<DWORD>(at & 0xFFFFFFFFULL);
        overlapped.OffsetHigh = static_cast<DWORD>(at >> 32U);

        DWORD read = 0;
        if (ReadFile(handle, out.data() + done, static_cast<DWORD>(chunk), &read, &overlapped) == 0 || read == 0)
        {
            return std::unexpected(AssetError::FileReadFailed);
        }
        done += read;
    }
    return {};
}

void RandomAccessFile::Close() noexcept
{
    if (_handle != kInvalidHandle)
    {
        CloseHandle(reinterpret_cast<HANDLE>(_handle));
        _handle = kInvalidHandle;
    }
}

#else

std::expected<RandomAccessFile, AssetError> RandomAccessFile::Open(const std::filesystem::path &path)
{
    const std::int32_t descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
    {
        return std::unexpected(AssetError::FileOpenFailed);
    }
    struct stat status{};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode))
    {
        ::close(descriptor);
        return std::unexpected(AssetError::FileOpenFailed);
    }
    return RandomAccessFile(descriptor, static_cast<std::uint64_t>(status.st_size));
}

std::expected<void, AssetError> RandomAccessFile::ReadAt(std::uint64_t offset, std::span<std::byte> out) const
{
    if (offset > _size || out.size() > _size - offset)
    {
        return std::unexpected(AssetError::FileReadFailed);
    }

    const auto descriptor = static_cast<std::int32_t>(_handle);
    std::size_t done      = 0;
    while (done < out.size())
    {
        const std::size_t chunk = std::min(out.size() - done, kMaxReadChunk);
        const ssize_t read =
            ::pread(descriptor, out.data() + done, chunk, static_cast<off_t>(offset + done));
        if (read < 0 && errno == EINTR)
        {
            continue;
        }
        // Zero is end of file: the file shrank after it was opened.
        if (read <= 0)
        {
            return std::unexpected(AssetError::FileReadFailed);
        }
        done += static_cast<std::size_t>(read);
    }
    return {};
}

void RandomAccessFile::Close() noexcept
{
    if (_handle != kInvalidHandle)
    {
        ::close(static_cast<std::int32_t>(_handle));
        _handle = kInvalidHandle;
    }
}

#endif

} // namespace Assisi::Core
