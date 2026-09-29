/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestAssetKind.cpp
/// @brief A kind registered from outside the engine is found by its files and
/// its id, cooks through its own step, and cannot take a name or an extension
/// something else already has.

#include <Assisi/Core/AssetKind.hpp>
#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Testing/TestAssetKinds.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <expected>
#include <span>
#include <string>
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

std::expected<Testing::TestBytes, std::string> LoadNothing(std::span<const std::byte>)
{
    return Testing::TestBytes{};
}

} // namespace

TEST_CASE("A registered kind is found by the extension of its files and by its id")
{
    const Core::AssetKindRegistry &registry = Core::AssetKindRegistry::Instance();

    const Core::AssetKind *byPath = registry.ForPath("things/sample.tbytes");
    REQUIRE(byPath != nullptr);
    CHECK(byPath->id == Testing::kReversedKind);
    CHECK(registry.Find(Testing::kReversedKind) == byPath);
    CHECK(byPath->valueType == typeid(Testing::TestBytes));

    CHECK(registry.ForPath("things/sample.unregistered") == nullptr);
    // Matched as the engine's own cookers match theirs: exactly.
    CHECK(registry.ForPath("things/sample.TBYTES") == nullptr);
    // A file called only its extension has no name to be a file of that kind.
    CHECK(registry.ForPath(".tbytes") == nullptr);
    CHECK(registry.Find(Core::AssetKindId{"no such kind"}) == nullptr);
}

TEST_CASE("A registered kind has a name in logs")
{
    CHECK(Core::DescribeKind(Testing::kReversedKind) == "test reversed bytes");
    CHECK(Core::DescribeKind(Core::AssetKindId{"no such kind"}).starts_with("unknown kind"));
}

TEST_CASE("A kind cannot take a name or an extension another kind has, or a built-in name")
{
    Core::AssetKindRegistry &registry = Core::AssetKindRegistry::Instance();

    CHECK_FALSE(registry.Register(
        Core::MakeAssetKind<Testing::TestBytes>("test reversed bytes", {".somethingelse"}, LoadNothing)));
    CHECK_FALSE(registry.Register(Core::MakeAssetKind<Testing::TestBytes>("another name", {".tbytes"}, LoadNothing)));
    CHECK_FALSE(registry.Register(Core::MakeAssetKind<Testing::TestBytes>("mesh", {".notamesh"}, LoadNothing)));

    // None of the refusals took the extension it asked for.
    CHECK(registry.ForPath("a.somethingelse") == nullptr);
    CHECK(registry.ForPath("a.notamesh") == nullptr);
    CHECK(registry.ForPath("a.tbytes")->id == Testing::kReversedKind);
}

TEST_CASE("A kind with no cook step ships its source unchanged behind the envelope")
{
    const Core::AssetKind *raw = Core::AssetKindRegistry::Instance().Find(Testing::kRawKind);
    REQUIRE(raw != nullptr);

    const std::expected<std::vector<std::byte>, std::string> blob = Core::CookAssetBytes(*raw, kSource);
    REQUIRE(blob.has_value());
    const std::pair<Core::AssetKindId, std::vector<std::byte>> opened = Open(*blob);
    CHECK(opened.first == Testing::kRawKind);
    CHECK(opened.second == kSource);
}

TEST_CASE("A kind with a cook step ships what the step makes of its source")
{
    const Core::AssetKind *reversed = Core::AssetKindRegistry::Instance().Find(Testing::kReversedKind);
    REQUIRE(reversed != nullptr);

    const std::expected<std::vector<std::byte>, std::string> blob = Core::CookAssetBytes(*reversed, kSource);
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
    const std::expected<std::vector<std::byte>, std::string> blob = Core::CookAssetBytes(*reversed, refused);
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error() == "the source asks the cook to fail");
}
