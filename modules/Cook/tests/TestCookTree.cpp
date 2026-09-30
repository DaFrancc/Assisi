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

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <Assisi/Cook/CookTree.hpp>
#include <Assisi/Core/AssetKind.hpp>
#include <Assisi/Core/AssetSidecar.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Core/Reflect/AssetTypeRegistry.hpp>
#include <Assisi/Image/Compress.hpp>
#include <Assisi/Mondrian/Font.hpp>
#include <Assisi/Testing/TestAssetKinds.hpp>

using Assisi::Cook::CookReport;
using Assisi::Cook::CookTree;
using Assisi::Cook::MakeCookers;

namespace
{

/// How many cookers cook @p kind.
std::size_t CookersOf(Assisi::Core::AssetKindId kind)
{
    const std::vector<std::unique_ptr<Assisi::Cook::Cooker>> cookers = MakeCookers();
    return static_cast<std::size_t>(std::ranges::count_if(
        cookers, [kind](const std::unique_ptr<Assisi::Cook::Cooker> &cooker) { return cooker->Kind() == kind; }));
}

/// A copy of the fixture tree with one more file and a sidecar naming @p kind
/// (none when empty), cooked. The fixture itself stays as every other case
/// cooks it.
std::expected<CookReport, Assisi::Cook::CookError> CookWithExtraFile(const std::filesystem::path &source,
                                                                     const std::filesystem::path &out,
                                                                     const std::string &vpath, const std::string &kind)
{
    std::error_code code;
    std::filesystem::copy(ASSISI_COOK_FIXTURE_ROOT, source, std::filesystem::copy_options::recursive, code);
    REQUIRE_FALSE(code);
    {
        std::ofstream file(source / vpath, std::ios::binary);
        file << "extra";
    }
    {
        std::ofstream sidecar(source / (vpath + ".aast"));
        sidecar << R"({"guid":"5e2c9b7a-0f41-4d86-b3a2-8c1e7d6f4a90","type":"AssetSidecar","version":1)";
        if (!kind.empty())
        {
            sidecar << R"(,"uses":[{"kind":")" << kind << R"("}])";
        }
        sidecar << "}";
    }
    return CookTree(source, out);
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

    ScratchDir(const ScratchDir &) = delete;
    ScratchDir &operator=(const ScratchDir &) = delete;
    ScratchDir(ScratchDir &&) = delete;
    ScratchDir &operator=(ScratchDir &&) = delete;

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

std::vector<char> ReadFile(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("Every kind, the engine's own and a module's, has exactly one cooker")
{
    // A file's sidecar names its kind and the cook hands it to that kind's
    // cooker, so a kind with none has files nothing can cook, and a kind with
    // two has files the cook would hand to whichever it found first.
    for (const Assisi::Core::AssetKind &kind : Assisi::Core::AssetKindRegistry::Instance().All())
    {
        CAPTURE(kind.name);
        CHECK(CookersOf(kind.id) == 1);
    }
}

TEST_CASE("A file whose sidecar names no kind fails the cook, naming it")
{
    const ScratchDir source("no-kind-src");
    const ScratchDir out("no-kind-out");
    const std::expected<CookReport, Assisi::Cook::CookError> report =
        CookWithExtraFile(source.Path(), out.Path(), "things/unstated.tbytes", "");
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().vpath == "things/unstated.tbytes");
    CHECK(report.error().reason.find("names no kind") != std::string::npos);
}

TEST_CASE("A file used as a kind that does not read its format fails the cook, naming both")
{
    const ScratchDir source("wrong-kind-src");
    const ScratchDir out("wrong-kind-out");
    const std::expected<CookReport, Assisi::Cook::CookError> report =
        CookWithExtraFile(source.Path(), out.Path(), "things/wrong.png", "test reversed bytes");
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().vpath == "things/wrong.png");
    CHECK(report.error().reason.find("test reversed bytes") != std::string::npos);
}

TEST_CASE("A file used as a kind this build does not have fails the cook, naming both")
{
    const ScratchDir source("unknown-kind-src");
    const ScratchDir out("unknown-kind-out");
    const std::expected<CookReport, Assisi::Cook::CookError> report =
        CookWithExtraFile(source.Path(), out.Path(), "things/odd.png", "heightmap");
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().vpath == "things/odd.png");
    CHECK(report.error().reason.find("heightmap") != std::string::npos);
}

TEST_CASE("A part of another asset is cooked with it, not as an asset of its own")
{
    // A glTF's buffer has no kind of its own to name, so a sidecar with none
    // is right for it; it produces nothing, and nothing fails.
    const ScratchDir source("part-src");
    const ScratchDir out("part-out");
    const std::expected<CookReport, Assisi::Cook::CookError> report =
        CookWithExtraFile(source.Path(), out.Path(), "things/mesh.bin", "");
    REQUIRE_MESSAGE(report.has_value(), Explain(report));
    for (const Assisi::Cook::ManifestEntry &entry : report->entries)
    {
        CHECK(entry.vpath != "things/mesh.bin");
    }
}

TEST_CASE("A PNG whose sidecar says it is another kind cooks as that kind, and one that says texture as a texture")
{
    const ScratchDir out("png-kinds");
    const std::expected<CookReport, Assisi::Cook::CookError> report = CookTree(ASSISI_COOK_FIXTURE_ROOT, out.Path());
    REQUIRE_MESSAGE(report.has_value(), Explain(report));

    const auto kindOf = [&](const std::string &vpath) -> Assisi::Core::AssetKindId
    {
        const std::vector<Assisi::Cook::ManifestEntry>::const_iterator entry =
            std::ranges::find(report->entries, vpath, &Assisi::Cook::ManifestEntry::vpath);
        REQUIRE(entry != report->entries.end());
        const std::vector<char> cooked = ReadFile(out.Path() / (entry->guid + ".cooked"));
        Assisi::Core::BitReader reader(std::as_bytes(std::span{cooked}));
        const std::expected<Assisi::Core::AssetKindId, Assisi::Core::CookedBlobError> kind =
            Assisi::Core::ReadCookedHeader(reader);
        REQUIRE(kind.has_value());
        return *kind;
    };
    CHECK(kindOf("things/photo.png") == Assisi::Testing::kRawKind);
    CHECK(kindOf("textures/checker.png") == Assisi::Core::kTextureKind);
}

TEST_CASE("A material that binds a file used as something other than a texture fails the cook, naming both")
{
    const ScratchDir source("bound-src");
    const ScratchDir out("bound-out");
    std::error_code code;
    std::filesystem::copy(ASSISI_COOK_FIXTURE_ROOT, source.Path(), std::filesystem::copy_options::recursive, code);
    REQUIRE_FALSE(code);
    // Point the fixture material's base colour at the PNG used as a test kind.
    {
        std::ofstream sidecar(source.Path() / "textures" / "checker.png.aast");
        sidecar << R"({"guid":"1c7fa8f9-bd8e-4f5d-b402-edadb14dc57d","type":"AssetSidecar","version":1,)"
                   R"("uses":[{"kind":"test raw bytes"}]})";
    }

    const std::expected<CookReport, Assisi::Cook::CookError> report = CookTree(source.Path(), out.Path());
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().vpath == "textures/checker.png");
    CHECK(report.error().reason.find("materials/checker.amat") != std::string::npos);
    CHECK(report.error().reason.find("test raw bytes") != std::string::npos);
}

TEST_CASE("Changing a file's kind re-cooks it as the new kind")
{
    const ScratchDir source("rekind-src");
    const ScratchDir out("rekind-out");
    std::error_code code;
    std::filesystem::copy(ASSISI_COOK_FIXTURE_ROOT, source.Path(), std::filesystem::copy_options::recursive, code);
    REQUIRE_FALSE(code);
    REQUIRE(CookTree(source.Path(), out.Path()).has_value());

    {
        std::ofstream sidecar(source.Path() / "things" / "photo.png.aast");
        sidecar << R"({"guid":"9daec218-7b41-4d98-9e25-21eaece14173","type":"AssetSidecar","version":1,)"
                   R"("uses":[{"kind":"texture"}]})";
    }
    const std::expected<CookReport, Assisi::Cook::CookError> second = CookTree(source.Path(), out.Path());
    REQUIRE_MESSAGE(second.has_value(), Explain(second));

    const std::vector<char> cooked = ReadFile(out.Path() / "9daec218-7b41-4d98-9e25-21eaece14173.cooked");
    Assisi::Core::BitReader reader(std::as_bytes(std::span{cooked}));
    const std::expected<Assisi::Core::AssetKindId, Assisi::Core::CookedBlobError> kind =
        Assisi::Core::ReadCookedHeader(reader);
    REQUIRE(kind.has_value());
    CHECK(*kind == Assisi::Core::kTextureKind);
}

TEST_CASE("A registered kind's file cooks through the kind's own step")
{
    const ScratchDir out("registered-kind");

    const std::expected<CookReport, Assisi::Cook::CookError> report = CookTree(ASSISI_COOK_FIXTURE_ROOT, out.Path());
    REQUIRE_MESSAGE(report.has_value(), Explain(report));

    const std::vector<Assisi::Cook::ManifestEntry>::const_iterator entry =
        std::ranges::find(report->entries, std::string{"things/sample.tbytes"}, &Assisi::Cook::ManifestEntry::vpath);
    REQUIRE(entry != report->entries.end());

    // The fixture holds "abc", and the test kind's step reverses it.
    const std::vector<char> cooked = ReadFile(out.Path() / (entry->guid + ".cooked"));
    REQUIRE(cooked.size() == Assisi::Core::kCookedHeaderBytes + 3);
    CHECK(std::string{cooked.begin() + Assisi::Core::kCookedHeaderBytes, cooked.end()} == "cba");
}

TEST_CASE("A registered kind's cook step refusing a file fails the cook, naming the path and the reason")
{
    const ScratchDir source("refused-kind-src");
    const ScratchDir out("refused-kind-out");

    std::error_code code;
    std::filesystem::copy(ASSISI_COOK_FIXTURE_ROOT, source.Path(), std::filesystem::copy_options::recursive, code);
    REQUIRE_FALSE(code);

    // Written here rather than kept in the fixture tree, which every other case
    // cooks whole. The test kind's step refuses a source that starts with 'X'.
    {
        std::ofstream refused(source.Path() / "things" / "refused.tbytes", std::ios::binary);
        refused << "Xyz";
    }
    {
        std::ofstream sidecar(source.Path() / "things" / "refused.tbytes.aast");
        sidecar << R"({"guid":"2bc56ba6-b801-47f0-b6bc-5b866a8c5e96","type":"AssetSidecar","version":1,)"
                   R"("uses":[{"kind":"test reversed bytes"}]})";
    }

    const std::expected<CookReport, Assisi::Cook::CookError> report = CookTree(source.Path(), out.Path());
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().vpath == "things/refused.tbytes");
    CHECK(report.error().reason.find(Assisi::Testing::kCookRefusedDetail) != std::string::npos);
}

TEST_CASE("Cooking the fixture tree twice produces identical bytes")
{
    // The issue's hard requirement, and the trap is every container that
    // iterates in an order nobody chose. Two separate output directories, so the
    // second run cannot pass by skipping.
    const ScratchDir first("determinism-a");
    const ScratchDir second("determinism-b");

    const std::expected<CookReport, Assisi::Cook::CookError> one = CookTree(ASSISI_COOK_FIXTURE_ROOT, first.Path());
    REQUIRE_MESSAGE(one.has_value(), Explain(one));

    const std::expected<CookReport, Assisi::Cook::CookError> two = CookTree(ASSISI_COOK_FIXTURE_ROOT, second.Path());
    REQUIRE(two.has_value());

    CHECK(ReadCookedTree(first.Path()) == ReadCookedTree(second.Path()));
}

TEST_CASE("A second cook over an unchanged tree cooks nothing")
{
    // Incrementality, into the *same* directory so the manifest from the first
    // run is what the second reads.
    const ScratchDir out("incremental");

    const std::expected<CookReport, Assisi::Cook::CookError> first = CookTree(ASSISI_COOK_FIXTURE_ROOT, out.Path());
    REQUIRE_MESSAGE(first.has_value(), Explain(first));
    CHECK(first->cooked > 0);
    CHECK(first->skipped == 0);

    const std::expected<CookReport, Assisi::Cook::CookError> second = CookTree(ASSISI_COOK_FIXTURE_ROOT, out.Path());
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

    const std::expected<CookReport, Assisi::Cook::CookError> first = CookTree(ASSISI_COOK_FIXTURE_ROOT, out.Path());
    REQUIRE(first.has_value());
    REQUIRE(first->cooked > 0);

    std::error_code code;
    std::filesystem::remove(out.Path() / (first->entries.front().guid + ".cooked"), code);
    REQUIRE_FALSE(code);

    const std::expected<CookReport, Assisi::Cook::CookError> second = CookTree(ASSISI_COOK_FIXTURE_ROOT, out.Path());
    REQUIRE(second.has_value());
    CHECK(second->cooked == 1);
}

TEST_CASE("The manifest names every asset that produced bytes")
{
    const ScratchDir out("manifest");

    const std::expected<CookReport, Assisi::Cook::CookError> report = CookTree(ASSISI_COOK_FIXTURE_ROOT, out.Path());
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
    std::filesystem::copy(ASSISI_COOK_FIXTURE_ROOT, source.Path(), std::filesystem::copy_options::recursive, code);
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
    std::filesystem::copy(ASSISI_COOK_FIXTURE_ROOT, source.Path(), std::filesystem::copy_options::recursive, code);
    REQUIRE_FALSE(code);
    std::filesystem::remove(source.Path() / "shaders" / "fullscreen.vert.spv", code);
    REQUIRE_FALSE(code);
    std::filesystem::remove(source.Path() / "shaders" / "fullscreen.vert.spv.aast", code);

    const std::expected<CookReport, Assisi::Cook::CookError> report = CookTree(source.Path(), out.Path());
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().vpath == "shaders/fullscreen.vert");
    CHECK(report.error().reason.find("no compiled") != std::string::npos);
}

TEST_CASE("A file of a format no kind reads fails the cook, naming the path")
{
    // The property the whole walk exists for: a shipped build must not be able
    // to quietly lack an asset. What matters is that the run *stops* and says
    // which file.
    const ScratchDir source("unclaimed-src");
    const ScratchDir out("unclaimed-out");

    // A copy of the fixture, plus one file nothing handles.
    std::error_code code;
    std::filesystem::copy(ASSISI_COOK_FIXTURE_ROOT, source.Path(), std::filesystem::copy_options::recursive, code);
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
    CHECK(report.error().reason.find("names no kind") != std::string::npos);
}

TEST_CASE("A screen naming an event nothing declares fails the cook, with the line and column")
{
    // What cooking a screen is *for*. The same check runs at load, but a load
    // failure happens on a player's machine; this one happens on the machine
    // that made the mistake, and says where in the file it is.
    const ScratchDir source("screen-src");
    const ScratchDir out("screen-out");

    std::error_code code;
    std::filesystem::copy(ASSISI_COOK_FIXTURE_ROOT, source.Path(), std::filesystem::copy_options::recursive, code);
    REQUIRE_FALSE(code);

    // Written here rather than kept in the fixture tree, because every other
    // case cooks that tree whole and a file that fails by design would break
    // all of them.
    const std::filesystem::path screen = source.Path() / "ui" / "Misspelt.amdn";
    {
        std::ofstream markup(screen);
        markup << "<screen>\n"
                  "  <button\n"
                  "          on_click=\"Game::NoSuchEvent\">Quit</button>\n"
                  "</screen>\n";
    }
    {
        std::ofstream sidecar(source.Path() / "ui" / "Misspelt.amdn.aast");
        sidecar << R"({"guid":"b74e0c15-92af-4d63-8e10-5a7c3f0d29b8","type":"AssetSidecar","version":1,)"
                   R"("uses":[{"kind":"screen"}]})";
    }

    const std::expected<CookReport, Assisi::Cook::CookError> report = CookTree(source.Path(), out.Path());
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().vpath == "ui/Misspelt.amdn");
    // The attribute is on line 3, which is what an editor jumps to.
    CHECK(report.error().reason.starts_with("3:"));
    CHECK(report.error().reason.find("Game::NoSuchEvent") != std::string::npos);
}

TEST_CASE("An ignored file is neither cooked nor a failure")
{
    // .assisiignore is what says a file is not content. Without it honoured, the
    // totality rule above would refuse every authoring source sitting beside its
    // output — which is why the cooker carries no skip list of its own.
    const ScratchDir source("ignored-src");
    const ScratchDir out("ignored-out");

    std::error_code code;
    std::filesystem::copy(ASSISI_COOK_FIXTURE_ROOT, source.Path(), std::filesystem::copy_options::recursive, code);
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

    const std::expected<CookReport, Assisi::Cook::CookError> report = CookTree(ASSISI_COOK_FIXTURE_ROOT, out.Path());
    REQUIRE(report.has_value());

    for (const Assisi::Cook::ManifestEntry &entry : report->entries)
    {
        CHECK(std::filesystem::exists(out.Path() / (entry.guid + ".cooked")));
    }
}

TEST_CASE("A cooker's kind is the kind its blobs say they are")
{
    // A pak lists assets by kind, and a load checks the kind in the blob. A
    // cooker that wrote a kind other than the one the sidecar named would list
    // a mesh as a scene and fail the load that trusted the list.
    const ScratchDir out("kinds");

    const std::expected<CookReport, Assisi::Cook::CookError> report = CookTree(ASSISI_COOK_FIXTURE_ROOT, out.Path());
    REQUIRE_MESSAGE(report.has_value(), Explain(report));
    REQUIRE_FALSE(report->entries.empty());

    for (const Assisi::Cook::ManifestEntry &entry : report->entries)
    {
        CAPTURE(entry.vpath);

        const std::vector<char> chars = ReadFile(out.Path() / (entry.guid + ".cooked"));
        Assisi::Core::BitReader reader(std::as_bytes(std::span{chars}));
        const std::expected<Assisi::Core::AssetKindId, Assisi::Core::CookedBlobError> written =
            Assisi::Core::ReadCookedHeader(reader);
        REQUIRE(written.has_value());

        // The kind the sidecar names; a compiled shader has none and is a shader.
        const std::vector<char> sidecarText =
            ReadFile(std::filesystem::path{ASSISI_COOK_FIXTURE_ROOT} / (entry.vpath + ".aast"));
        const std::expected<Assisi::Core::AssetSidecar, Assisi::Core::AssetSidecarError> sidecar =
            Assisi::Core::DeserializeSidecar(std::string_view{sidecarText.data(), sidecarText.size()});
        const Assisi::Core::AssetKindId expected = sidecar.has_value() && !sidecar->uses.empty()
                                                       ? Assisi::Core::AssetKindId{sidecar->uses.front().kind}
                                                       : Assisi::Core::kShaderKind;
        CHECK(*written == expected);
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
    Reflect::AssetTypeRegistry::Instance().Register(
        Reflect::AssetTypeMeta{"CookTestLayoutProbe",
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

/// The sidecar the editor writes beside a new file at @p path under @p root:
/// @p guid, and the kind a new file of its format is given.
void WriteNewFileSidecar(const std::filesystem::path &root, const std::string &path, std::string_view guid)
{
    const std::optional<Assisi::Core::AssetId> id = Assisi::Core::AssetId::Parse(guid);
    REQUIRE(id.has_value());
    std::ofstream(root / (path + ".aast")) << Assisi::Core::SerializeSidecar(Assisi::Core::NewFileSidecar(*id, path));
}

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
    WriteNewFileSidecar(root, "fonts/Test.afont", "5b0e3c2a-6f1d-4e8a-9c7b-2d4f6a8e0c13");
    WriteNewFileSidecar(root, "fonts/Test.ttf", "9a1c7e5f-3b2d-4c6e-8f0a-1e3d5b7c9a24");
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

namespace
{

/// Writes @p text to @p path under @p root, with the sidecar the editor would
/// give a new file there: @p guid, and its format's kind.
void WriteAsset(const std::filesystem::path &root, const std::string &path, std::string_view text,
                std::string_view guid)
{
    std::error_code code;
    std::filesystem::create_directories((root / path).parent_path(), code);
    std::ofstream(root / path) << text;
    WriteNewFileSidecar(root, path, guid);
}

/// A copy of the fixture tree with a template library and two screens built
/// from it.
void AddLibrary(const std::filesystem::path &root)
{
    std::error_code code;
    std::filesystem::copy(ASSISI_COOK_FIXTURE_ROOT, root, std::filesystem::copy_options::recursive, code);
    WriteAsset(root, "ui/common/Controls.amdt",
               "<templates>\n  <template name=\"menu_button\"><button padding=\"8\" /></template>\n</templates>\n",
               "2f6a1c9e-7b3d-4e58-a0c2-9d4e6b8f1a37");
    WriteAsset(root, "ui/Menu.amdn",
               "<screen>\n  <import path=\"ui/common/Controls.amdt\" />\n  <menu_button>Play</menu_button>\n"
               "</screen>\n",
               "8c1e5a3f-2d7b-4f96-b4a8-6e0c3d9f5b12");
    WriteAsset(root, "ui/Options.amdn",
               "<screen>\n  <import path=\"ui/common/Controls.amdt\" />\n  <menu_button>Back</menu_button>\n"
               "</screen>\n",
               "4d9b7e2a-6c1f-4a83-9e5d-0b7a2c8f4e61");
}

} // namespace

TEST_CASE("Changing a template library re-cooks every screen built from it")
{
    const ScratchDir source("library-src");
    const ScratchDir out("library-out");
    AddLibrary(source.Path());

    const std::expected<CookReport, Assisi::Cook::CookError> first = CookTree(source.Path(), out.Path());
    REQUIRE_MESSAGE(first.has_value(), Explain(first));
    CHECK(EntryFor(*first, "ui/common/Controls.amdt") == nullptr);

    std::ofstream(source.Path() / "ui" / "common" / "Controls.amdt", std::ios::app) << "<!-- edited -->\n";

    const std::expected<CookReport, Assisi::Cook::CookError> second = CookTree(source.Path(), out.Path());
    REQUIRE_MESSAGE(second.has_value(), Explain(second));
    CHECK(second->cooked == 2);
}

TEST_CASE("A broken template library fails the cook though no screen uses it")
{
    const ScratchDir source("broken-library-src");
    const ScratchDir out("broken-library-out");
    std::error_code code;
    std::filesystem::copy(ASSISI_COOK_FIXTURE_ROOT, source.Path(), std::filesystem::copy_options::recursive, code);
    WriteAsset(source.Path(), "ui/Unused.amdt",
               "<templates>\n  <template name=\"t\">\n    <button colour=\"#ff0000\" />\n  </template>\n</templates>\n",
               "a3e7c1d9-5f2b-4c86-8d0e-7b9f1a4c6e23");

    const std::expected<CookReport, Assisi::Cook::CookError> report = CookTree(source.Path(), out.Path());
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().vpath == "ui/Unused.amdt");
    CHECK(report.error().reason.find("3:") != std::string::npos);
    CHECK(report.error().reason.find("colour") != std::string::npos);
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

namespace
{

/// A copy of the fixture tree with UI settings listing one string table, the
/// table, and a screen whose title comes from it. @p screenText is the title's
/// text as the screen writes it.
void AddStringTable(const std::filesystem::path &root, std::string_view screenText)
{
    std::error_code code;
    std::filesystem::copy(ASSISI_COOK_FIXTURE_ROOT, root, std::filesystem::copy_options::recursive, code);
    WriteAsset(root, "config/ui.json", R"({ "version": 1, "type": "UiConfig", "stringTables": ["ui/menu.csv"] })",
               "4c2e8a6f-1d3b-4f5a-9e7c-0b2d4f6a8c1e");
    WriteAsset(root, "ui/menu.csv", "key,en\ntitle,Main menu\n", "7e1a3c5b-9d2f-4a6e-8c0b-3f5d7a9c1e2b");
    WriteAsset(root, "ui/Menu.amdn", "<screen>\n  <text>" + std::string{screenText} + "</text>\n</screen>\n",
               "2b4d6f8a-0c1e-4a3c-8e5f-7a9b1d3f5e6c");
}

} // namespace

TEST_CASE("A listed string table cooks, and changing it re-cooks every screen")
{
    const ScratchDir source("table-src");
    const ScratchDir out("table-out");
    AddStringTable(source.Path(), "#menu:title");

    const std::expected<CookReport, Assisi::Cook::CookError> first = CookTree(source.Path(), out.Path());
    REQUIRE_MESSAGE(first.has_value(), Explain(first));
    REQUIRE(EntryFor(*first, "ui/menu.csv") != nullptr);

    std::ofstream(source.Path() / "ui" / "menu.csv", std::ios::app) << "subtitle,Choose\n";

    // The table, and both screens: each is checked against every table.
    const std::expected<CookReport, Assisi::Cook::CookError> second = CookTree(source.Path(), out.Path());
    REQUIRE_MESSAGE(second.has_value(), Explain(second));
    CHECK(second->cooked == 3);
}

TEST_CASE("A key its table does not have fails the screen's cook")
{
    const ScratchDir source("table-key-src");
    const ScratchDir out("table-key-out");
    AddStringTable(source.Path(), "#menu:missing");

    const std::expected<CookReport, Assisi::Cook::CookError> report = CookTree(source.Path(), out.Path());
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().vpath == "ui/Menu.amdn");
    CHECK(report.error().reason.starts_with("2:"));
}
