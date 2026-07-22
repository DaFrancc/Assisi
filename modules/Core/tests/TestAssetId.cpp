/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
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

    // The table exposes exactly the four primitive stand-ins (nil excluded).
    CHECK(BuiltinAssets().size() == 4);
    for (const BuiltinAssetEntry &entry : BuiltinAssets())
    {
        CHECK(entry.id.IsReserved());
        CHECK_FALSE(entry.id.IsNil());
        CHECK_FALSE(entry.virtualPath.empty());
    }
}

TEST_CASE("AssetId string round-trips")
{
    const std::string canonical = "12345678-9abc-4def-8123-456789abcdef";
    auto              parsed    = AssetId::Parse(canonical);
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
