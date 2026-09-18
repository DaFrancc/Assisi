/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPakProvider.cpp
/// @brief A mounted pak serves each slice's exact bytes by id and by path, and
/// refuses every slice or archive it cannot read rather than guessing.
///
/// The paks here are assembled from the format functions directly, so a fault in
/// the provider cannot hide behind the same fault in the writer.

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/PakFormat.hpp>
#include <Assisi/Core/PakProvider.hpp>

using namespace Assisi::Core;

namespace
{

/// One asset to put in a test pak.
struct TestSlice
{
    std::string vpath;
    std::vector<std::byte> bytes;
    PakCodec codec     = PakCodec::None;
    CookedKind kind    = CookedKind::Verbatim;
    std::uint8_t flags = 0;
    std::uint16_t archive = 0;
};

std::vector<std::byte> Pattern(std::size_t length, std::uint8_t seed)
{
    constexpr std::size_t kPeriod = 13;
    std::vector<std::byte> bytes(length);
    for (std::size_t i = 0; i < length; ++i)
    {
        bytes[i] = static_cast<std::byte>(seed + (i % kPeriod));
    }
    return bytes;
}

/// Writes @p slices as a pak at @p path: header, slices, index.
void WriteTestPak(const std::filesystem::path &path, const std::vector<TestSlice> &slices)
{
    BitWriter body;
    std::vector<PakEntry> entries;
    std::uint64_t offset = kPakHeaderBytes;
    for (const TestSlice &slice : slices)
    {
        const std::expected<std::vector<std::byte>, PakCodecError> stored =
            slice.codec < PakCodec::Count ? CompressSlice(slice.codec, slice.bytes)
                                          : std::expected<std::vector<std::byte>, PakCodecError>{slice.bytes};
        REQUIRE(stored.has_value());

        PakEntry entry;
        entry.id               = DerivedAssetId("id:" + slice.vpath);
        entry.pathId           = DerivedAssetId(slice.vpath);
        entry.offset           = offset;
        entry.storedSize       = stored->size();
        entry.uncompressedSize = slice.bytes.size();
        entry.codec            = slice.codec;
        entry.flags            = slice.flags;
        entry.kind             = slice.kind;
        entry.archive          = slice.archive;
        entries.push_back(entry);

        body.WriteBytes(*stored);
        offset += stored->size();
    }

    PakHeader header;
    header.entryCount  = static_cast<std::uint32_t>(entries.size());
    header.indexOffset = offset;

    BitWriter out;
    WritePakHeader(out, header);
    out.WriteBytes(body.Data());
    for (const PakEntry &entry : entries)
    {
        WritePakEntry(out, entry);
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    const std::span<const std::byte> bytes = out.Data();
    file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

/// A pak file in the temp directory that removes itself.
class TempPak
{
public:
    TempPak(const char *name, const std::vector<TestSlice> &slices)
        : _path(std::filesystem::temp_directory_path() / name)
    {
        WriteTestPak(_path, slices);
    }
    ~TempPak()
    {
        std::error_code code;
        std::filesystem::remove(_path, code);
    }
    TempPak(const TempPak &)            = delete;
    TempPak &operator=(const TempPak &) = delete;

    [[nodiscard]] const std::filesystem::path &Path() const { return _path; }

private:
    std::filesystem::path _path;
};

AssetId IdOf(const std::string &vpath)
{
    return DerivedAssetId("id:" + vpath);
}

constexpr std::size_t kSliceBytes = 3000;

} // namespace

TEST_CASE("A mounted pak serves each slice's exact bytes, whatever its codec")
{
    const std::vector<TestSlice> slices{
        {.vpath = "raw.txt", .bytes = Pattern(kSliceBytes, 1), .codec = PakCodec::None},
        {.vpath = "fast.txt", .bytes = Pattern(kSliceBytes, 2), .codec = PakCodec::Lz4},
        {.vpath = "small.txt", .bytes = Pattern(kSliceBytes, 3), .codec = PakCodec::Zstd},
    };
    const TempPak pak("assisi-pak-codecs.pak", slices);

    const std::expected<PakProvider, AssetError> mounted = PakProvider::Mount(pak.Path());
    REQUIRE(mounted.has_value());
    for (const TestSlice &slice : slices)
    {
        CAPTURE(slice.vpath);
        const std::expected<std::vector<std::byte>, AssetError> bytes = mounted->Open(IdOf(slice.vpath));
        REQUIRE(bytes.has_value());
        CHECK(*bytes == slice.bytes);
    }
}

TEST_CASE("A pak resolves a packed path to its id and refuses one it does not hold")
{
    const TempPak pak("assisi-pak-resolve.pak", {{.vpath = "config/game.json", .bytes = Pattern(1, 1)}});
    const std::expected<PakProvider, AssetError> mounted = PakProvider::Mount(pak.Path());
    REQUIRE(mounted.has_value());

    const std::expected<AssetId, AssetError> found = mounted->Resolve("config/game.json");
    REQUIRE(found.has_value());
    CHECK(*found == IdOf("config/game.json"));

    const std::expected<AssetId, AssetError> absent = mounted->Resolve("config/absent.json");
    REQUIRE_FALSE(absent.has_value());
    CHECK(absent.error() == AssetError::UnknownAssetId);
}

TEST_CASE("An id the pak does not hold, or a built-in id, is unknown and nothing else is read")
{
    const TempPak pak("assisi-pak-unknown.pak", {{.vpath = "a.txt", .bytes = Pattern(1, 1)}});
    const std::expected<PakProvider, AssetError> mounted = PakProvider::Mount(pak.Path());
    REQUIRE(mounted.has_value());

    const std::expected<std::vector<std::byte>, AssetError> missing = mounted->Open(IdOf("b.txt"));
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == AssetError::UnknownAssetId);

    const std::expected<std::vector<std::byte>, AssetError> builtin = mounted->Open(BuiltinAssetId::Cube);
    REQUIRE_FALSE(builtin.has_value());
    CHECK(builtin.error() == AssetError::UnknownAssetId);
}

TEST_CASE("A slice this build cannot decode is refused, not handed on as garbage")
{
    const auto unknownCodec = static_cast<PakCodec>(PakCodec::Count);
    const std::vector<TestSlice> slices{
        {.vpath = "encrypted.bin", .bytes = Pattern(kSliceBytes, 1),
         .flags = static_cast<std::uint8_t>(PakSliceFlag::Encrypted)},
        {.vpath = "codec.bin", .bytes = Pattern(kSliceBytes, 2), .codec = unknownCodec},
        {.vpath = "archive.bin", .bytes = Pattern(kSliceBytes, 3), .archive = 1},
        {.vpath = "fine.bin", .bytes = Pattern(kSliceBytes, 4)},
    };
    const TempPak pak("assisi-pak-refused.pak", slices);
    const std::expected<PakProvider, AssetError> mounted = PakProvider::Mount(pak.Path());
    REQUIRE(mounted.has_value());

    for (const char *vpath : {"encrypted.bin", "codec.bin", "archive.bin"})
    {
        CAPTURE(vpath);
        const std::expected<std::vector<std::byte>, AssetError> bytes = mounted->Open(IdOf(vpath));
        REQUIRE_FALSE(bytes.has_value());
        CHECK(bytes.error() == AssetError::UnsupportedEncoding);
    }
    // One unreadable slice does not take the others down with it.
    CHECK(mounted->Open(IdOf("fine.bin")).has_value());
}

TEST_CASE("A pak cut short is refused at mount rather than read past its end")
{
    const TempPak pak("assisi-pak-cut.pak", {{.vpath = "a.txt", .bytes = Pattern(kSliceBytes, 1)}});
    const std::uintmax_t size = std::filesystem::file_size(pak.Path());

    // Short of the index: the header promises entries that are not there.
    std::filesystem::resize_file(pak.Path(), size - 1);
    const std::expected<PakProvider, AssetError> shortIndex = PakProvider::Mount(pak.Path());
    REQUIRE_FALSE(shortIndex.has_value());
    CHECK(shortIndex.error() == AssetError::CorruptArchive);

    // Short of the header.
    std::filesystem::resize_file(pak.Path(), kPakHeaderBytes - 1);
    CHECK_FALSE(PakProvider::Mount(pak.Path()).has_value());
}

TEST_CASE("A file that is not a pak does not mount")
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "assisi-pak-notapak.pak";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "this is a text file and not an archive of any kind";
    }
    CHECK_FALSE(PakProvider::Mount(path).has_value());
    std::error_code code;
    std::filesystem::remove(path, code);
}

TEST_CASE("Workers opening different slices of one pak at once each get their own bytes")
{
    constexpr std::size_t kSlices  = 16;
    constexpr std::size_t kThreads = 8;
    constexpr std::size_t kRounds  = 50;

    std::vector<TestSlice> slices;
    for (std::size_t i = 0; i < kSlices; ++i)
    {
        slices.push_back({.vpath = "slice" + std::to_string(i),
                          .bytes = Pattern(kSliceBytes + i, static_cast<std::uint8_t>(i)),
                          .codec = i % 2 == 0 ? PakCodec::Lz4 : PakCodec::Zstd});
    }
    const TempPak pak("assisi-pak-threads.pak", slices);
    const std::expected<PakProvider, AssetError> mounted = PakProvider::Mount(pak.Path());
    REQUIRE(mounted.has_value());
    const PakProvider &shared = *mounted;

    std::atomic<std::size_t> wrong{0};
    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&, t]()
                             {
                                 for (std::size_t r = 0; r < kRounds; ++r)
                                 {
                                     const TestSlice &slice = slices[(t + r) % kSlices];
                                     const auto bytes       = shared.Open(IdOf(slice.vpath));
                                     if (!bytes || *bytes != slice.bytes)
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

TEST_CASE("A pak lists its slices of one kind")
{
    const std::vector<TestSlice> slices{
        {.vpath = "levels/a.alvl", .bytes = Pattern(1, 1), .kind = CookedKind::Scene},
        {.vpath = "textures/t.png", .bytes = Pattern(1, 2), .kind = CookedKind::Texture},
        {.vpath = "levels/b.alvl", .bytes = Pattern(1, 3), .kind = CookedKind::Scene},
    };
    const TempPak pak("assisi-pak-list.pak", slices);
    const std::expected<PakProvider, AssetError> mounted = PakProvider::Mount(pak.Path());
    REQUIRE(mounted.has_value());

    const std::vector<PakEntry> scenes = mounted->EntriesOfKind(CookedKind::Scene);
    REQUIRE(scenes.size() == 2);
    // By id, so two machines holding the same pak list it in the same order.
    CHECK(scenes[0].id < scenes[1].id);
    for (const PakEntry &entry : scenes)
    {
        CHECK(entry.kind == CookedKind::Scene);
    }
}
