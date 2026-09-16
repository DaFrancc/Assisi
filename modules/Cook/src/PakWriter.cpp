/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Cook/PakWriter.hpp>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Core/PakFormat.hpp>

#include <format>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Assisi::Cook
{

namespace
{

CookError Failure(std::string_view vpath, std::string reason)
{
    return CookError{.vpath = std::string{vpath}, .reason = std::move(reason)};
}

void WriteBytes(std::ofstream &out, std::span<const std::byte> bytes)
{
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
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

} // namespace

std::expected<PakReport, CookError> WritePak(const std::filesystem::path &cookedRoot,
                                             std::span<const ManifestEntry> entries,
                                             const std::filesystem::path &outPath, Core::PakCodec codec)
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

    const std::filesystem::path partialPath = PartialPath(outPath);
    std::error_code code;
    std::filesystem::create_directories(outPath.parent_path(), code);

    PakReport report;
    std::vector<Core::PakEntry> index;
    index.reserve(entries.size());
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
        for (const ManifestEntry &entry : entries)
        {
            const std::optional<Core::AssetId> id = Core::AssetId::Parse(entry.guid);
            if (!id)
            {
                std::filesystem::remove(partialPath, code);
                return std::unexpected(Failure(entry.vpath, std::format("has a manifest id '{}' that is not a GUID",
                                                                        entry.guid)));
            }

            std::expected<std::vector<std::byte>, CookError> blob =
                ReadBlob(cookedRoot / (entry.guid + std::string{kCookedExtension}), entry.vpath);
            if (!blob)
            {
                std::filesystem::remove(partialPath, code);
                return std::unexpected(blob.error());
            }

            Core::BitReader headerReader{*blob};
            const std::expected<Core::CookedKind, Core::CookedBlobError> kind = Core::ReadCookedHeader(headerReader);
            if (!kind)
            {
                std::filesystem::remove(partialPath, code);
                return std::unexpected(Failure(entry.vpath, std::format("has a cooked blob that does not read: {}",
                                                                        Core::ToString(kind.error()))));
            }

            // Stored as cooked when compression would not save a byte: a texture
            // already block-compressed often does not shrink, and decompressing it
            // for nothing is load time spent on every run.
            Core::PakCodec stored = Core::PakCodec::None;
            std::vector<std::byte> slice;
            if (codec != Core::PakCodec::None)
            {
                std::expected<std::vector<std::byte>, Core::PakCodecError> compressed =
                    Core::CompressSlice(codec, *blob);
                if (!compressed)
                {
                    std::filesystem::remove(partialPath, code);
                    return std::unexpected(Failure(entry.vpath, std::format("did not compress: {}",
                                                                            Core::ToString(compressed.error()))));
                }
                if (compressed->size() < blob->size())
                {
                    stored = codec;
                    slice  = std::move(*compressed);
                }
            }
            if (stored == Core::PakCodec::None)
            {
                slice = std::move(*blob);
            }

            Core::PakEntry row;
            row.id               = *id;
            row.pathId           = Core::DerivedAssetId(entry.vpath);
            row.offset           = offset;
            row.storedSize       = slice.size();
            row.uncompressedSize = stored == Core::PakCodec::None ? slice.size() : blob->size();
            row.contentHash      = entry.outputHash;
            row.codec            = stored;
            row.kind             = *kind;
            index.push_back(row);

            WriteBytes(out, slice);
            offset += slice.size();
            report.storedBytes += row.storedSize;
            report.uncompressedBytes += row.uncompressedSize;
            ++report.slices;
        }

        Core::BitWriter indexWriter;
        for (const Core::PakEntry &row : index)
        {
            Core::WritePakEntry(indexWriter, row);
        }
        WriteBytes(out, indexWriter.Data());

        Core::PakHeader header;
        header.entryCount  = static_cast<std::uint32_t>(index.size());
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
