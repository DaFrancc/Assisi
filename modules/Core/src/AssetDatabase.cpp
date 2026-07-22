/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Core/AssetDatabase.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
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

/// @brief Also write a freshly minted sidecar into the authoring root, when one
/// is configured (dev builds; see AssetSystem::SetAuthoringRoot).
///
/// A dev build reads from a staged copy of the assets next to the executable,
/// because generated files (compiled .spv) exist only there. That copy is wiped
/// by a clean build, so a GUID minted into it alone is regenerated differently
/// next time and every by-GUID reference to that asset silently stops resolving.
/// Mirroring the sidecar into the source tree makes the id durable and
/// committable. The staged copy is still written (above) so the id is consistent
/// for the rest of this run, and the next build's asset copy carries the source
/// sidecar back over it.
void MirrorSidecarToAuthoringRoot(std::string_view virtualPath, std::string_view content)
{
    const fs::path &authoringRoot = AssetSystem::GetAuthoringRoot();
    if (authoringRoot.empty())
    {
        return; // shipped build, or the staged copy IS the durable tree
    }

    std::error_code ec;

    // Only mirror when the ASSET itself lives in the durable tree. Build outputs
    // staged into the read root (compiled .spv) have no source counterpart, so a
    // sidecar for one would be an orphan describing a file no clone has — and it
    // would show up as an untracked source-tree change after every editor run.
    // Their ids are regenerated with the artifact, which is correct for a
    // derived file.
    if (!fs::exists(authoringRoot / virtualPath, ec))
    {
        return;
    }

    const fs::path target = authoringRoot / fs::path(virtualPath).concat(kSidecarExtension);
    if (fs::exists(target, ec))
    {
        return; // the durable tree already has an id for this asset; never clobber it
    }

    fs::create_directories(target.parent_path(), ec);
    if (!WriteWholeFile(target, content))
    {
        Log::Warn("AssetDatabase: minted a sidecar for '{}' but could not mirror it to the authoring root at "
                  "'{}'; the id will not survive a clean build.",
                  virtualPath, target.generic_string());
    }
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

    // Drawing from the engine mutates it. The magic-static initialization above is
    // thread-safe but the draws are not, and asset import already runs on job
    // threads (AssetImport calls this), so serialize them: two concurrent imports
    // racing the engine's state could otherwise hand back a torn or duplicate id,
    // and a duplicate id is exactly the collision the caller then has to re-mint.
    static std::mutex               engineMutex;
    std::lock_guard<std::mutex>     lock(engineMutex);
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

    // Kept separate from `ec`, which the loop body reuses and clears: a walk
    // failure must stay distinguishable from a per-entry stat failure.
    std::error_code walkEc;

    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, walkEc);
    const fs::recursive_directory_iterator end;
    if (walkEc)
    {
        Log::Warn("AssetDatabase: cannot walk asset root '{}': {}", root.generic_string(), walkEc.message());
        return std::unexpected(AssetError::NotInitialized);
    }

    for (; it != end; it.increment(walkEc))
    {
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
                    // size_t arithmetic (not slot+1 in uint32) so a large slot
                    // can't wrap the grow target; DeserializeSidecar already caps
                    // slot below kMaxMaterialSlots — this is defense in depth.
                    const std::size_t slotIndex = entry.slot;
                    if (slotIndex >= slots.size())
                    {
                        slots.resize(slotIndex + 1);
                    }
                    slots[slotIndex] = entry.material;
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
            MirrorSidecarToAuthoringRoot(virtualPath, content);
        }

        // Register, guarding against two files claiming the same id — which is
        // what a copied asset (sidecar and all) produces. First writer keeps the
        // id; the loser is re-minted rather than dropped. Leaving it unregistered
        // was permanent: its sidecar parses fine, so the mint-on-missing branch
        // above never fires for it, and every later Rebuild() repeats the same
        // collision, leaving the file unaddressable by id forever.
        auto [slot, inserted] = _idToPath.try_emplace(id, virtualPath);
        if (!inserted)
        {
            const AssetId  previous = id;
            const AssetId  reminted = MintAssetId();
            const std::string content = SerializeSidecar(AssetSidecar{.guid = reminted});
            if (!WriteWholeFile(sidecarPath, content))
            {
                Log::Warn("AssetDatabase: id {} is already taken by '{}' and re-minting for '{}' failed; it stays "
                          "unaddressable by id.",
                          previous.ToString(), slot->second, virtualPath);
                continue;
            }
            MirrorSidecarToAuthoringRoot(virtualPath, content);
            Log::Warn("AssetDatabase: id {} was already claimed by '{}'; re-minted '{}' as {} (duplicated asset?).",
                      previous.ToString(), slot->second, virtualPath, reminted.ToString());

            const auto [reslot, reinserted] = _idToPath.try_emplace(reminted, virtualPath);
            if (!reinserted)
            {
                continue; // astronomically unlikely; skip rather than clobber
            }
            id = reminted;
        }
        _pathToId.insert_or_assign(virtualPath, id);
        ++registered;
    }

    if (walkEc)
    {
        // A failed increment also sets the iterator to end, so the loop exits
        // normally — the error is only visible out here.
        Log::Warn("AssetDatabase: directory walk of '{}' ended early: {}", root.generic_string(), walkEc.message());
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
