/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Core/AssetDatabase.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <system_error>

#include <Assisi/Core/AssetSidecar.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>

namespace Assisi::Core
{
namespace
{
namespace fs = std::filesystem;

constexpr std::string_view kSidecarExtension = ".aast";

/// @brief Read a whole file into a string via absolute path. std::nullopt on any
///        I/O failure. Used for sidecars, whose absolute paths the scan already
///        holds — no need to round-trip through virtual-path resolution.
std::optional<std::string> ReadWholeFile(const fs::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (!stream.good() && !stream.eof())
    {
        return std::nullopt;
    }
    return buffer.str();
}

/// @brief Write text to an absolute path, truncating. Returns false on failure.
bool WriteWholeFile(const fs::path &path, std::string_view text)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        return false;
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    return stream.good();
}
} // namespace

AssetId MintAssetId()
{
    // Editor-time only. Seeded once from the platform entropy source; a single
    // 64-bit engine is plenty for id generation (this is not cryptographic).
    static std::mt19937_64 engine = []
    {
        std::random_device                                       device;
        std::array<std::uint32_t, std::mt19937_64::state_size>   seedData{};
        for (auto &word : seedData)
        {
            word = device();
        }
        std::seed_seq seeds(seedData.begin(), seedData.end());
        return std::mt19937_64(seeds);
    }();

    std::uniform_int_distribution<std::uint64_t> dist;
    const std::uint64_t                          high = dist(engine);
    const std::uint64_t                          low  = dist(engine);

    AssetId id{};
    for (std::size_t i = 0; i < 8; ++i)
    {
        id.bytes[i]     = static_cast<std::uint8_t>(high >> (8 * (7 - i)));
        id.bytes[8 + i] = static_cast<std::uint8_t>(low >> (8 * (7 - i)));
    }
    // RFC 4122: version 4 in the high nibble of byte 6, variant 0b10 in byte 8.
    // Both are non-zero, so a minted id can never land in the reserved built-in
    // range (first 15 bytes zero).
    id.bytes[6] = static_cast<std::uint8_t>(0x40 | (id.bytes[6] & 0x0F));
    id.bytes[8] = static_cast<std::uint8_t>(0x80 | (id.bytes[8] & 0x3F));
    return id;
}

std::expected<std::size_t, AssetError> AssetDatabase::Rebuild()
{
    _idToPath.clear();
    _pathToId.clear();
    _manifests.clear();

    // Seed the reserved built-ins first — they resolve to primitive factories,
    // not files, so the scan never touches them.
    for (const BuiltinAssetEntry &entry : BuiltinAssets())
    {
        _idToPath.emplace(entry.id, std::string(entry.virtualPath));
        _pathToId.emplace(std::string(entry.virtualPath), entry.id);
    }

    const fs::path &root = AssetSystem::GetRoot();
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec))
    {
        return std::unexpected(AssetError::NotInitialized);
    }

    std::size_t registered = 0;

    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec))
    {
        if (ec)
        {
            Log::Warn("AssetDatabase: directory walk error: {}", ec.message());
            break;
        }

        if (!it->is_regular_file(ec) || ec)
        {
            ec.clear();
            continue;
        }

        const fs::path &filePath = it->path();

        // D6: every file gets a sidecar except a sidecar itself (no
        // sidecar-for-sidecar — the scan-termination requirement).
        if (filePath.extension() == kSidecarExtension)
        {
            continue;
        }

        // Virtual path in generic ('/') form, relative to the asset root.
        const std::string virtualPath = fs::relative(filePath, root, ec).generic_string();
        if (ec || virtualPath.empty())
        {
            ec.clear();
            continue;
        }

        // Sidecar sits next to the file: "<file>.aast".
        fs::path sidecarPath = filePath;
        sidecarPath += std::string(kSidecarExtension);

        AssetId id{};
        if (fs::exists(sidecarPath, ec))
        {
            // Reconcile-not-clobber: read the existing id, never rewrite it.
            const std::optional<std::string> text = ReadWholeFile(sidecarPath);
            if (!text.has_value())
            {
                Log::Warn("AssetDatabase: cannot read sidecar '{}', skipping.", sidecarPath.generic_string());
                continue;
            }
            const std::expected<AssetSidecar, AssetSidecarError> sidecar = DeserializeSidecar(*text);
            if (!sidecar.has_value())
            {
                Log::Warn("AssetDatabase: malformed sidecar '{}' ({}), skipping (left untouched).",
                          sidecarPath.generic_string(), ToString(sidecar.error()));
                continue;
            }
            id = sidecar->guid;

            // Composite manifest (S3): flatten `slot → material` into a dense
            // slot-indexed vector, nil-filling any gap so SlotMaterial() is a
            // plain bounds-checked index. A duplicate slot: last entry wins.
            if (!sidecar->subAssets.empty())
            {
                std::vector<AssetId> slots;
                for (const AssetSubAsset &entry : sidecar->subAssets)
                {
                    if (entry.slot >= slots.size())
                    {
                        slots.resize(entry.slot + 1);
                    }
                    slots[entry.slot] = entry.material;
                }
                _manifests.insert_or_assign(id, std::move(slots));
            }
        }
        else
        {
            // Missing sidecar: mint an id and write one.
            id                        = MintAssetId();
            const std::string content = SerializeSidecar(AssetSidecar{.guid = id});
            if (!WriteWholeFile(sidecarPath, content))
            {
                Log::Warn("AssetDatabase: failed to write sidecar '{}', skipping.", sidecarPath.generic_string());
                continue;
            }
        }

        // Register, guarding against two files claiming the same id (e.g. a
        // copied sidecar). First writer wins; the collision is reported.
        const auto [slot, inserted] = _idToPath.try_emplace(id, virtualPath);
        if (!inserted)
        {
            Log::Warn("AssetDatabase: id {} already maps to '{}'; ignoring duplicate at '{}'.", id.ToString(),
                      slot->second, virtualPath);
            continue;
        }
        _pathToId.insert_or_assign(virtualPath, id);
        ++registered;
    }

    return registered;
}

std::optional<std::string> AssetDatabase::PathFor(AssetId id) const
{
    const auto found = _idToPath.find(id);
    if (found == _idToPath.end())
    {
        return std::nullopt;
    }
    return found->second;
}

std::optional<AssetId> AssetDatabase::IdFor(std::string_view virtualPath) const
{
    const auto found = _pathToId.find(std::string(virtualPath));
    if (found == _pathToId.end())
    {
        return std::nullopt;
    }
    return found->second;
}

std::size_t AssetDatabase::Count() const noexcept
{
    return _idToPath.size();
}

std::vector<std::pair<AssetId, std::string>> AssetDatabase::Assets() const
{
    std::vector<std::pair<AssetId, std::string>> assets;
    assets.reserve(_idToPath.size());
    for (const auto &[id, path] : _idToPath)
    {
        // Skip the reserved built-ins: they are primitive factories, not files
        // an editor pass can open or explode.
        if (!id.IsReserved())
        {
            assets.emplace_back(id, path);
        }
    }
    return assets;
}

bool AssetDatabase::HasManifest(AssetId meshId) const
{
    return _manifests.contains(meshId);
}

AssetId AssetDatabase::SlotMaterial(AssetId meshId, std::uint32_t slot) const
{
    const auto found = _manifests.find(meshId);
    if (found == _manifests.end() || slot >= found->second.size())
    {
        return {};
    }
    return found->second[slot];
}

} // namespace Assisi::Core
