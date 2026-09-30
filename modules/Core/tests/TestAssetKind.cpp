/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestAssetKind.cpp
/// @brief Kinds, the engine's own and those registered from outside it: which
/// formats each reads, which kind a new file is given, and how a kind cooks.

#include <Assisi/Core/AssetKind.hpp>
#include <Assisi/Core/AssetSidecar.hpp>
#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Testing/TestAssetKinds.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace Assisi;

namespace
{

const std::vector<std::byte> kSource{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};

/// The kind a blob's envelope names, and the payload after it.
std::pair<Core::AssetKindId, std::vector<std::byte>> Open(const std::vector<std::byte> &blob)
{
    Core::BitReader reader{blob};
    const std::expected<Core::AssetKindId, Core::CookedBlobError> kind = Core::ReadCookedHeader(reader);
    REQUIRE(kind.has_value());
    const std::span<const std::byte> payload = std::span<const std::byte>{blob}.subspan(Core::kCookedHeaderBytes);
    return {*kind, std::vector<std::byte>{payload.begin(), payload.end()}};
}

std::expected<Testing::TestBytes, Core::AssetError> LoadNothing(std::span<const std::byte>)
{
    return Testing::TestBytes{};
}

} // namespace

TEST_CASE("Every engine kind is in the registry, so every file can say it is one")
{
    const Core::AssetKindRegistry &registry = Core::AssetKindRegistry::Instance();
    for (const Core::AssetKindId kind :
         {Core::kReflectedKind, Core::kSceneKind, Core::kMeshKind, Core::kTextureKind, Core::kShaderKind,
          Core::kVerbatimKind, Core::kFontKind, Core::kScreenKind, Core::kStringTableKind})
    {
        CAPTURE(Core::DescribeKind(kind));
        CHECK(registry.Find(kind) != nullptr);
    }
    CHECK(registry.Reads(Core::kTextureKind, ".png"));
    CHECK_FALSE(registry.Reads(Core::kTextureKind, ".wav"));
}

TEST_CASE("Several kinds can read one format, listed by name")
{
    const std::vector<const Core::AssetKind *> readers = Core::AssetKindRegistry::Instance().KindsReading(".png");
    REQUIRE(readers.size() == 2);
    CHECK(readers[0]->id == Testing::kRawKind);
    CHECK(readers[1]->id == Core::kTextureKind);
}

TEST_CASE("A new file is given the kind that prefers its format, not the first by name")
{
    const Core::AssetKindRegistry &registry = Core::AssetKindRegistry::Instance();
    // "test raw bytes" sorts before "texture" and also reads .png, but only the
    // texture kind prefers it.
    CHECK(registry.KindForNewFile(".png")->id == Core::kTextureKind);
    CHECK(registry.KindForNewFile(".traw")->id == Testing::kRawKind);
    CHECK(registry.KindForNewFile(".unheardof") == nullptr);
}

TEST_CASE("A new file's sidecar states its kind; a format no kind reads gets none")
{
    const Core::AssetId id = Core::DerivedAssetId("any");

    const Core::AssetSidecar image = Core::NewFileSidecar(id, "textures/photo.png");
    REQUIRE(image.uses.size() == 1);
    CHECK(image.uses.front().kind == "texture");

    // Part of another asset rather than an asset: nothing to state.
    CHECK(Core::NewFileSidecar(id, "shaders/mesh.vert").uses.empty());
    CHECK(Core::NewFileSidecar(id, "notes.unheardof").uses.empty());
}

TEST_CASE("Files that are parts of other assets, and compiled shaders, are named by their extension")
{
    const Core::AssetKindRegistry &registry = Core::AssetKindRegistry::Instance();
    CHECK(registry.ConsumerOf(".bin")->id == Core::kMeshKind);
    CHECK(registry.ConsumerOf(".ttf")->id == Core::kFontKind);
    CHECK(registry.ConsumerOf(".png") == nullptr);
    CHECK(registry.GeneratorOf(".spv")->id == Core::kShaderKind);
    CHECK(registry.GeneratorOf(".png") == nullptr);
}

TEST_CASE("A file's extension is its last one")
{
    CHECK(Core::ExtensionOf("shaders/mesh.vert.spv") == ".spv");
    CHECK(Core::ExtensionOf("sounds/click.wav") == ".wav");
    CHECK(Core::ExtensionOf("dir.with.dots/file") == "");
    CHECK(Core::ExtensionOf(".assisiignore") == "");
}

TEST_CASE("A registered kind has a name in logs")
{
    CHECK(Core::DescribeKind(Testing::kReversedKind) == "test reversed bytes");
    CHECK(Core::DescribeKind(Core::AssetKindId{"no such kind"}).starts_with("unknown kind"));
}

TEST_CASE("A kind cannot take a name that is taken, come without a loader, or claim a generated format")
{
    Core::AssetKindRegistry &registry = Core::AssetKindRegistry::Instance();
    const Core::AssetFormat format{.extension = ".somethingelse"};

    CHECK_FALSE(
        registry.Register(Core::MakeAssetKind<Testing::TestBytes>("test reversed bytes", {format}, LoadNothing)));
    CHECK_FALSE(registry.Register(Core::MakeAssetKind<Testing::TestBytes>("mesh", {format}, LoadNothing)));

    Core::AssetKind unloadable;
    unloadable.name = "no loader";
    unloadable.id = Core::AssetKindId{unloadable.name};
    CHECK_FALSE(registry.Register(unloadable));

    CHECK_FALSE(registry.Register(Core::MakeAssetKind<Testing::TestBytes>(
        "generated", {Core::AssetFormat{.extension = ".gen", .role = Core::FormatRole::Generated}}, LoadNothing)));

    // None of the refusals left a reader behind.
    CHECK(registry.KindsReading(".somethingelse").empty());
    CHECK(registry.GeneratorOf(".gen") == nullptr);
}

TEST_CASE("A kind with no cook step ships its source unchanged behind the envelope")
{
    const Core::AssetKind *raw = Core::AssetKindRegistry::Instance().Find(Testing::kRawKind);
    REQUIRE(raw != nullptr);

    const std::expected<std::vector<std::byte>, Core::AssetError> blob = Core::CookAssetBytes(*raw, kSource);
    REQUIRE(blob.has_value());
    const std::pair<Core::AssetKindId, std::vector<std::byte>> opened = Open(*blob);
    CHECK(opened.first == Testing::kRawKind);
    CHECK(opened.second == kSource);
}

TEST_CASE("A kind with a cook step ships what the step makes of its source")
{
    const Core::AssetKind *reversed = Core::AssetKindRegistry::Instance().Find(Testing::kReversedKind);
    REQUIRE(reversed != nullptr);

    const std::expected<std::vector<std::byte>, Core::AssetError> blob = Core::CookAssetBytes(*reversed, kSource);
    REQUIRE(blob.has_value());
    const std::pair<Core::AssetKindId, std::vector<std::byte>> opened = Open(*blob);
    CHECK(opened.first == Testing::kReversedKind);
    CHECK(opened.second == std::vector<std::byte>{std::byte{'c'}, std::byte{'b'}, std::byte{'a'}});
}

TEST_CASE("A cook step's refusal comes back with its reason")
{
    const Core::AssetKind *reversed = Core::AssetKindRegistry::Instance().Find(Testing::kReversedKind);
    REQUIRE(reversed != nullptr);

    const std::vector<std::byte> refused{std::byte{'X'}, std::byte{'y'}};
    const std::expected<std::vector<std::byte>, Core::AssetError> blob = Core::CookAssetBytes(*reversed, refused);
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error() == Core::AssetErrorCode::CorruptAsset);
    CHECK(blob.error().detail == Testing::kCookRefusedDetail);
}

TEST_CASE("A sidecar's uses round-trip, a sidecar without them still reads, and two are refused")
{
    Core::AssetSidecar written = Core::AssetSidecar::Leaf(Core::DerivedAssetId("terrain.png"));
    written.uses.push_back(Core::AssetUse{.kind = "heightmap"});
    const std::expected<Core::AssetSidecar, Core::AssetSidecarError> read =
        Core::DeserializeSidecar(Core::SerializeSidecar(written));
    REQUIRE(read.has_value());
    REQUIRE(read->uses.size() == 1);
    CHECK(read->uses.front().kind == "heightmap");

    const Core::AssetSidecar leaf = Core::AssetSidecar::Leaf(Core::DerivedAssetId("old.png"));
    const std::string leafText = Core::SerializeSidecar(leaf);
    CHECK(leafText.find("uses") == std::string::npos);
    const std::expected<Core::AssetSidecar, Core::AssetSidecarError> old = Core::DeserializeSidecar(leafText);
    REQUIRE(old.has_value());
    CHECK(old->uses.empty());

    written.uses.push_back(Core::AssetUse{.kind = "texture"});
    const std::expected<Core::AssetSidecar, Core::AssetSidecarError> two =
        Core::DeserializeSidecar(Core::SerializeSidecar(written));
    REQUIRE_FALSE(two.has_value());
    CHECK(two.error() == Core::AssetSidecarError::TooManyUses);
}
