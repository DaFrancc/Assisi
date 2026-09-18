/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Cook/PakWriter.hpp>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Core/PakFormat.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <numeric>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Assisi::Cook
{

namespace
{

/// Zeros are written from a buffer this size, so a gap of any length costs no
/// allocation of its own.
constexpr std::size_t kZeroBlockBytes = 64 * 1024;

/// Percent is out of this.
constexpr std::uint64_t kPercent = 100;

CookError Failure(std::string_view vpath, std::string reason)
{
    return CookError{.vpath = std::string{vpath}, .reason = std::move(reason)};
}

void WriteBytes(std::ofstream &out, std::span<const std::byte> bytes)
{
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

/// Written rather than seeked over, so the file is streamed front to back and
/// ends exactly where the index does.
void WriteZeros(std::ofstream &out, std::uint64_t count)
{
    static constexpr std::array<std::byte, kZeroBlockBytes> kZeros{};
    while (count > 0)
    {
        const std::uint64_t block = std::min<std::uint64_t>(count, kZeroBlockBytes);
        WriteBytes(out, std::span{kZeros}.first(static_cast<std::size_t>(block)));
        count -= block;
    }
}

std::expected<std::vector<std::byte>, CookError> ReadBlob(const std::filesystem::path &path, std::string_view vpath)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return std::unexpected(Failure(vpath, std::format("has no cooked blob at '{}'", path.generic_string())));
    }
    std::vector<char> chars{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::vector<std::byte> bytes(chars.size());
    std::transform(chars.begin(), chars.end(), bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

/// The temporary a pack writes before renaming it over @p outPath.
std::filesystem::path PartialPath(const std::filesystem::path &outPath)
{
    std::filesystem::path partial = outPath;
    partial += ".partial";
    return partial;
}

/// One asset's stored bytes and its index row, offset not yet decided.
struct PackedSlice
{
    Core::PakEntry row;
    std::vector<std::byte> bytes;
};

/// Reads, validates and compresses the blob @p entry names.
std::expected<PackedSlice, CookError> PackSlice(const std::filesystem::path &cookedRoot, const ManifestEntry &entry,
                                                Core::PakCodec codec)
{
    const std::optional<Core::AssetId> id = Core::AssetId::Parse(entry.guid);
    if (!id)
    {
        return std::unexpected(
            Failure(entry.vpath, std::format("has a manifest id '{}' that is not a GUID", entry.guid)));
    }

    std::expected<std::vector<std::byte>, CookError> blob =
        ReadBlob(cookedRoot / (entry.guid + std::string{kCookedExtension}), entry.vpath);
    if (!blob)
    {
        return std::unexpected(blob.error());
    }

    Core::BitReader headerReader{*blob};
    const std::expected<Core::CookedKind, Core::CookedBlobError> kind = Core::ReadCookedHeader(headerReader);
    if (!kind)
    {
        return std::unexpected(Failure(
            entry.vpath, std::format("has a cooked blob that does not read: {}", Core::ToString(kind.error()))));
    }

    PackedSlice slice;
    slice.row.id               = *id;
    slice.row.pathId           = Core::DerivedAssetId(entry.vpath);
    slice.row.uncompressedSize = blob->size();
    slice.row.contentHash      = entry.outputHash;
    slice.row.codec            = Core::PakCodec::None;
    slice.row.kind             = *kind;

    // Stored as cooked when compression would not save a byte: a texture
    // already block-compressed often does not shrink, and decompressing it
    // for nothing is load time spent on every run.
    if (codec != Core::PakCodec::None)
    {
        std::expected<std::vector<std::byte>, Core::PakCodecError> compressed = Core::CompressSlice(codec, *blob);
        if (!compressed)
        {
            return std::unexpected(
                Failure(entry.vpath, std::format("did not compress: {}", Core::ToString(compressed.error()))));
        }
        if (compressed->size() < blob->size())
        {
            slice.row.codec = codec;
            slice.bytes     = std::move(*compressed);
        }
    }
    if (slice.row.codec == Core::PakCodec::None)
    {
        slice.bytes = std::move(*blob);
    }
    slice.row.storedSize = slice.bytes.size();
    return slice;
}

/// Places every slice back to back after the header, in manifest order.
/// @return where the slice region ends.
std::uint64_t LayOutFresh(std::span<PackedSlice> slices)
{
    std::uint64_t offset = Core::kPakHeaderBytes;
    for (PackedSlice &slice : slices)
    {
        slice.row.offset = offset;
        offset += slice.row.storedSize;
    }
    return offset;
}

/// A run of the slice region no slice occupies.
struct Extent
{
    std::uint64_t offset = 0;
    std::uint64_t size   = 0;
};

/// What laying out over a previous pak produced.
struct PreviousLayout
{
    std::uint64_t end      = 0; ///< Where the slice region ends.
    std::uint64_t gapBytes = 0; ///< Unoccupied bytes left inside the region.
    std::size_t kept       = 0; ///< Slices at their previous offset.
};

/// Keeps every slice that still fits where @p previous had it there, and marks it
/// occupied. @return the slices that have to be placed anew, in manifest order.
std::vector<std::size_t> KeepPreviousPlaces(std::span<PackedSlice> slices, std::span<const Core::PakEntry> previous,
                                            std::vector<Extent> &occupied)
{
    std::unordered_map<Core::AssetId, const Core::PakEntry *> previousById;
    previousById.reserve(previous.size());
    for (const Core::PakEntry &entry : previous)
    {
        previousById.emplace(entry.id, &entry);
    }

    std::vector<std::size_t> unplaced;
    for (std::size_t i = 0; i < slices.size(); ++i)
    {
        Core::PakEntry &row = slices[i].row;
        const auto found    = previousById.find(row.id);
        if (found != previousById.end() && found->second->archive == row.archive &&
            row.storedSize <= found->second->storedSize)
        {
            row.offset = found->second->offset;
            occupied.push_back(Extent{.offset = row.offset, .size = row.storedSize});
        }
        else
        {
            unplaced.push_back(i);
        }
    }
    return unplaced;
}

/// Lays @p slices out over the placements in @p previous. See WritePak.
std::expected<PreviousLayout, CookError> LayOutFromPrevious(std::span<PackedSlice> slices,
                                                            std::span<const Core::PakEntry> previous)
{
    std::vector<Extent> occupied;
    std::vector<std::size_t> unplaced = KeepPreviousPlaces(slices, previous, occupied);
    const std::size_t kept            = slices.size() - unplaced.size();

    // The gaps between kept slices. Space after the last one is not a gap: the
    // region ends there and new slices append.
    std::ranges::sort(occupied, {}, &Extent::offset);
    std::vector<Extent> gaps;
    std::uint64_t end = Core::kPakHeaderBytes;
    for (const Extent &extent : occupied)
    {
        if (extent.offset < end)
        {
            return std::unexpected(Failure("previous pak", "has slices that overlap; pack without it"));
        }
        if (extent.offset > end)
        {
            gaps.push_back(Extent{.offset = end, .size = extent.offset - end});
        }
        end = extent.offset + extent.size;
    }

    // Largest first, so a big slice finds a gap before small ones break it up.
    // Stable, so equal sizes go in manifest order and the result is a function
    // of the inputs alone.
    std::ranges::stable_sort(unplaced, std::greater{},
                             [&slices](std::size_t i) { return slices[i].row.storedSize; });
    for (const std::size_t i : unplaced)
    {
        Core::PakEntry &row = slices[i].row;
        Extent *best        = nullptr;
        for (Extent &gap : gaps)
        {
            if (gap.size >= row.storedSize && (best == nullptr || gap.size < best->size))
            {
                best = &gap;
            }
        }
        if (best != nullptr)
        {
            row.offset = best->offset;
            best->offset += row.storedSize;
            best->size -= row.storedSize;
        }
        else
        {
            row.offset = end;
            end += row.storedSize;
        }
    }

    const std::uint64_t gapBytes =
        std::accumulate(gaps.begin(), gaps.end(), std::uint64_t{0},
                        [](std::uint64_t sum, const Extent &gap) { return sum + gap.size; });
    return PreviousLayout{.end = end, .gapBytes = gapBytes, .kept = kept};
}

/// Decides every slice's offset. @return the layout used and how many slices kept
/// their place, or a previous index that cannot be laid out over.
std::expected<PakReport, CookError> LayOut(std::span<PackedSlice> slices, std::span<const Core::PakEntry> previous)
{
    PakReport report;
    if (previous.empty())
    {
        LayOutFresh(slices);
        return report;
    }

    const std::expected<PreviousLayout, CookError> placed = LayOutFromPrevious(slices, previous);
    if (!placed)
    {
        return std::unexpected(placed.error());
    }
    const std::uint64_t region = placed->end - Core::kPakHeaderBytes;
    if (placed->gapBytes * kPercent > region * kMaxPakGapPercent)
    {
        LayOutFresh(slices);
        report.layout = PakLayout::Compacted;
        return report;
    }
    report.layout     = PakLayout::FromPrevious;
    report.keptSlices = placed->kept;
    return report;
}

} // namespace

std::string_view ToString(PakLayout layout) noexcept
{
    switch (layout)
    {
    case PakLayout::Fresh:
        return "fresh layout";
    case PakLayout::FromPrevious:
        return "laid out over the previous pak";
    case PakLayout::Compacted:
        return "compacted: the previous layout left too many gaps";
    case PakLayout::Count:
        break;
    }
    return {};
}

std::expected<PakReport, CookError> WritePak(const std::filesystem::path &cookedRoot,
                                             std::span<const ManifestEntry> entries,
                                             const std::filesystem::path &outPath, Core::PakCodec codec,
                                             std::span<const Core::PakEntry> previous)
{
    // Refused before any work: a derived path id is how a pak finds an asset by
    // path, so two paths sharing one would leave one of them unreachable.
    std::unordered_map<Core::AssetId, const ManifestEntry *> byPath;
    byPath.reserve(entries.size());
    for (const ManifestEntry &entry : entries)
    {
        const auto [slot, inserted] = byPath.try_emplace(Core::DerivedAssetId(entry.vpath), &entry);
        if (!inserted)
        {
            return std::unexpected(Failure(entry.vpath, std::format("shares a path id with '{}', so one of them "
                                                                    "could not be found by path; rename either",
                                                                    slot->second->vpath)));
        }
    }

    // Every slice is held until the layout is decided, because where one goes
    // depends on the sizes of all the others.
    std::vector<PackedSlice> slices;
    slices.reserve(entries.size());
    for (const ManifestEntry &entry : entries)
    {
        std::expected<PackedSlice, CookError> slice = PackSlice(cookedRoot, entry, codec);
        if (!slice)
        {
            return std::unexpected(slice.error());
        }
        slices.push_back(std::move(*slice));
    }

    std::expected<PakReport, CookError> report = LayOut(slices, previous);
    if (!report)
    {
        return report;
    }
    report->slices = slices.size();

    std::vector<const PackedSlice *> byOffset;
    byOffset.reserve(slices.size());
    for (const PackedSlice &slice : slices)
    {
        byOffset.push_back(&slice);
        report->storedBytes += slice.row.storedSize;
        report->uncompressedBytes += slice.row.uncompressedSize;
    }
    std::ranges::sort(byOffset, {}, [](const PackedSlice *slice) { return slice->row.offset; });

    const std::filesystem::path partialPath = PartialPath(outPath);
    std::error_code code;
    std::filesystem::create_directories(outPath.parent_path(), code);
    {
        std::ofstream out(partialPath, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            return std::unexpected(Failure(outPath.generic_string(), "could not be created"));
        }

        // A placeholder the real header replaces once the index offset is known.
        Core::BitWriter placeholder;
        Core::WritePakHeader(placeholder, Core::PakHeader{});
        WriteBytes(out, placeholder.Data());

        std::uint64_t offset = Core::kPakHeaderBytes;
        for (const PackedSlice *slice : byOffset)
        {
            WriteZeros(out, slice->row.offset - offset);
            report->gapBytes += slice->row.offset - offset;
            WriteBytes(out, slice->bytes);
            offset = slice->row.offset + slice->row.storedSize;
        }

        // In manifest order, so the index itself only changes where the slices did.
        Core::BitWriter indexWriter;
        for (const PackedSlice &slice : slices)
        {
            Core::WritePakEntry(indexWriter, slice.row);
        }
        WriteBytes(out, indexWriter.Data());

        Core::PakHeader header;
        header.entryCount  = static_cast<std::uint32_t>(slices.size());
        header.indexOffset = offset;
        Core::BitWriter headerWriter;
        Core::WritePakHeader(headerWriter, header);
        out.seekp(0);
        WriteBytes(out, headerWriter.Data());

        if (!out.flush())
        {
            out.close();
            std::filesystem::remove(partialPath, code);
            return std::unexpected(Failure(outPath.generic_string(), "could not be written"));
        }
    }

    std::filesystem::rename(partialPath, outPath, code);
    if (code)
    {
        std::filesystem::remove(partialPath, code);
        return std::unexpected(Failure(outPath.generic_string(), "could not be moved into place"));
    }
    return report;
}

} // namespace Assisi::Cook
