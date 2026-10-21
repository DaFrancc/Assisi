/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "PakWriterTesting.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Core/PakProvider.hpp>

namespace Assisi::Cook::Testing
{

using Assisi::Core::AssetError;
using Assisi::Core::AssetId;
using Assisi::Core::PakCodec;
using Assisi::Core::PakProvider;

namespace
{

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

} // namespace

std::vector<char> ReadFile(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string Explain(const std::expected<PakReport, CookError> &result)
{
    return result ? std::string{} : result.error().vpath + ": " + result.error().reason;
}

std::vector<ManifestEntry> CookFixture(const std::filesystem::path &out)
{
    const std::expected<CookReport, CookError> report =
        CookTree(ASSISI_COOK_FIXTURE_ROOT, out, Assisi::Image::CompressQuality::Fast);
    REQUIRE(report.has_value());
    REQUIRE_FALSE(report->entries.empty());
    return report->entries;
}

ManifestEntry WriteBlob(const std::filesystem::path &cookedRoot, const std::string &vpath, std::uint64_t totalBytes)
{
    Assisi::Core::BitWriter writer;
    Assisi::Core::WriteCookedHeader(writer, Assisi::Core::kVerbatimKind);
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

std::size_t NewChunks(const std::filesystem::path &before, const std::filesystem::path &after)
{
    const std::set<std::string> held = Chunks(before);
    return static_cast<std::size_t>(
        std::ranges::count_if(Chunks(after), [&held](const std::string &chunk) { return !held.contains(chunk); }));
}

std::expected<PakReport, CookError> PackOver(const std::filesystem::path &cookedRoot,
                                             std::span<const ManifestEntry> manifest,
                                             const std::filesystem::path &outPath,
                                             const std::filesystem::path &previousPath)
{
    const std::expected<PakProvider, AssetError> previous = PakProvider::Mount(previousPath);
    REQUIRE(previous.has_value());
    return WritePak(cookedRoot, manifest, outPath, PakCodec::None, previous->Entries());
}

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

const Assisi::Core::PakEntry &Row(const std::vector<Assisi::Core::PakEntry> &index, const std::string &vpath)
{
    const Assisi::Core::AssetId id = Assisi::Core::DerivedAssetId(vpath);
    const auto found = std::ranges::find(index, id, &Assisi::Core::PakEntry::id);
    REQUIRE(found != index.end());
    return *found;
}

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

} // namespace Assisi::Cook::Testing
