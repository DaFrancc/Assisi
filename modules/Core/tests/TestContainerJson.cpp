/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestContainerJson.cpp
/// @brief Reflected containers through JSON: every permitted shape round-trips,
/// a map writes its keys sorted so an unedited save produces no diff, and a value
/// the file cannot honour is refused rather than truncated or wrapped.

#include <doctest/doctest.h>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <Assisi/Core/Reflect/ContainerJson.hpp>
#include <Assisi/Core/ShortString.hpp>

using Assisi::Core::ShortString;
using Assisi::Core::Reflect::ContainerFromJson;
using Assisi::Core::Reflect::ContainerToJson;
using Assisi::Core::Reflect::ReadContainer;

namespace
{

enum class Colour : std::uint8_t
{
    Red   = 0,
    Green = 7,
    Blue  = 9,
};

ShortString Str(std::string_view text)
{
    return ShortString{text};
}

} // namespace

TEST_CASE("ContainerJson: a vector is an array and round-trips")
{
    const std::vector<std::int32_t> source{-7, 0, 13};
    const nlohmann::json encoded = ContainerToJson(source);
    CHECK(encoded.dump() == "[-7,0,13]");

    std::vector<std::int32_t> decoded;
    REQUIRE(ContainerFromJson(encoded, "C", "numbers", decoded));
    CHECK(decoded == source);
}

TEST_CASE("ContainerJson: an enum element is its underlying value, as a scalar enum field is")
{
    const std::vector<Colour> source{Colour::Blue, Colour::Red, Colour::Green};
    const nlohmann::json encoded = ContainerToJson(source);
    CHECK(encoded.dump() == "[9,0,7]");

    std::vector<Colour> decoded;
    REQUIRE(ContainerFromJson(encoded, "C", "colours", decoded));
    CHECK(decoded == source);
}

TEST_CASE("ContainerJson: a map is an object whose keys are written sorted")
{
    // Inserted deliberately out of order. Without the sort an unordered_map emits
    // its buckets in memory order, so re-saving a level nobody edited produces a
    // diff — and two machines produce different files for the same content.
    std::unordered_map<ShortString, std::int32_t> counts;
    counts.emplace(Str("zulu"), 3);
    counts.emplace(Str("alpha"), 1);
    counts.emplace(Str("mike"), 2);

    CHECK(ContainerToJson(counts).dump() == R"({"alpha":1,"mike":2,"zulu":3})");

    std::unordered_map<ShortString, std::int32_t> decoded;
    REQUIRE(ContainerFromJson(ContainerToJson(counts), "C", "counts", decoded));
    CHECK(decoded == counts);
}

TEST_CASE("ContainerJson: an integer key round-trips through its decimal spelling")
{
    const std::map<std::int32_t, float> source{{-1, 0.5f}, {4, -2.25f}, {9, 1024.f}};
    const nlohmann::json encoded = ContainerToJson(source);
    CHECK(encoded.dump() == R"({"-1":0.5,"4":-2.25,"9":1024.0})");

    std::map<std::int32_t, float> decoded;
    REQUIRE(ContainerFromJson(encoded, "C", "weights", decoded));
    CHECK(decoded == source);
}

TEST_CASE("ContainerJson: a nested list-per-key round-trips, empty lists included")
{
    std::unordered_map<ShortString, std::vector<Colour>> bindings;
    bindings[Str("MoveForward")] = {Colour::Red, Colour::Blue};
    bindings[Str("Crouch")]      = {};

    const nlohmann::json encoded = ContainerToJson(bindings);
    CHECK(encoded.dump() == R"({"Crouch":[],"MoveForward":[0,9]})");

    std::unordered_map<ShortString, std::vector<Colour>> decoded;
    REQUIRE(ContainerFromJson(encoded, "C", "bindings", decoded));
    CHECK(decoded == bindings);

    // The empty list specifically: an action that exists with nothing bound to it
    // is not the same as an action that is absent.
    REQUIRE(decoded.find(Str("Crouch")) != decoded.end());
    CHECK(decoded.at(Str("Crouch")).empty());
}

TEST_CASE("ContainerJson: a wrong element type is refused, not coerced")
{
    std::vector<std::int32_t> decoded;
    CHECK_FALSE(ContainerFromJson(nlohmann::json::parse(R"(["nope"])"), "C", "f", decoded));

    std::vector<ShortString> strings;
    CHECK_FALSE(ContainerFromJson(nlohmann::json::parse("[12]"), "C", "f", strings));

    // An array where an object goes, and the reverse.
    std::map<std::int32_t, float> asArray;
    CHECK_FALSE(ContainerFromJson(nlohmann::json::parse("[1,2]"), "C", "f", asArray));

    std::vector<std::int32_t> asObject;
    CHECK_FALSE(ContainerFromJson(nlohmann::json::parse(R"({"a":1})"), "C", "f", asObject));
}

TEST_CASE("ContainerJson: an out-of-range narrow element is refused, not wrapped")
{
    // get<uint8_t>() would keep the low byte and load 300 as 44 with nothing said.
    std::vector<std::uint8_t> decoded;
    CHECK_FALSE(ContainerFromJson(nlohmann::json::parse("[300]"), "C", "f", decoded));

    std::vector<std::int8_t> negative;
    CHECK_FALSE(ContainerFromJson(nlohmann::json::parse("[-200]"), "C", "f", negative));
}

TEST_CASE("ContainerJson: an over-long string element is refused, not truncated")
{
    // A ShortString holds 32 bytes. A longer one is a value the file does not
    // contain, so it fails rather than silently becoming a different string.
    const std::string tooLong(33, 'a');
    std::vector<ShortString> decoded;
    CHECK_FALSE(ContainerFromJson(nlohmann::json{tooLong}, "C", "f", decoded));

    const std::string fits(32, 'a');
    std::vector<ShortString> ok;
    REQUIRE(ContainerFromJson(nlohmann::json{fits}, "C", "f", ok));
    CHECK(ok.front().View() == fits);
}

TEST_CASE("ContainerJson: a key that is not wholly a number is refused")
{
    // from_chars stops at the first non-digit, so a prefix parse would read
    // "12abc" as 12 and lose the rest.
    std::map<std::int32_t, float> decoded;
    CHECK_FALSE(ContainerFromJson(nlohmann::json::parse(R"({"12abc":1.0})"), "C", "f", decoded));
    CHECK_FALSE(ContainerFromJson(nlohmann::json::parse(R"({"":1.0})"), "C", "f", decoded));
    CHECK_FALSE(ContainerFromJson(nlohmann::json::parse(R"({"1.5":1.0})"), "C", "f", decoded));
}

TEST_CASE("ContainerJson: an absent field leaves the destination alone")
{
    // What lets a component gain a container field without refusing every file
    // written before it existed.
    std::vector<std::int32_t> existing{1, 2};
    REQUIRE(ReadContainer(nlohmann::json::object(), "C", "missing", existing));
    CHECK(existing == std::vector<std::int32_t>{1, 2});
}

TEST_CASE("ContainerJson: reading replaces the destination rather than appending")
{
    std::vector<std::int32_t> decoded{9, 9, 9, 9, 9};
    REQUIRE(ContainerFromJson(nlohmann::json::parse("[1,2]"), "C", "f", decoded));
    CHECK(decoded == std::vector<std::int32_t>{1, 2});
}
