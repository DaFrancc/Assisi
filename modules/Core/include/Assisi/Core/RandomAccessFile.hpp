/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file RandomAccessFile.hpp
/// @brief A read-only file read at explicit offsets, safely from many threads.
///
/// Every read names its own offset and the handle keeps no position, so workers
/// loading different assets from one archive never have to take turns. A stream
/// with a shared position would need a lock around every seek-and-read pair, and
/// forgetting it reads one asset's bytes into another.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>

#include <Assisi/Core/Errors.hpp>

namespace Assisi::Core
{

class RandomAccessFile
{
public:
    /// @brief Open @p path for reading.
    /// @return the file, or FileOpenFailed.
    [[nodiscard]] static std::expected<RandomAccessFile, AssetError> Open(const std::filesystem::path &path);

    ~RandomAccessFile();

    RandomAccessFile(const RandomAccessFile &)            = delete;
    RandomAccessFile &operator=(const RandomAccessFile &) = delete;
    RandomAccessFile(RandomAccessFile &&other) noexcept;
    RandomAccessFile &operator=(RandomAccessFile &&other) noexcept;

    /// @brief Fill @p out with the bytes starting at @p offset.
    ///
    /// Safe to call concurrently. A range that runs past the end of the file is
    /// FileReadFailed with nothing promised about @p out, never a short read
    /// reported as success.
    [[nodiscard]] std::expected<void, AssetError> ReadAt(std::uint64_t offset, std::span<std::byte> out) const;

    /// @brief The file's size in bytes when it was opened.
    [[nodiscard]] std::uint64_t Size() const noexcept { return _size; }

private:
    /// The platform's handle: a file descriptor on POSIX, a HANDLE on Windows.
    using NativeHandle = std::intptr_t;

    RandomAccessFile(NativeHandle handle, std::uint64_t size) noexcept;
    void Close() noexcept;

    std::uint64_t _size   = 0;
    NativeHandle _handle;
};

} // namespace Assisi::Core
