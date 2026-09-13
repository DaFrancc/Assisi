/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>

#include <Assisi/Core/AssetDatabase.hpp>
#include <Assisi/Core/AssetId.hpp>

using namespace Assisi::Core;

TEST_CASE("AssetId default is nil and reserved")
{
    AssetId id{};
    CHECK(id.IsNil());
    CHECK(id.IsReserved());
    CHECK(id == BuiltinAssetId::Nil);
    CHECK(id.ToString() == "00000000-0000-0000-0000-000000000000");
}

TEST_CASE("Built-in ids are reserved, distinct, and not nil")
{
    CHECK(BuiltinAssetId::Cube.IsReserved());
    CHECK_FALSE(BuiltinAssetId::Cube.IsNil());
    CHECK(BuiltinAssetId::Cube != BuiltinAssetId::White);
    CHECK(BuiltinAssetId::Cube.ToString() == "00000000-0000-0000-0000-000000000001");

    // Nil excluded; every other reserved id is a resolvable primitive.
    CHECK(BuiltinAssets().size() == 12);
    for (const BuiltinAssetEntry &entry : BuiltinAssets())
    {
        CHECK(entry.id.IsReserved());
        CHECK_FALSE(entry.id.IsNil());
        CHECK_FALSE(entry.virtualPath.empty());
    }
}

// Both halves of the table are load-bearing and are written out by hand in two
// different files, so a rung added to one and forgotten in the other is the
// mistake worth catching. An id collision would silently alias two primitives
// onto one mesh; a duplicate path would do the same through the other lookup.
// Levels store these ids, so a *changed* id is worse than either: it silently
// repoints committed content at different geometry.
TEST_CASE("The built-in asset table has no duplicate ids or paths")
{
    std::set<std::string> ids;
    std::set<std::string_view> paths;
    for (const BuiltinAssetEntry &entry : BuiltinAssets())
    {
        CAPTURE(entry.virtualPath);
        CHECK(ids.insert(entry.id.ToString()).second);
        CHECK(paths.insert(entry.virtualPath).second);
    }
}

TEST_CASE("Built-in ids are stable — levels on disk store these")
{
    CHECK(BuiltinAssetId::Cube.ToString() == "00000000-0000-0000-0000-000000000001");
    CHECK(BuiltinAssetId::White.ToString() == "00000000-0000-0000-0000-000000000002");
    CHECK(BuiltinAssetId::WhiteLinear.ToString() == "00000000-0000-0000-0000-000000000003");
    CHECK(BuiltinAssetId::FlatNormal.ToString() == "00000000-0000-0000-0000-000000000004");
    CHECK(BuiltinAssetId::SphereLow.ToString() == "00000000-0000-0000-0000-000000000005");
    CHECK(BuiltinAssetId::Sphere.ToString() == "00000000-0000-0000-0000-000000000006");
    CHECK(BuiltinAssetId::SphereHigh.ToString() == "00000000-0000-0000-0000-000000000007");
    CHECK(BuiltinAssetId::IcosphereLow.ToString() == "00000000-0000-0000-0000-000000000008");
    CHECK(BuiltinAssetId::Icosphere.ToString() == "00000000-0000-0000-0000-000000000009");
    CHECK(BuiltinAssetId::IcosphereHigh.ToString() == "00000000-0000-0000-0000-00000000000a");
    CHECK(BuiltinAssetId::Cylinder.ToString() == "00000000-0000-0000-0000-00000000000b");
    CHECK(BuiltinAssetId::CylinderHigh.ToString() == "00000000-0000-0000-0000-00000000000c");
}

TEST_CASE("AssetId string round-trips")
{
    const std::string canonical = "12345678-9abc-4def-8123-456789abcdef";
    auto parsed    = AssetId::Parse(canonical);
    REQUIRE(parsed.has_value());
    CHECK(parsed->ToString() == canonical);
    CHECK_FALSE(parsed->IsReserved());
}

TEST_CASE("AssetId::Parse accepts dashless and uppercase, rejects malformed")
{
    auto dashless = AssetId::Parse("123456789abc4def8123456789abcdef");
    auto dashed   = AssetId::Parse("12345678-9abc-4def-8123-456789abcdef");
    REQUIRE(dashless.has_value());
    REQUIRE(dashed.has_value());
    CHECK(*dashless == *dashed);

    // Case-insensitive.
    auto upper = AssetId::Parse("12345678-9ABC-4DEF-8123-456789ABCDEF");
    REQUIRE(upper.has_value());
    CHECK(*upper == *dashed);

    CHECK_FALSE(AssetId::Parse("").has_value());              // empty
    CHECK_FALSE(AssetId::Parse("not-a-uuid").has_value());    // non-hex
    CHECK_FALSE(AssetId::Parse("12345678-9abc-4def").has_value()); // too short
    CHECK_FALSE(AssetId::Parse(std::string(33, 'a')).has_value()); // too long
}

TEST_CASE("MintAssetId yields unique, non-reserved, version-4 ids")
{
    std::unordered_set<AssetId> seen;
    for (int32_t i = 0; i < 1000; ++i)
    {
        const AssetId id = MintAssetId();
        CHECK_FALSE(id.IsReserved());
        CHECK_FALSE(id.IsNil());
        // RFC 4122: version nibble == 4, variant top bits == 0b10.
        CHECK((id.bytes[6] >> 4) == 0x4);
        CHECK((id.bytes[8] >> 6) == 0x2);
        // A freshly minted id round-trips through its string form.
        CHECK(AssetId::Parse(id.ToString()) == id);
        CHECK(seen.insert(id).second); // never a duplicate
    }
}
