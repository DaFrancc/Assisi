/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

// doctest forward-declares std::ostream; stringifying a std::string_view in a
// CHECK instantiates operator<<(ostream&, string_view), which needs the complete
// type. Pull it in so string_view comparisons can be reported on failure.
#include <ostream>

#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>

#include <Assisi/Core/AssetPath.hpp>

using Assisi::Core::AssetPath;
using Assisi::Core::kAssetPathMax;

TEST_CASE("AssetPath: default-constructed is empty")
{
    AssetPath p;
    CHECK(p.Empty());
    CHECK(p.Size() == 0);
    CHECK(p.View() == std::string_view{});
}

TEST_CASE("AssetPath: round-trips a normal path")
{
    AssetPath p;
    CHECK(p.Assign("textures/crate.png"));
    CHECK_FALSE(p.Empty());
    CHECK(p.Size() == 18);
    CHECK(p.View() == "textures/crate.png");
}

TEST_CASE("AssetPath: constructing from string_view assigns")
{
    AssetPath p{std::string_view{"prim://cube"}};
    CHECK(p.View() == "prim://cube");
}

TEST_CASE("AssetPath: stores exactly kAssetPathMax without truncating")
{
    const std::string exact(kAssetPathMax, 'a');
    AssetPath p;
    CHECK(p.Assign(exact)); // returns true — no truncation
    CHECK(p.Size() == kAssetPathMax);
    CHECK(p.View() == exact);
}

TEST_CASE("AssetPath: truncates an over-long path and reports it")
{
    const std::string tooLong(kAssetPathMax + 50, 'x');
    AssetPath p;
    CHECK_FALSE(p.Assign(tooLong)); // returns false — truncation happened
    CHECK(p.Size() == kAssetPathMax);
    CHECK(p.View() == std::string_view(tooLong.data(), kAssetPathMax));
}

TEST_CASE("AssetPath: reassignment to a shorter path leaves no stale tail")
{
    AssetPath longP{std::string_view{"a/very/long/asset/path.png"}};
    AssetPath shortP{std::string_view{"a.png"}};

    // Assigning the long value then the short value must equal a freshly-made
    // short value — equality reads View(), so the previous tail bytes don't count.
    AssetPath p{std::string_view{"a/very/long/asset/path.png"}};
    CHECK(p.Assign("a.png"));
    CHECK(p == shortP);
    CHECK(p.View() == "a.png");
    CHECK_FALSE(p == longP);
}

TEST_CASE("AssetPath: equality and hashing agree with the view")
{
    AssetPath a{std::string_view{"models/hero.obj"}};
    AssetPath b{std::string_view{"models/hero.obj"}};
    AssetPath c{std::string_view{"models/enemy.obj"}};

    CHECK(a == b);
    CHECK_FALSE(a == c);

    const std::hash<AssetPath> hash;
    CHECK(hash(a) == hash(b));

    // Usable as a hash-set key.
    std::unordered_set<AssetPath> set;
    set.insert(a);
    set.insert(b); // duplicate of a
    set.insert(c);
    CHECK(set.size() == 2);
}

TEST_CASE("AssetPath: stays trivially copyable and compact")
{
    static_assert(std::is_trivially_copyable_v<AssetPath>,
                  "AssetPath must stay memcpy-safe for cheap component storage");
    // The char buffer plus a uint16_t length prefix (with alignment padding).
    static_assert(sizeof(AssetPath) <= kAssetPathMax + 4,
                  "AssetPath should be little more than its character buffer");
}
