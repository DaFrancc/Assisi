/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestRandomAccessFile.cpp
/// @brief Positioned reads return the bytes at the offset asked for, from any
/// number of threads at once, and never report a short read as success.

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include <Assisi/Core/RandomAccessFile.hpp>

using namespace Assisi::Core;

namespace
{

/// A file whose byte at position i is (i * 31) mod 251, so any read from the
/// wrong offset produces the wrong bytes.
class PatternFile
{
public:
    explicit PatternFile(const char *name, std::size_t length)
        : _path(std::filesystem::temp_directory_path() / name), _bytes(length)
    {
        constexpr std::size_t kMultiplier = 31;
        constexpr std::size_t kModulus    = 251;
        for (std::size_t i = 0; i < length; ++i)
        {
            _bytes[i] = static_cast<std::byte>((i * kMultiplier) % kModulus);
        }
        std::ofstream out(_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(_bytes.data()), static_cast<std::streamsize>(_bytes.size()));
    }

    ~PatternFile()
    {
        std::error_code code;
        std::filesystem::remove(_path, code);
    }

    PatternFile(const PatternFile &)            = delete;
    PatternFile &operator=(const PatternFile &) = delete;

    [[nodiscard]] const std::filesystem::path &Path() const { return _path; }
    /// Whether @p read equals the file's bytes starting at @p offset.
    [[nodiscard]] bool Matches(std::uint64_t offset, const std::vector<std::byte> &read) const
    {
        return std::equal(read.begin(), read.end(), _bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    }

private:
    std::filesystem::path _path;
    std::vector<std::byte> _bytes;
};

constexpr std::size_t kFileBytes = 65536;

} // namespace

TEST_CASE("A positioned read returns the bytes at that offset")
{
    const PatternFile file("assisi-raf-offset.bin", kFileBytes);
    std::expected<RandomAccessFile, AssetError> opened = RandomAccessFile::Open(file.Path());
    REQUIRE(opened.has_value());
    CHECK(opened->Size() == kFileBytes);

    constexpr std::uint64_t kOffset = 12345;
    constexpr std::size_t kLength   = 777;
    std::vector<std::byte> out(kLength);
    REQUIRE(opened->ReadAt(kOffset, out).has_value());
    CHECK(file.Matches(kOffset, out));
}

TEST_CASE("A read past the end of the file fails rather than returning short")
{
    const PatternFile file("assisi-raf-end.bin", kFileBytes);
    std::expected<RandomAccessFile, AssetError> opened = RandomAccessFile::Open(file.Path());
    REQUIRE(opened.has_value());

    std::vector<std::byte> out(2);
    CHECK_FALSE(opened->ReadAt(kFileBytes - 1, out).has_value());
    CHECK_FALSE(opened->ReadAt(kFileBytes + 1, std::span<std::byte>{}).has_value());
    CHECK(opened->ReadAt(kFileBytes, std::span<std::byte>{}).has_value());
}

TEST_CASE("A missing file does not open")
{
    CHECK_FALSE(RandomAccessFile::Open(std::filesystem::temp_directory_path() / "assisi-raf-absent.bin").has_value());
}

TEST_CASE("Concurrent positioned reads from one handle each get their own bytes")
{
    // The case a shared file position gets wrong: two threads seeking and reading
    // interleave, and one gets the other's bytes.
    const PatternFile file("assisi-raf-threads.bin", kFileBytes);
    std::expected<RandomAccessFile, AssetError> opened = RandomAccessFile::Open(file.Path());
    REQUIRE(opened.has_value());
    const RandomAccessFile &shared = *opened;

    constexpr std::size_t kThreads   = 8;
    constexpr std::size_t kReads     = 2000;
    constexpr std::size_t kReadBytes = 97;
    // Two primes, so each thread walks the file in a different order.
    constexpr std::uint64_t kThreadStride = 7919;
    constexpr std::uint64_t kReadStride   = 104729;
    std::atomic<std::size_t> wrong{0};

    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&, t]()
                             {
                                 std::vector<std::byte> out(kReadBytes);
                                 for (std::size_t r = 0; r < kReads; ++r)
                                 {
                                     const std::uint64_t offset =
                                         (t * kThreadStride + r * kReadStride) % (kFileBytes - kReadBytes);
                                     if (!shared.ReadAt(offset, out) || !file.Matches(offset, out))
                                     {
                                         ++wrong;
                                     }
                                 }
                             });
    }
    for (std::thread &thread : threads)
    {
        thread.join();
    }
    CHECK(wrong.load() == 0);
}
