/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCookTree.cpp
/// @brief Cooking a whole tree: what each cooker claims, and the three
/// properties the cook has to have.
///
/// Determinism and incrementality are checked by cooking the same fixture twice.
/// Totality is checked by putting a file nothing handles into a tree and
/// requiring the cook to refuse it — the property that a shipped build cannot
/// quietly lack an asset is worth more than any individual cooker's output.

#include <doctest/doctest.h>

#include <ostream>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <Assisi/Cook/CookTree.hpp>
#include <Assisi/Core/AssetSystem.hpp>

using Assisi::Cook::Claim;
using Assisi::Cook::CookReport;
using Assisi::Cook::CookTree;
using Assisi::Cook::MakeCookers;

namespace
{

/// Which claim the cooker set gives a path, by asking them in the order the walk
/// does.
Claim ClaimFor(std::string_view vpath)
{
    for (const auto &cooker : MakeCookers())
    {
        if (const Claim claim = cooker->Claims(vpath); claim != Claim::None)
        {
            return claim;
        }
    }
    return Claim::None;
}

/// What went wrong, as a line a failing test can print — the cook's whole
/// contract is that a failure names the file, so a test that only said "false"
/// would be throwing that away.
std::string Explain(const std::expected<CookReport, Assisi::Cook::CookError> &result)
{
    return result ? std::string{} : result.error().vpath + ": " + result.error().reason;
}

/// A scratch directory that cleans itself up, so a failed test leaves no tree
/// behind for the next run to find and skip against.
class ScratchDir
{
public:
    explicit ScratchDir(std::string_view name)
        : _path(std::filesystem::temp_directory_path() / ("assisi-cook-test-" + std::string{name}))
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
    ScratchDir(ScratchDir &&)                 = delete;
    ScratchDir &operator=(ScratchDir &&)      = delete;

    [[nodiscard]] const std::filesystem::path &Path() const { return _path; }

private:
    std::filesystem::path _path;
};

/// Every cooked blob in a tree, read back, keyed by filename — so two runs can be
/// compared byte for byte without caring what order the directory lists them in.
std::map<std::string, std::vector<char>> ReadCookedTree(const std::filesystem::path &root)
{
    std::map<std::string, std::vector<char>> files;
    std::error_code code;
    for (const auto &entry : std::filesystem::directory_iterator(root, code))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        std::ifstream in(entry.path(), std::ios::binary);
        files.emplace(entry.path().filename().generic_string(),
                      std::vector<char>{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()});
    }
    return files;
}

} // namespace

TEST_CASE("Each asset kind is claimed by the cooker that owns it")
{
    CHECK(ClaimFor("materials/checker.amat") == Claim::Output);
    CHECK(ClaimFor("config/game.json") == Claim::Output);
    CHECK(ClaimFor("levels/Test.alvl") == Claim::Output);
    CHECK(ClaimFor("blueprints/Player.abp") == Claim::Output);
    CHECK(ClaimFor("models/helmet.gltf") == Claim::Output);
    CHECK(ClaimFor("models/helmet.glb") == Claim::Output);
    CHECK(ClaimFor("textures/checker.png") == Claim::Output);
    CHECK(ClaimFor("textures/moon.jpg") == Claim::Output);
    CHECK(ClaimFor("shaders/mesh.vert.spv") == Claim::Output);
    CHECK(ClaimFor("editor/JetBrainsMono/Regular.ttf") == Claim::Output);
}

TEST_CASE("Shader sources are claimed and produce nothing")
{
    // A third answer, not a missing one: GLSL is content in the tree and absent
    // from a shipped build, and a cook that did not recognise it would fail.
    CHECK(ClaimFor("shaders/mesh.vert") == Claim::SourceOnly);
    CHECK(ClaimFor("shaders/mesh.frag") == Claim::SourceOnly);
    CHECK(ClaimFor("shaders/cluster.comp") == Claim::SourceOnly);
    CHECK(ClaimFor("shaders/mesh/material.glsl") == Claim::SourceOnly);
}

TEST_CASE("A file no cooker knows is claimed by nobody")
{
    // Which is what makes the walk refuse it. The catch-all is deliberately not
    // a catch-all: it takes named extensions, so an unknown one reaches the
    // refusal rather than being copied on a guess.
    CHECK(ClaimFor("models/Coffee_Machine.zip") == Claim::None);
    CHECK(ClaimFor("models/scene.blend") == Claim::None);
    CHECK(ClaimFor("notes.md") == Claim::None);
}

TEST_CASE("Cooking the fixture tree twice produces identical bytes")
{
    // The issue's hard requirement, and the trap is every container that
    // iterates in an order nobody chose. Two separate output directories, so the
    // second run cannot pass by skipping.
    const ScratchDir first("determinism-a");
    const ScratchDir second("determinism-b");

    const std::expected<CookReport, Assisi::Cook::CookError> one =
        CookTree(ASSISI_COOK_FIXTURE_ROOT, first.Path());
    REQUIRE_MESSAGE(one.has_value(), Explain(one));

    const std::expected<CookReport, Assisi::Cook::CookError> two =
        CookTree(ASSISI_COOK_FIXTURE_ROOT, second.Path());
    REQUIRE(two.has_value());

    CHECK(ReadCookedTree(first.Path()) == ReadCookedTree(second.Path()));
}

TEST_CASE("A second cook over an unchanged tree cooks nothing")
{
    // Incrementality, into the *same* directory so the manifest from the first
    // run is what the second reads.
    const ScratchDir out("incremental");

    const std::expected<CookReport, Assisi::Cook::CookError> first =
        CookTree(ASSISI_COOK_FIXTURE_ROOT, out.Path());
    REQUIRE_MESSAGE(first.has_value(), Explain(first));
    CHECK(first->cooked > 0);
    CHECK(first->skipped == 0);

    const std::expected<CookReport, Assisi::Cook::CookError> second =
        CookTree(ASSISI_COOK_FIXTURE_ROOT, out.Path());
    REQUIRE(second.has_value());
    CHECK(second->cooked == 0);
    CHECK(second->skipped == first->cooked);
}

TEST_CASE("A deleted blob is re-cooked even though its key still matches")
{
    // The manifest says what was produced; the tree says what is there. Trusting
    // the first alone would leave a cooked tree missing a file every later run
    // believes it already wrote.
    const ScratchDir out("missing-blob");

    const std::expected<CookReport, Assisi::Cook::CookError> first =
        CookTree(ASSISI_COOK_FIXTURE_ROOT, out.Path());
    REQUIRE(first.has_value());
    REQUIRE(first->cooked > 0);

    std::error_code code;
    std::filesystem::remove(out.Path() / (first->entries.front().guid + ".cooked"), code);
    REQUIRE_FALSE(code);

    const std::expected<CookReport, Assisi::Cook::CookError> second =
        CookTree(ASSISI_COOK_FIXTURE_ROOT, out.Path());
    REQUIRE(second.has_value());
    CHECK(second->cooked == 1);
}

TEST_CASE("The manifest names every asset that produced bytes")
{
    const ScratchDir out("manifest");

    const std::expected<CookReport, Assisi::Cook::CookError> report =
        CookTree(ASSISI_COOK_FIXTURE_ROOT, out.Path());
    REQUIRE(report.has_value());

    CHECK(report->entries.size() == report->cooked + report->skipped);
    CHECK(std::filesystem::exists(out.Path() / Assisi::Cook::kManifestFileName));

    // Sorted by virtual path, which is what makes two cooked trees diffable.
    for (std::size_t i = 1; i < report->entries.size(); ++i)
    {
        CHECK(report->entries[i - 1].vpath < report->entries[i].vpath);
    }
}

TEST_CASE("A derived id is the same on every machine and collides with nothing")
{
    using Assisi::Cook::DerivedAssetId;

    // Same path, same id — which is the whole point: a .spv's sidecar is
    // gitignored, so there is no committed GUID for two machines to agree on.
    CHECK(DerivedAssetId("shaders/mesh.vert.spv") == DerivedAssetId("shaders/mesh.vert.spv"));
    CHECK(DerivedAssetId("shaders/mesh.vert.spv") != DerivedAssetId("shaders/mesh.frag.spv"));

    for (const std::string_view path : {"shaders/mesh.vert.spv", "editor/shaders/line.frag.spv", "a", ""})
    {
        const Assisi::Core::AssetId id = DerivedAssetId(path);
        // Never nil and never in the built-in range, so it cannot be mistaken
        // for "no asset" or for a prim:// primitive.
        CHECK_FALSE(id.IsNil());
        CHECK_FALSE(id.IsReserved());
        // Version nibble 0xD, which RFC 4122 does not define — so a minted v4,
        // whose nibble is 4, can never equal one of these.
        CHECK((id.bytes[6] & 0xF0) == 0xD0);
        CHECK((id.bytes[8] & 0xE0) == 0xE0);
    }
}

TEST_CASE("A shader with no sidecar still cooks, under its derived id")
{
    // The clean-clone case. .gitignore excludes both the .spv and its .aast, so
    // a read-only scan of a fresh checkout lists neither — and a cook that only
    // walked the database would produce a tree with no shaders in it at all.
    const ScratchDir source("no-sidecar-src");
    const ScratchDir out("no-sidecar-out");

    std::error_code code;
    std::filesystem::copy(ASSISI_COOK_FIXTURE_ROOT, source.Path(),
                          std::filesystem::copy_options::recursive, code);
    REQUIRE_FALSE(code);
    std::filesystem::remove(source.Path() / "shaders" / "fullscreen.vert.spv.aast", code);
    REQUIRE_FALSE(code);

    const std::expected<CookReport, Assisi::Cook::CookError> report = CookTree(source.Path(), out.Path());
    REQUIRE_MESSAGE(report.has_value(), Explain(report));

    const Assisi::Core::AssetId expected = Assisi::Cook::DerivedAssetId("shaders/fullscreen.vert.spv");
    bool found = false;
    for (const Assisi::Cook::ManifestEntry &entry : report->entries)
    {
        if (entry.vpath == "shaders/fullscreen.vert.spv")
        {
            CHECK(entry.guid == expected.ToString());
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("A shader source with no compiled output fails the cook")
{
    // Claimed and producing nothing is not the same as "ignore it". A .spv that
    // is absent means the compile did not happen, and skipping quietly would
    // ship a tree one stage short.
    const ScratchDir source("no-spv-src");
    const ScratchDir out("no-spv-out");

    std::error_code code;
    std::filesystem::copy(ASSISI_COOK_FIXTURE_ROOT, source.Path(),
                          std::filesystem::copy_options::recursive, code);
    REQUIRE_FALSE(code);
    std::filesystem::remove(source.Path() / "shaders" / "fullscreen.vert.spv", code);
    REQUIRE_FALSE(code);
    std::filesystem::remove(source.Path() / "shaders" / "fullscreen.vert.spv.aast", code);

    const std::expected<CookReport, Assisi::Cook::CookError> report = CookTree(source.Path(), out.Path());
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().vpath == "shaders/fullscreen.vert");
    CHECK(report.error().reason.find("no compiled") != std::string::npos);
}

TEST_CASE("A file no cooker claims fails the cook, naming the path")
{
    // The definition of done, and the property the whole walk exists for: a
    // shipped build must not be able to quietly lack an asset. Driven through
    // CookTree rather than through Claims() alone, because what matters is that
    // the run *stops* and says which file.
    const ScratchDir source("unclaimed-src");
    const ScratchDir out("unclaimed-out");

    // A copy of the fixture, plus one file nothing handles.
    std::error_code code;
    std::filesystem::copy(ASSISI_COOK_FIXTURE_ROOT, source.Path(),
                          std::filesystem::copy_options::recursive, code);
    REQUIRE_FALSE(code);

    {
        std::ofstream stray(source.Path() / "notes.md");
        stray << "not an asset any cooker knows\n";
    }
    {
        std::ofstream sidecar(source.Path() / "notes.md.aast");
        sidecar << R"({"guid":"6e8a0b24-579b-4f13-8e02-468ace024680","type":"AssetSidecar","version":1})";
    }

    const std::expected<CookReport, Assisi::Cook::CookError> report = CookTree(source.Path(), out.Path());
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().vpath == "notes.md");
    CHECK(report.error().reason.find("no cooker") != std::string::npos);
}

TEST_CASE("An ignored file is neither cooked nor a failure")
{
    // .assisiignore is what says a file is not content. Without it honoured, the
    // totality rule above would refuse every authoring source sitting beside its
    // output — which is why the cooker carries no skip list of its own.
    const ScratchDir source("ignored-src");
    const ScratchDir out("ignored-out");

    std::error_code code;
    std::filesystem::copy(ASSISI_COOK_FIXTURE_ROOT, source.Path(),
                          std::filesystem::copy_options::recursive, code);
    REQUIRE_FALSE(code);

    // The fixture's .assisiignore excludes *.zip.
    {
        std::ofstream archive(source.Path() / "source-art.zip", std::ios::binary);
        archive << "PK\x03\x04";
    }

    const std::expected<CookReport, Assisi::Cook::CookError> report = CookTree(source.Path(), out.Path());
    REQUIRE_MESSAGE(report.has_value(), Explain(report));
    for (const Assisi::Cook::ManifestEntry &entry : report->entries)
    {
        CHECK(entry.vpath != "source-art.zip");
    }
}

TEST_CASE("Every cooked blob is named by its asset's GUID")
{
    const ScratchDir out("naming");

    const std::expected<CookReport, Assisi::Cook::CookError> report =
        CookTree(ASSISI_COOK_FIXTURE_ROOT, out.Path());
    REQUIRE(report.has_value());

    for (const Assisi::Cook::ManifestEntry &entry : report->entries)
    {
        CHECK(std::filesystem::exists(out.Path() / (entry.guid + ".cooked")));
    }
}
