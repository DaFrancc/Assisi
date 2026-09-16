/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Cook/CookTree.hpp>

#include <Assisi/Core/AssetDatabase.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/ContentHash.hpp>
#include <Assisi/Core/JobSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Geometry/MaterialChannels.hpp>
#include <Assisi/Geometry/MaterialFile.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <format>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace Assisi::Cook
{

namespace
{

CookError Failure(std::string_view vpath, std::string reason)
{
    return CookError{.vpath = std::string{vpath}, .reason = std::move(reason)};
}

std::uint64_t HashOf(std::span<const std::byte> bytes)
{
    return Core::ContentHash64(bytes);
}

bool WriteWholeFile(const std::filesystem::path &path, std::span<const std::byte> bytes)
{
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        return false;
    }
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

/// One asset, decided serially and encoded in parallel.
struct PlannedAsset
{
    std::filesystem::path output;
    std::string vpath;
    const Cooker *cooker = nullptr;
    Core::AssetId id;
    std::uint64_t key        = 0;
    std::uint64_t outputHash = 0;

    /// Set when the blob is on disk and current — by the plan for an asset whose
    /// key already matched, or by the worker that wrote it.
    bool done = false;

    /// Set by the plan alone: this asset was already current, so no worker
    /// touched it. Kept apart from `done` because after the encode pass both are
    /// true for everything, and the report has to say which was which.
    bool skipped = false;
};

/// What every worker in the encode pass shares.
///
/// The plan and the context are read-only to them but for each worker's own slot
/// of `plan`; the failure fields are the only thing they contend on, and only on
/// the path that is about to end the run anyway.
struct CookRun
{
    std::vector<PlannedAsset> *plan = nullptr;
    const CookContext *context      = nullptr;

    CookError firstFailure;
    std::mutex failureMutex;
    std::atomic<bool> failed{false};
};

/// Keeps the *first* failure rather than the last.
///
/// Workers fail concurrently and the build log shows one line, so which one it
/// is matters: the lowest virtual path is the one a person re-running the cook
/// will hit again, and it does not change with how the chunks happened to land.
void RecordFailure(CookRun &run, CookError error)
{
    const std::lock_guard<std::mutex> lock(run.failureMutex);
    if (!run.failed.exchange(true, std::memory_order_relaxed) || error.vpath < run.firstFailure.vpath)
    {
        run.firstFailure = std::move(error);
    }
}

/// Encodes one planned asset and writes its blob.
void CookOne(CookRun &run, std::uint32_t index)
{
    PlannedAsset &planned = (*run.plan)[index];

    const std::expected<std::vector<std::byte>, CookError> cooked =
        planned.cooker->Cook(planned.vpath, planned.id, *run.context);
    if (!cooked)
    {
        RecordFailure(run, cooked.error());
        return;
    }

    if (!WriteWholeFile(planned.output, *cooked))
    {
        RecordFailure(run, Failure(planned.vpath, std::format("could not be written to '{}'",
                                                              planned.output.generic_string())));
        return;
    }

    planned.outputHash = HashOf(*cooked);
    planned.done       = true;
}

/// One worker's share of the plan.
///
/// Stops early once any worker has failed: the run is going to end on that
/// failure, and the assets after it would be minutes of BC7 nobody reads.
void CookChunk(CookRun &run, std::uint32_t begin, std::uint32_t end)
{
    for (std::uint32_t i = begin; i < end && !run.failed.load(std::memory_order_relaxed); ++i)
    {
        if (!(*run.plan)[i].done)
        {
            CookOne(run, i);
        }
    }
}

/// Folds one more value into a running FNV-1a, so a key can cover several things
/// without concatenating them into a buffer first.
std::uint64_t MixHash(std::uint64_t seed, std::uint64_t value)
{
    for (std::uint32_t byte = 0; byte < 8; ++byte)
    {
        seed ^= (value >> (byte * 8)) & 0xFFull;
        seed *= Core::kFnvPrime;
    }
    return seed;
}

/// Everything that decides what an asset cooks to.
///
/// The source bytes are the obvious part. The rest is what would otherwise leave
/// stale blobs behind a change that never touched a source file: the envelope's
/// version, the payload kind, and the codec layout the reflected and scene
/// cookers write through.
std::uint64_t CookKey(std::span<const std::byte> source, std::span<const std::uint64_t> dependencyHashes,
                      std::string_view cookerName)
{
    std::uint64_t key = HashOf(source);
    for (const std::uint64_t dependency : dependencyHashes)
    {
        key = MixHash(key, dependency);
    }
    key = MixHash(key, Core::kCookedFormatVersion);
    key = MixHash(key, Core::ContentHash64(std::as_bytes(std::span{cookerName})));
    // The component table, which both the scene blocks and every asset block are
    // written against. A field added to a component changes what a cooked level
    // means without changing one byte of the level file.
    key = MixHash(key, Core::Reflect::ProtocolHash());
    return key;
}

/// Every texture's channel, gathered before a single one is cooked.
std::expected<TextureRoles, CookError> GatherTextureRoles(const std::vector<std::pair<Core::AssetId,
                                                                                     std::string>> &assets)
{
    TextureRoles roles;
    for (const auto &[id, vpath] : assets)
    {
        if (!vpath.ends_with(".amat"))
        {
            continue;
        }
        const std::expected<std::string, Core::AssetError> text = Core::AssetSystem::ReadText(vpath);
        if (!text)
        {
            return std::unexpected(Failure(vpath, "could not be read while gathering texture channels"));
        }
        const std::expected<Geometry::MaterialData, Geometry::MaterialFileError> material =
            Geometry::DeserializeMaterial(*text);
        if (!material)
        {
            return std::unexpected(Failure(vpath, std::string{Geometry::ToString(material.error())}));
        }

        for (const Geometry::MaterialChannel channel : Geometry::kMaterialChannels)
        {
            // A false here is a conflict, which is reported when that texture is
            // cooked — that is where the path is known.
            (void)roles.Bind(Geometry::ChannelTexture(*material, channel), channel, vpath);
        }
    }
    return roles;
}

} // namespace

std::string SerializeManifest(const std::vector<ManifestEntry> &entries)
{
    std::string text;
    text.reserve(entries.size() * 96u);
    for (const ManifestEntry &entry : entries)
    {
        text += std::format("{} {} {} {}\n", entry.vpath, entry.guid, Core::ToHex64(entry.cookKey),
                            Core::ToHex64(entry.outputHash));
    }
    return text;
}

std::vector<ManifestEntry> DeserializeManifest(std::string_view text)
{
    std::vector<ManifestEntry> entries;
    std::size_t start = 0;
    while (start < text.size())
    {
        const std::size_t end  = std::min(text.find('\n', start), text.size());
        const std::string_view line = text.substr(start, end - start);
        start                  = end + 1;
        if (line.empty())
        {
            continue;
        }

        // A malformed line is dropped rather than guessed at: the cost is
        // re-cooking that asset, and the alternative is skipping one that
        // changed.
        const std::size_t firstSpace = line.find(' ');
        if (firstSpace == std::string_view::npos)
        {
            continue;
        }
        const std::size_t secondSpace = line.find(' ', firstSpace + 1);
        if (secondSpace == std::string_view::npos)
        {
            continue;
        }
        const std::size_t thirdSpace = line.find(' ', secondSpace + 1);
        if (thirdSpace == std::string_view::npos)
        {
            continue;
        }

        const std::optional<std::uint64_t> key =
            Core::FromHex64(line.substr(secondSpace + 1, thirdSpace - secondSpace - 1));
        const std::optional<std::uint64_t> output = Core::FromHex64(line.substr(thirdSpace + 1));
        if (!key || !output)
        {
            continue;
        }

        entries.push_back(ManifestEntry{.vpath      = std::string{line.substr(0, firstSpace)},
                                        .guid       = std::string{line.substr(firstSpace + 1,
                                                                              secondSpace - firstSpace - 1)},
                                        .cookKey    = *key,
                                        .outputHash = *output});
    }
    return entries;
}

std::expected<CookReport, CookError> CookTree(const std::filesystem::path &sourceRoot,
                                              const std::filesystem::path &cookedRoot)
{
    if (const std::expected<void, Core::AssetError> root = Core::AssetSystem::SetRoot(sourceRoot); !root)
    {
        return std::unexpected(Failure(sourceRoot.generic_string(), "is not a readable asset root"));
    }

    // Read-only: a cook that minted an id would give an asset a name the
    // authoring tree knows nothing about, and the next scan there would mint
    // another one for the same file.
    Core::AssetDatabase database;
    if (const std::expected<std::size_t, Core::AssetError> built =
            database.Rebuild(Core::RebuildMode::ReadOnly);
        !built)
    {
        return std::unexpected(Failure(sourceRoot.generic_string(), "could not be scanned"));
    }

    // The filesystem, not the database's index.
    //
    // A read-only scan leaves out every file with no `.aast` beside it, and the
    // shaders are exactly that case: `.gitignore` excludes both the `.spv` and
    // its sidecar, so on a fresh clone the database would not list a single one
    // and the cooked tree would silently have no shaders in it. Walking the tree
    // and asking the database for an id — rather than asking the database what
    // exists — is what lets a cooker name its own assets instead.
    //
    // Sorted, because both `directory_iterator` and `Assets()` leave their order
    // to the filesystem and to a hash map respectively, and an unspecified order
    // is a cooked tree that differs between two runs of the same binary.
    const Core::AssetIgnoreList &ignore = database.Ignore();

    std::vector<std::string> paths;
    std::error_code walkCode;
    for (std::filesystem::recursive_directory_iterator it{sourceRoot, walkCode}, end; it != end;
         it.increment(walkCode))
    {
        if (walkCode)
        {
            return std::unexpected(Failure(sourceRoot.generic_string(), "could not be walked"));
        }
        if (!it->is_regular_file())
        {
            continue;
        }

        const std::string vpath =
            std::filesystem::relative(it->path(), sourceRoot, walkCode).generic_string();
        if (walkCode || vpath.empty())
        {
            continue;
        }
        // Sidecars describe assets rather than being them, and the ignore list
        // describes the pipeline rather than feeding it.
        if (vpath.ends_with(".aast") || ignore.IsFileIgnored(vpath))
        {
            continue;
        }
        paths.push_back(vpath);
    }
    std::ranges::sort(paths);

    // The `.amat` files, in the same sorted order, so the channel map is built
    // from a list nothing can reorder.
    std::vector<std::pair<Core::AssetId, std::string>> assets;
    assets.reserve(paths.size());
    for (const std::string &vpath : paths)
    {
        const std::optional<Core::AssetId> id = database.IdFor(vpath);
        assets.emplace_back(id.value_or(Core::AssetId{}), vpath);
    }

    const std::expected<TextureRoles, CookError> roles = GatherTextureRoles(assets);
    if (!roles)
    {
        return std::unexpected(roles.error());
    }

    CookContext context;
    context.database = &database;
    context.roles    = &*roles;

    const std::vector<std::unique_ptr<Cooker>> cookers = MakeCookers();

    // What the last run produced, so an unchanged asset costs a hash rather than
    // an encode.
    std::unordered_map<std::string, ManifestEntry> previous;
    {
        std::ifstream manifest(cookedRoot / kManifestFileName, std::ios::binary);
        if (manifest)
        {
            const std::string text{std::istreambuf_iterator<char>(manifest), std::istreambuf_iterator<char>()};
            for (ManifestEntry &entry : DeserializeManifest(text))
            {
                previous.emplace(entry.vpath, std::move(entry));
            }
        }
    }

    // Pass 1, serial: decide what each asset needs, and refuse the tree here
    // rather than inside a worker. Everything below reads this plan and never
    // adds to it, which is what makes the encode safe to fan out.
    CookReport report;
    std::vector<PlannedAsset> plan;
    plan.reserve(assets.size());

    for (const auto &[indexedId, vpath] : assets)
    {
        const Cooker *owner = nullptr;
        Claim claim         = Claim::None;
        for (const std::unique_ptr<Cooker> &cooker : cookers)
        {
            if (const Claim candidate = cooker->Claims(vpath); candidate != Claim::None)
            {
                owner = cooker.get();
                claim = candidate;
                break;
            }
        }

        if (owner == nullptr)
        {
            // Total by construction: `.assisiignore` is what says a file is not
            // content, so anything still here that nothing cooks is an asset the
            // shipped build would silently lack.
            return std::unexpected(Failure(vpath, "is content that no cooker claims"));
        }
        if (claim == Claim::SourceOnly)
        {
            // Claimed and producing nothing still has to be *accounted for*: a
            // shader source whose compiled output is absent would otherwise
            // leave the cooked tree one stage short with nothing said about it.
            if (const std::expected<void, CookError> complete = owner->CheckSource(vpath); !complete)
            {
                return std::unexpected(complete.error());
            }
            ++report.sourceOnly;
            continue;
        }

        // The sidecar's id where there is one, and the cooker's own where the
        // sidecar does not ship. An asset with neither cannot be addressed at
        // all, so it fails here rather than being written under a name nothing
        // could ask for.
        Core::AssetId id = indexedId;
        if (id.IsNil())
        {
            id = owner->DerivedId(vpath);
        }
        if (id.IsNil())
        {
            return std::unexpected(Failure(vpath, "has no .aast sidecar, so it has no id to be cooked under"));
        }

        const std::expected<std::vector<std::byte>, Core::AssetError> source = Core::AssetSystem::ReadBinary(vpath);
        if (!source)
        {
            return std::unexpected(Failure(vpath, "could not be read"));
        }

        std::vector<std::uint64_t> dependencyHashes;
        for (const std::string &dependency : owner->Dependencies(vpath))
        {
            if (const std::expected<std::vector<std::byte>, Core::AssetError> bytes =
                    Core::AssetSystem::ReadBinary(dependency))
            {
                dependencyHashes.push_back(HashOf(*bytes));
            }
        }

        PlannedAsset planned;
        planned.vpath  = vpath;
        planned.id     = id;
        planned.cooker = owner;
        planned.key    = CookKey(*source, dependencyHashes, owner->Name());
        planned.output = cookedRoot / (id.ToString() + std::string{kCookedExtension});

        // The manifest says what was produced; the tree says what is there.
        // Trusting the first alone would leave a cooked tree missing a file every
        // later run believes it already wrote.
        if (const auto found = previous.find(vpath);
            found != previous.end() && found->second.cookKey == planned.key &&
            std::filesystem::exists(planned.output))
        {
            planned.done       = true;
            planned.skipped    = true;
            planned.outputHash = found->second.outputHash;
        }
        plan.push_back(std::move(planned));
    }

    // Pass 2, parallel: encode and write. Every asset reads its own bytes and
    // writes its own blob under its own GUID, so there is nothing to share —
    // the channel map and the database are finished and read-only by now, and
    // each slot of `plan` is touched by exactly one worker.
    //
    // A level cook opens a serialization context, which refuses to nest. That
    // guard is thread_local, so one scene per thread is fine and two on one
    // thread never happens: ParallelFor gives a chunk to a worker and the
    // context is closed before the next asset in that chunk begins.
    //
    // Determinism is unaffected: order decided the plan, and the manifest below
    // is assembled from it rather than from whatever finishes first.
    CookRun run;
    run.plan    = &plan;
    run.context = &context;

    // One asset per chunk: the work per asset is wildly uneven — a 2K texture at
    // the best BC7 tier against a two-line material — so a coarser grain would
    // leave one worker holding every big texture.
    constexpr std::uint32_t kOneAssetPerChunk = 1;

    Core::JobSystem jobs;
    jobs.ParallelFor(static_cast<std::uint32_t>(plan.size()), kOneAssetPerChunk,
                     [&run](std::uint32_t begin, std::uint32_t end) { CookChunk(run, begin, end); });

    if (run.failed.load(std::memory_order_relaxed))
    {
        return std::unexpected(run.firstFailure);
    }

    for (const PlannedAsset &planned : plan)
    {
        report.entries.push_back(ManifestEntry{.vpath      = planned.vpath,
                                               .guid       = planned.id.ToString(),
                                               .cookKey    = planned.key,
                                               .outputHash = planned.outputHash});
        if (planned.skipped)
        {
            ++report.skipped;
        }
        else
        {
            ++report.cooked;
        }
    }

    // Already in virtual-path order, because the asset list was sorted before the
    // walk and every entry is appended once.
    const std::string manifest = SerializeManifest(report.entries);
    if (!WriteWholeFile(cookedRoot / kManifestFileName, std::as_bytes(std::span{manifest})))
    {
        return std::unexpected(Failure(kManifestFileName, "could not be written"));
    }

    return report;
}

} // namespace Assisi::Cook
