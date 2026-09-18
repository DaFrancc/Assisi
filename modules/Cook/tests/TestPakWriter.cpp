/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPakWriter.cpp
/// @brief Packing a cooked tree: exactly the manifest's assets, readable back
/// through a mounted pak, byte-identical from one run to the next, and laid out
/// over the previous release's pak so unchanged slices do not move.

#include <doctest/doctest.h>

#include <ostream>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <Assisi/Cook/CookTree.hpp>
#include <Assisi/Cook/PakWriter.hpp>
#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Core/PakFormat.hpp>
#include <Assisi/Core/PakProvider.hpp>

using Assisi::Cook::CookError;
using Assisi::Cook::CookReport;
using Assisi::Cook::CookTree;
using Assisi::Cook::ManifestEntry;
using Assisi::Cook::PakLayout;
using Assisi::Cook::PakReport;
using Assisi::Cook::WritePak;
using Assisi::Core::AssetError;
using Assisi::Core::AssetId;
using Assisi::Core::PakCodec;
using Assisi::Core::PakProvider;

namespace
{

/// A scratch directory that cleans itself up.
class ScratchDir
{
public:
    explicit ScratchDir(std::string_view name)
        : _path(std::filesystem::temp_directory_path() / ("assisi-pak-test-" + std::string{name}))
    {
        std::error_code code;
        std::filesystem::remove_all(_path, code);
        std::filesystem::create_directories(_path, code);
    }
    ~ScratchDir()
    {
        std::error_code code;
        std::filesystem::remove_all(_path, code);
    }
    ScratchDir(const ScratchDir &)            = delete;
    ScratchDir &operator=(const ScratchDir &) = delete;

    [[nodiscard]] const std::filesystem::path &Path() const { return _path; }

private:
    std::filesystem::path _path;
};

std::vector<char> ReadFile(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string Explain(const std::expected<PakReport, CookError> &result)
{
    return result ? std::string{} : result.error().vpath + ": " + result.error().reason;
}

/// Cooks the fixture tree into @p out at the fast tier and returns its manifest.
std::vector<ManifestEntry> CookFixture(const std::filesystem::path &out)
{
    const std::expected<CookReport, CookError> report =
        CookTree(ASSISI_COOK_FIXTURE_ROOT, out, Assisi::Image::CompressQuality::Fast);
    REQUIRE(report.has_value());
    REQUIRE_FALSE(report->entries.empty());
    return report->entries;
}

constexpr std::uint64_t kKiB = 1024;
constexpr std::uint64_t kMiB = kKiB * kKiB;

/// Writes a cooked blob of exactly @p totalBytes, header included, under @p cookedRoot
/// and returns the manifest row naming it. The payload depends on @p vpath, so no
/// two blobs share bytes and a chunk's content says which slices it holds.
ManifestEntry WriteBlob(const std::filesystem::path &cookedRoot, const std::string &vpath, std::uint64_t totalBytes)
{
    Assisi::Core::BitWriter writer;
    Assisi::Core::WriteCookedHeader(writer, Assisi::Core::CookedKind::Verbatim);
    const std::uint64_t headerBytes = writer.Data().size();
    REQUIRE(totalBytes >= headerBytes);

    // An odd stride walks every byte value, so a chunk of payload is never all one byte.
    constexpr std::uint64_t kPatternStride = 131;

    std::vector<std::byte> payload(static_cast<std::size_t>(totalBytes - headerBytes));
    const std::uint64_t seed = std::hash<std::string>{}(vpath);
    for (std::size_t i = 0; i < payload.size(); ++i)
    {
        payload[i] = static_cast<std::byte>(static_cast<std::uint8_t>(seed + i * kPatternStride));
    }
    writer.WriteBytes(payload);

    const std::string guid = Assisi::Core::DerivedAssetId(vpath).ToString();
    std::ofstream out(cookedRoot / (guid + ".cooked"), std::ios::binary | std::ios::trunc);
    const std::span<const std::byte> bytes = writer.Data();
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return ManifestEntry{.vpath = vpath, .guid = guid};
}

/// Every index row of the pak at @p path, in index order.
std::vector<Assisi::Core::PakEntry> ReadIndex(const std::filesystem::path &path)
{
    const std::vector<char> chars = ReadFile(path);
    const std::span<const std::byte> bytes = std::as_bytes(std::span{chars});
    const std::expected<Assisi::Core::PakHeader, Assisi::Core::PakFormatError> header =
        Assisi::Core::ReadPakHeader(bytes);
    REQUIRE(header.has_value());

    Assisi::Core::BitReader reader{bytes.subspan(static_cast<std::size_t>(header->indexOffset))};
    std::vector<Assisi::Core::PakEntry> index;
    for (std::uint32_t i = 0; i < header->entryCount; ++i)
    {
        const std::expected<Assisi::Core::PakEntry, Assisi::Core::PakFormatError> entry =
            Assisi::Core::ReadPakEntry(reader);
        REQUIRE(entry.has_value());
        index.push_back(*entry);
    }
    return index;
}

/// The chunk a storefront's delta updater cuts a file into, at fixed offsets.
constexpr std::uint64_t kChunkBytes = kMiB;

/// The pak at @p path cut into chunks, as the set of their contents: what a
/// player who has that pak already holds.
std::set<std::string> Chunks(const std::filesystem::path &path)
{
    const std::vector<char> chars = ReadFile(path);
    std::set<std::string> chunks;
    for (std::size_t start = 0; start < chars.size(); start += kChunkBytes)
    {
        const std::size_t length = std::min<std::size_t>(kChunkBytes, chars.size() - start);
        chunks.emplace(chars.data() + start, length);
    }
    return chunks;
}

/// Chunks of @p after that @p before does not hold anywhere: what an update ships.
std::size_t NewChunks(const std::filesystem::path &before, const std::filesystem::path &after)
{
    const std::set<std::string> held = Chunks(before);
    return static_cast<std::size_t>(
        std::ranges::count_if(Chunks(after), [&held](const std::string &chunk) { return !held.contains(chunk); }));
}

/// Packs @p manifest into @p outPath over the pak at @p previousPath.
std::expected<PakReport, CookError> PackOver(const std::filesystem::path &cookedRoot,
                                             std::span<const ManifestEntry> manifest,
                                             const std::filesystem::path &outPath,
                                             const std::filesystem::path &previousPath)
{
    const std::expected<PakProvider, AssetError> previous = PakProvider::Mount(previousPath);
    REQUIRE(previous.has_value());
    return WritePak(cookedRoot, manifest, outPath, PakCodec::None, previous->Entries());
}

/// Mounts the pak at @p pakPath as a game would and checks that every asset in
/// @p manifest comes back as exactly the blob cooked for it. An index row that
/// disagrees with where its bytes were written, or two slices written over each
/// other, fails here even when every offset looks right.
void CheckServesEveryAsset(const std::filesystem::path &pakPath, const std::filesystem::path &cookedRoot,
                           std::span<const ManifestEntry> manifest)
{
    const std::expected<PakProvider, AssetError> pak = PakProvider::Mount(pakPath);
    REQUIRE(pak.has_value());
    for (const ManifestEntry &entry : manifest)
    {
        CAPTURE(entry.vpath);
        const std::expected<std::vector<std::byte>, AssetError> bytes = pak->Open(*AssetId::Parse(entry.guid));
        REQUIRE(bytes.has_value());
        const std::vector<char> blob = ReadFile(cookedRoot / (entry.guid + ".cooked"));
        REQUIRE(bytes->size() == blob.size());
        CHECK(std::equal(blob.begin(), blob.end(), bytes->begin(),
                         [](char c, std::byte b) { return static_cast<std::byte>(c) == b; }));
    }
}

/// The index row for @p vpath in @p index.
const Assisi::Core::PakEntry &Row(const std::vector<Assisi::Core::PakEntry> &index, const std::string &vpath)
{
    const Assisi::Core::AssetId id = Assisi::Core::DerivedAssetId(vpath);
    const auto found = std::ranges::find(index, id, &Assisi::Core::PakEntry::id);
    REQUIRE(found != index.end());
    return *found;
}

/// A manifest of @p count equal slices, named so they sort in creation order.
std::vector<ManifestEntry> WriteEqualBlobs(const std::filesystem::path &cookedRoot, std::uint32_t count,
                                           std::uint64_t bytesEach)
{
    std::vector<ManifestEntry> manifest;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        manifest.push_back(WriteBlob(cookedRoot, std::format("slice{:03}", i), bytesEach));
    }
    return manifest;
}

/// A slice well under a chunk, so several share one.
constexpr std::uint64_t kSliceBytes = 150 * kKiB;

/// Enough slices to span a dozen chunks, so a layout that shifts everything after
/// a change ships far more chunks than one that moves a single slice.
constexpr std::uint32_t kSliceCount = 80;

} // namespace

TEST_CASE("A packed tree serves every manifest asset's cooked bytes, by id and by path")
{
    const ScratchDir cooked("serve-cooked");
    const ScratchDir packed("serve-packed");
    const std::vector<ManifestEntry> manifest = CookFixture(cooked.Path());

    for (const PakCodec codec : {PakCodec::None, PakCodec::Lz4, PakCodec::Zstd})
    {
        CAPTURE(static_cast<std::uint32_t>(codec));
        const std::filesystem::path pakPath = packed.Path() / "assets.pak";
        const std::expected<PakReport, CookError> report = WritePak(cooked.Path(), manifest, pakPath, codec, {});
        REQUIRE_MESSAGE(report.has_value(), Explain(report));
        CHECK(report->slices == manifest.size());

        const std::expected<PakProvider, AssetError> pak = PakProvider::Mount(pakPath);
        REQUIRE(pak.has_value());
        for (const ManifestEntry &entry : manifest)
        {
            CAPTURE(entry.vpath);
            const AssetId id = *AssetId::Parse(entry.guid);

            const std::expected<AssetId, AssetError> resolved = pak->Resolve(entry.vpath);
            REQUIRE(resolved.has_value());
            CHECK(*resolved == id);

            const std::expected<std::vector<std::byte>, AssetError> bytes = pak->Open(id);
            REQUIRE(bytes.has_value());
            const std::vector<char> blob = ReadFile(cooked.Path() / (entry.guid + ".cooked"));
            REQUIRE(bytes->size() == blob.size());
            CHECK(std::equal(blob.begin(), blob.end(), bytes->begin(),
                             [](char c, std::byte b) { return static_cast<std::byte>(c) == b; }));
        }
    }
}

TEST_CASE("A blob the manifest does not list is left out of the pak")
{
    // The cooker never deletes: a removed or renamed asset leaves its blob in the
    // cooked directory forever. Packing the directory would ship those.
    const ScratchDir cooked("orphan-cooked");
    const ScratchDir packed("orphan-packed");
    const std::vector<ManifestEntry> manifest = CookFixture(cooked.Path());

    const AssetId orphan = Assisi::Core::DerivedAssetId("orphan");
    std::filesystem::copy_file(cooked.Path() / (manifest.front().guid + ".cooked"),
                               cooked.Path() / (orphan.ToString() + ".cooked"));

    const std::filesystem::path pakPath = packed.Path() / "assets.pak";
    REQUIRE(WritePak(cooked.Path(), manifest, pakPath, PakCodec::None, {}).has_value());

    const std::expected<PakProvider, AssetError> pak = PakProvider::Mount(pakPath);
    REQUIRE(pak.has_value());
    const std::expected<std::vector<std::byte>, AssetError> bytes = pak->Open(orphan);
    REQUIRE_FALSE(bytes.has_value());
    CHECK(bytes.error() == AssetError::UnknownAssetId);
}

TEST_CASE("A manifest row whose blob is missing fails the pack, naming the path")
{
    const ScratchDir cooked("missing-cooked");
    const ScratchDir packed("missing-packed");
    const std::vector<ManifestEntry> manifest = CookFixture(cooked.Path());
    std::filesystem::remove(cooked.Path() / (manifest.back().guid + ".cooked"));

    const std::expected<PakReport, CookError> report =
        WritePak(cooked.Path(), manifest, packed.Path() / "assets.pak", PakCodec::None, {});
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().vpath == manifest.back().vpath);
    // Nothing half-written is left where a game would find it.
    CHECK_FALSE(std::filesystem::exists(packed.Path() / "assets.pak"));
}

TEST_CASE("Two paths deriving one path id fail the pack, naming both")
{
    // A lookup by path goes through the path's derived id, so two paths sharing
    // one would make one of the assets unreachable by path. The fixture has no
    // real collision, so one path is listed twice under two ids.
    const ScratchDir cooked("collide-cooked");
    const ScratchDir packed("collide-packed");
    std::vector<ManifestEntry> manifest = CookFixture(cooked.Path());
    REQUIRE(manifest.size() >= 2);
    manifest[1].vpath = manifest[0].vpath;

    const std::expected<PakReport, CookError> report =
        WritePak(cooked.Path(), manifest, packed.Path() / "assets.pak", PakCodec::None, {});
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().reason.find(manifest[0].vpath) != std::string::npos);
}

TEST_CASE("Packing the same tree twice produces identical bytes")
{
    // A patch is a diff between two builds' paks, so a pack that differs between
    // runs of the same input makes every patch the whole game.
    const ScratchDir cooked("determinism-cooked");
    const ScratchDir packed("determinism-packed");
    const std::vector<ManifestEntry> manifest = CookFixture(cooked.Path());

    REQUIRE(WritePak(cooked.Path(), manifest, packed.Path() / "a.pak", PakCodec::Zstd, {}).has_value());
    REQUIRE(WritePak(cooked.Path(), manifest, packed.Path() / "b.pak", PakCodec::Zstd, {}).has_value());
    CHECK(ReadFile(packed.Path() / "a.pak") == ReadFile(packed.Path() / "b.pak"));
}

TEST_CASE("With no previous pak, slices sit back to back in manifest order")
{
    const ScratchDir cooked("fresh-cooked");
    const ScratchDir packed("fresh-packed");
    constexpr std::uint64_t kTinyBytes = 64;
    const std::vector<ManifestEntry> manifest = {WriteBlob(cooked.Path(), "a", 100 * kKiB),
                                                 WriteBlob(cooked.Path(), "b", kTinyBytes),
                                                 WriteBlob(cooked.Path(), "c", 300 * kKiB)};

    const std::filesystem::path pakPath = packed.Path() / "assets.pak";
    const std::expected<PakReport, CookError> report =
        WritePak(cooked.Path(), manifest, pakPath, PakCodec::None, {});
    REQUIRE_MESSAGE(report.has_value(), Explain(report));
    CHECK(report->layout == PakLayout::Fresh);
    CHECK(report->gapBytes == 0);

    const std::vector<Assisi::Core::PakEntry> index = ReadIndex(pakPath);
    CHECK(Row(index, "a").offset == Assisi::Core::kPakHeaderBytes);
    CHECK(Row(index, "b").offset == Assisi::Core::kPakHeaderBytes + 100 * kKiB);
    CHECK(Row(index, "c").offset == Assisi::Core::kPakHeaderBytes + 100 * kKiB + kTinyBytes);
}

TEST_CASE("Repacking an unchanged tree over its own pak reproduces it byte for byte")
{
    const ScratchDir cooked("same-cooked");
    const ScratchDir packed("same-packed");
    const std::vector<ManifestEntry> manifest = WriteEqualBlobs(cooked.Path(), kSliceCount, kSliceBytes);
    REQUIRE(WritePak(cooked.Path(), manifest, packed.Path() / "before.pak", PakCodec::None, {}).has_value());

    const std::expected<PakReport, CookError> report =
        PackOver(cooked.Path(), manifest, packed.Path() / "after.pak", packed.Path() / "before.pak");
    REQUIRE_MESSAGE(report.has_value(), Explain(report));
    CHECK(report->layout == PakLayout::FromPrevious);
    CHECK(report->keptSlices == manifest.size());
    CHECK(ReadFile(packed.Path() / "before.pak") == ReadFile(packed.Path() / "after.pak"));
}

TEST_CASE("A slice that grows moves, and every other slice keeps its offset")
{
    // A delta updater cuts files at fixed offsets. A slice that grows must not shift
    // every byte after it, or one changed material ships the rest of the pak.
    const ScratchDir cooked("grow-cooked");
    const ScratchDir packed("grow-packed");
    const std::vector<ManifestEntry> manifest = WriteEqualBlobs(cooked.Path(), kSliceCount, kSliceBytes);
    const std::filesystem::path before = packed.Path() / "before.pak";
    const std::filesystem::path after  = packed.Path() / "after.pak";
    REQUIRE(WritePak(cooked.Path(), manifest, before, PakCodec::None, {}).has_value());

    constexpr std::uint64_t kGrowthBytes = 8;
    const std::string grown = manifest[2].vpath;
    WriteBlob(cooked.Path(), grown, kSliceBytes + kGrowthBytes);
    const std::expected<PakReport, CookError> report = PackOver(cooked.Path(), manifest, after, before);
    REQUIRE_MESSAGE(report.has_value(), Explain(report));
    CHECK(report->keptSlices == manifest.size() - 1);

    const std::vector<Assisi::Core::PakEntry> oldIndex = ReadIndex(before);
    const std::vector<Assisi::Core::PakEntry> newIndex = ReadIndex(after);
    for (const ManifestEntry &entry : manifest)
    {
        CAPTURE(entry.vpath);
        if (entry.vpath != grown)
        {
            CHECK(Row(newIndex, entry.vpath).offset == Row(oldIndex, entry.vpath).offset);
        }
    }
    // Nothing else is big enough to take it, so it goes where the slices ended.
    const Assisi::Core::PakEntry &last = Row(oldIndex, manifest.back().vpath);
    CHECK(Row(newIndex, grown).offset == last.offset + last.storedSize);

    // The header's chunk, up to two where the slice was and is now zeros, and up to
    // two where it now sits beside the index.
    constexpr std::size_t kChunksPerMovedSlice = 5;
    CHECK(NewChunks(before, after) <= kChunksPerMovedSlice);

    CheckServesEveryAsset(after, cooked.Path(), manifest);
}

TEST_CASE("A slice that shrinks stays where it was, leaving the rest of its place as a gap")
{
    const ScratchDir cooked("shrink-cooked");
    const ScratchDir packed("shrink-packed");
    const std::vector<ManifestEntry> manifest = WriteEqualBlobs(cooked.Path(), kSliceCount, kSliceBytes);
    const std::filesystem::path before = packed.Path() / "before.pak";
    const std::filesystem::path after  = packed.Path() / "after.pak";
    REQUIRE(WritePak(cooked.Path(), manifest, before, PakCodec::None, {}).has_value());

    WriteBlob(cooked.Path(), manifest[2].vpath, kSliceBytes - kKiB);
    const std::expected<PakReport, CookError> report = PackOver(cooked.Path(), manifest, after, before);
    REQUIRE_MESSAGE(report.has_value(), Explain(report));
    CHECK(report->keptSlices == manifest.size());
    CHECK(report->gapBytes == kKiB);
    CHECK(Row(ReadIndex(after), manifest[2].vpath).offset == Row(ReadIndex(before), manifest[2].vpath).offset);

    CheckServesEveryAsset(after, cooked.Path(), manifest);
}

TEST_CASE("A removed slice is zeroed, and a new slice takes the smallest gap it fits")
{
    const ScratchDir cooked("gap-cooked");
    const ScratchDir packed("gap-packed");
    std::vector<ManifestEntry> manifest = WriteEqualBlobs(cooked.Path(), kSliceCount, kSliceBytes);
    const std::string small = manifest[5].vpath;
    const std::string large = manifest[12].vpath;
    WriteBlob(cooked.Path(), large, 2 * kSliceBytes);
    const std::filesystem::path first  = packed.Path() / "first.pak";
    const std::filesystem::path second = packed.Path() / "second.pak";
    const std::filesystem::path third  = packed.Path() / "third.pak";
    REQUIRE(WritePak(cooked.Path(), manifest, first, PakCodec::None, {}).has_value());
    const std::vector<Assisi::Core::PakEntry> firstIndex = ReadIndex(first);

    std::erase_if(manifest, [&](const ManifestEntry &entry) { return entry.vpath == small || entry.vpath == large; });
    const std::expected<PakReport, CookError> removed = PackOver(cooked.Path(), manifest, second, first);
    REQUIRE_MESSAGE(removed.has_value(), Explain(removed));
    CHECK(removed->layout == PakLayout::FromPrevious);
    CHECK(removed->gapBytes == 3 * kSliceBytes);

    // Content a release deleted does not ship in the next one.
    const std::vector<char> secondBytes = ReadFile(second);
    const Assisi::Core::PakEntry &gone  = Row(firstIndex, small);
    CHECK(std::all_of(secondBytes.begin() + static_cast<std::ptrdiff_t>(gone.offset),
                      secondBytes.begin() + static_cast<std::ptrdiff_t>(gone.offset + gone.storedSize),
                      [](char c) { return c == 0; }));
    CheckServesEveryAsset(second, cooked.Path(), manifest);

    constexpr std::uint64_t kAddedBytes = 100 * kKiB;
    manifest.push_back(WriteBlob(cooked.Path(), "slice999", kAddedBytes));
    const std::expected<PakReport, CookError> added = PackOver(cooked.Path(), manifest, third, second);
    REQUIRE_MESSAGE(added.has_value(), Explain(added));
    CHECK(Row(ReadIndex(third), "slice999").offset == gone.offset);
    CHECK(added->gapBytes == 3 * kSliceBytes - kAddedBytes);
    CheckServesEveryAsset(third, cooked.Path(), manifest);
}

TEST_CASE("Gaps past the limit are compacted into a fresh layout")
{
    const ScratchDir cooked("compact-cooked");
    const ScratchDir packed("compact-packed");
    std::vector<ManifestEntry> manifest = WriteEqualBlobs(cooked.Path(), kSliceCount, kSliceBytes);
    REQUIRE(WritePak(cooked.Path(), manifest, packed.Path() / "before.pak", PakCodec::None, {}).has_value());

    // A fifth of the slices, from the front, so what they leave is gaps rather
    // than space past the last slice.
    constexpr std::size_t kRemoved = kSliceCount / 5;
    manifest.erase(manifest.begin(), manifest.begin() + kRemoved);
    const std::expected<PakReport, CookError> report =
        PackOver(cooked.Path(), manifest, packed.Path() / "after.pak", packed.Path() / "before.pak");
    REQUIRE_MESSAGE(report.has_value(), Explain(report));
    CHECK(report->layout == PakLayout::Compacted);
    CHECK(report->gapBytes == 0);
    CHECK(Row(ReadIndex(packed.Path() / "after.pak"), manifest.front().vpath).offset ==
          Assisi::Core::kPakHeaderBytes);
    CheckServesEveryAsset(packed.Path() / "after.pak", cooked.Path(), manifest);
}
