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
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <Assisi/Cook/CookTree.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Core/Reflect/AssetTypeRegistry.hpp>
#include <Assisi/Image/Compress.hpp>
#include <Assisi/Mondrian/Font.hpp>

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
    CHECK(ClaimFor("fonts/Inter-Regular.afont") == Claim::Output);
}

TEST_CASE("Font files and their licences are claimed and produce nothing of their own")
{
    // A font's glyphs reach the cooked tree through the description that names
    // it, so neither the font file nor the licence beside it ships.
    CHECK(ClaimFor("fonts/Inter-Regular.ttf") == Claim::SourceOnly);
    CHECK(ClaimFor("fonts/Other.otf") == Claim::SourceOnly);
    CHECK(ClaimFor("fonts/OFL.txt") == Claim::SourceOnly);
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

    const Assisi::Core::AssetId expected = Assisi::Core::DerivedAssetId("shaders/fullscreen.vert.spv");
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

TEST_CASE("A cooker's kind is the kind its blobs say they are")
{
    // A provider lists assets by the kind their cooker reports, without cooking
    // them. A cooker that reports one kind and writes another would list a mesh
    // as a scene and fail the load that trusted the list.
    const ScratchDir out("kinds");

    const std::expected<CookReport, Assisi::Cook::CookError> report =
        CookTree(ASSISI_COOK_FIXTURE_ROOT, out.Path());
    REQUIRE_MESSAGE(report.has_value(), Explain(report));
    REQUIRE_FALSE(report->entries.empty());

    const std::vector<std::unique_ptr<Assisi::Cook::Cooker>> cookers = MakeCookers();
    for (const Assisi::Cook::ManifestEntry &entry : report->entries)
    {
        CAPTURE(entry.vpath);

        std::ifstream in(out.Path() / (entry.guid + ".cooked"), std::ios::binary);
        const std::vector<char> chars{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        Assisi::Core::BitReader reader(std::as_bytes(std::span{chars}));
        const std::expected<Assisi::Core::CookedKind, Assisi::Core::CookedBlobError> written =
            Assisi::Core::ReadCookedHeader(reader);
        REQUIRE(written.has_value());

        const Assisi::Cook::Cooker *owner = nullptr;
        for (const std::unique_ptr<Assisi::Cook::Cooker> &cooker : cookers)
        {
            if (cooker->Claims(entry.vpath) != Claim::None)
            {
                owner = cooker.get();
                break;
            }
        }
        REQUIRE(owner != nullptr);
        CHECK(owner->Kind() == *written);
    }
}

TEST_CASE("Changing the texture tier re-cooks the textures and nothing else")
{
    // The tier changes a texture's bytes without touching its source file. Left
    // out of the cache key, a tree cooked at the fast tier would be skipped as
    // current when the shipping cook asks for the best one.
    const ScratchDir out("texture-tier");

    const std::expected<CookReport, Assisi::Cook::CookError> best =
        CookTree(ASSISI_COOK_FIXTURE_ROOT, out.Path(), Assisi::Image::CompressQuality::Best);
    REQUIRE_MESSAGE(best.has_value(), Explain(best));

    const std::expected<CookReport, Assisi::Cook::CookError> fast =
        CookTree(ASSISI_COOK_FIXTURE_ROOT, out.Path(), Assisi::Image::CompressQuality::Fast);
    REQUIRE_MESSAGE(fast.has_value(), Explain(fast));

    // The fixture holds exactly one texture.
    constexpr std::size_t kFixtureTextures = 1;
    CHECK(fast->cooked == kFixtureTextures);
    CHECK(fast->skipped == best->cooked - kFixtureTextures);
}

namespace
{

/// A reflected type registered by the test, so the build's set of layouts
/// changes without any source file changing.
struct LayoutProbe
{
    float value = 0.f;
};

} // namespace

TEST_CASE("A change to any reflected type's layout re-cooks the reflected documents")
{
    // A document's source can stay byte-identical while the type it names gains,
    // loses or retypes a field, which changes the bytes it cooks to. Left out of
    // the cache key, the stale blob is kept and the game refuses it at load.
    const ScratchDir out("reflected-layout");

    const std::expected<CookReport, Assisi::Cook::CookError> before =
        CookTree(ASSISI_COOK_FIXTURE_ROOT, out.Path(), Assisi::Image::CompressQuality::Fast);
    REQUIRE_MESSAGE(before.has_value(), Explain(before));

    namespace Reflect = Assisi::Core::Reflect;
    Reflect::AssetTypeRegistry::Instance().Register(Reflect::AssetTypeMeta{
        "CookTestLayoutProbe",
        typeid(LayoutProbe),
        {Reflect::FieldMeta{.name = "value", .type = Reflect::FieldType::Float, .offset = 0}},
        {},
        {},
        {},
        {}});

    const std::expected<CookReport, Assisi::Cook::CookError> after =
        CookTree(ASSISI_COOK_FIXTURE_ROOT, out.Path(), Assisi::Image::CompressQuality::Fast);
    REQUIRE_MESSAGE(after.has_value(), Explain(after));

    // The fixture holds exactly one reflected document, its material.
    constexpr std::size_t kFixtureReflected = 1;
    CHECK(after->cooked == kFixtureReflected);
    CHECK(after->skipped == before->cooked - kFixtureReflected);
}

namespace
{

/// A copy of the fixture tree with a font description and its font file added,
/// both with sidecars so they have ids to cook under.
void AddFont(const std::filesystem::path &root)
{
    std::error_code code;
    std::filesystem::copy(ASSISI_COOK_FIXTURE_ROOT, root, std::filesystem::copy_options::recursive, code);
    std::filesystem::create_directories(root / "fonts", code);
    std::filesystem::copy_file(ASSISI_COOK_TEST_FONT, root / "fonts" / "Test.ttf", code);

    std::ofstream(root / "fonts" / "Test.afont")
        << R"({ "version": 1, "type": "FontDescription", "source": "fonts/Test.ttf",)"
        << R"( "ranges": [32, 126], "pixelSize": 24, "spread": 4 })";
    std::ofstream(root / "fonts" / "Test.afont.aast")
        << R"({ "guid": "5b0e3c2a-6f1d-4e8a-9c7b-2d4f6a8e0c13", "type": "AssetSidecar", "version": 1 })";
    std::ofstream(root / "fonts" / "Test.ttf.aast")
        << R"({ "guid": "9a1c7e5f-3b2d-4c6e-8f0a-1e3d5b7c9a24", "type": "AssetSidecar", "version": 1 })";
}

/// The manifest entry for @p vpath, or null.
const Assisi::Cook::ManifestEntry *EntryFor(const CookReport &report, std::string_view vpath)
{
    for (const Assisi::Cook::ManifestEntry &entry : report.entries)
    {
        if (entry.vpath == vpath)
        {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("A font description cooks to a font the runtime reads, and its font file does not ship")
{
    const ScratchDir source("font-src");
    const ScratchDir out("font-out");
    AddFont(source.Path());

    const std::expected<CookReport, Assisi::Cook::CookError> report = CookTree(source.Path(), out.Path());
    REQUIRE_MESSAGE(report.has_value(), Explain(report));

    CHECK(EntryFor(*report, "fonts/Test.ttf") == nullptr);
    const Assisi::Cook::ManifestEntry *font = EntryFor(*report, "fonts/Test.afont");
    REQUIRE(font != nullptr);

    std::ifstream in(out.Path() / (font->guid + ".cooked"), std::ios::binary);
    const std::vector<char> chars{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const std::expected<Assisi::Mondrian::Font, Assisi::Mondrian::CookedFontError> read =
        Assisi::Mondrian::ReadCookedFont(std::as_bytes(std::span{chars}));
    REQUIRE(read.has_value());
    CHECK(read->GlyphFor('A').has_value());
}

TEST_CASE("Changing a font file re-cooks the description that names it")
{
    const ScratchDir source("font-dep-src");
    const ScratchDir out("font-dep-out");
    AddFont(source.Path());

    const std::expected<CookReport, Assisi::Cook::CookError> first = CookTree(source.Path(), out.Path());
    REQUIRE_MESSAGE(first.has_value(), Explain(first));

    // Trailing bytes after the last table: still the same font to FreeType, and
    // a different file to the cache key.
    std::ofstream(source.Path() / "fonts" / "Test.ttf", std::ios::binary | std::ios::app) << '\0';

    const std::expected<CookReport, Assisi::Cook::CookError> second = CookTree(source.Path(), out.Path());
    REQUIRE_MESSAGE(second.has_value(), Explain(second));
    CHECK(second->cooked == 1);
}
