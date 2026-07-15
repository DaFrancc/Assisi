/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <string>

#include <nlohmann/json.hpp>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/AssetIdJson.hpp>

using namespace Assisi::Core;

namespace
{
// Restores the global hint resolver to "none" so one test's installation can't
// leak into another (the resolver is process-wide state, like SceneSerializer's).
struct HintResolverGuard
{
    ~HintResolverGuard() { SetAssetIdHintResolver({}); }
};
} // namespace

TEST_CASE("SerializeAssetId writes guid only when no hint resolver is set")
{
    HintResolverGuard guard;
    SetAssetIdHintResolver({});

    const AssetId  id   = *AssetId::Parse("12345678-9abc-4def-8123-456789abcdef");
    const nlohmann::json j = SerializeAssetId(id);

    REQUIRE(j.is_object());
    CHECK(j.at("guid").get<std::string>() == "12345678-9abc-4def-8123-456789abcdef");
    CHECK_FALSE(j.contains("path"));
}

TEST_CASE("SerializeAssetId adds the path hint from the installed resolver")
{
    HintResolverGuard guard;
    SetAssetIdHintResolver([](const AssetId &) { return std::string{"textures/crate.png"}; });

    const nlohmann::json j = SerializeAssetId(*AssetId::Parse("12345678-9abc-4def-8123-456789abcdef"));
    CHECK(j.at("path").get<std::string>() == "textures/crate.png");

    // An empty hint (unknown id) is omitted, not written as "".
    SetAssetIdHintResolver([](const AssetId &) { return std::string{}; });
    CHECK_FALSE(SerializeAssetId(BuiltinAssetId::Cube).contains("path"));
}

TEST_CASE("DeserializeAssetId reads the guid and ignores the hint")
{
    HintResolverGuard guard;
    const nlohmann::json obj = {{"guid", "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"}, {"path", "stale/old/path.png"}};
    const AssetId        id  = DeserializeAssetId(obj);
    CHECK(id == *AssetId::Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"));
}

TEST_CASE("DeserializeAssetId tolerates a bare guid string")
{
    const AssetId id = DeserializeAssetId(nlohmann::json("00000000-0000-0000-0000-000000000001"));
    CHECK(id == BuiltinAssetId::Cube);
}

TEST_CASE("AssetId object round-trips through serialize/deserialize")
{
    HintResolverGuard guard;
    SetAssetIdHintResolver([](const AssetId &) { return std::string{"a/b/c.png"}; });

    const AssetId original = *AssetId::Parse("0f0e0d0c-0b0a-4908-8706-050403020100");
    CHECK(DeserializeAssetId(SerializeAssetId(original)) == original);
}

TEST_CASE("DeserializeAssetId yields nil on malformed input")
{
    CHECK(DeserializeAssetId(nlohmann::json("not-a-uuid")).IsNil());
    CHECK(DeserializeAssetId(nlohmann::json::object()).IsNil()); // no "guid"
    CHECK(DeserializeAssetId(nlohmann::json(42)).IsNil());       // wrong type
}
