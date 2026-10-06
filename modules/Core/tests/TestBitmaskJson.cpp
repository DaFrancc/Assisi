/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBitmaskJson.cpp
/// @brief A bitmask field's file form: "All", or the names of its bits.

#include <doctest/doctest.h>

#include <Assisi/Core/Reflect/BitmaskJson.hpp>
#include <Assisi/Core/Reflect/EnumLabels.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>

using namespace Assisi::Core::Reflect;

namespace
{

/// An enum of three named bits, with room for more that only a label can name.
constexpr EnumName kFruit[] = {{"Apple", 0}, {"Pear", 1}, {"Plum", 2}};
constexpr std::uint32_t kAllFruit = 0b111u;
const BitmaskNames kFruitMask{kFruit, "Test::Fruit", kAllFruit};

/// Reads @p text's "mask" field into @p out.
bool Read(const char *text, std::uint32_t &out)
{
    return ReadBitmask(nlohmann::json::parse(text), "Test", "mask", kFruitMask, out);
}

} // namespace

TEST_CASE("Every bit set is written as All, and All reads back as every bit")
{
    CHECK(BitmaskToJson(kAllFruit, kFruitMask) == "All");

    std::uint32_t bits = 0;
    REQUIRE(Read(R"({"mask": "All"})", bits));
    CHECK(bits == kAllFruit);
}

TEST_CASE("Some bits set are written as their names, in bit order, and read back")
{
    const nlohmann::json written = BitmaskToJson(0b101u, kFruitMask);
    CHECK(written == nlohmann::json::array({"Apple", "Plum"}));

    std::uint32_t bits = 0;
    REQUIRE(ReadBitmask(nlohmann::json{{"mask", written}}, "Test", "mask", kFruitMask, bits));
    CHECK(bits == 0b101u);
}

TEST_CASE("No bits set is an empty list, not an absent field")
{
    CHECK(BitmaskToJson(0u, kFruitMask) == nlohmann::json::array());

    std::uint32_t bits = kAllFruit;
    REQUIRE(Read(R"({"mask": []})", bits));
    CHECK(bits == 0u);
}

TEST_CASE("A set bit nobody named is written as its number, so a save loses nothing")
{
    const nlohmann::json written = BitmaskToJson(0b1001u, kFruitMask);
    CHECK(written == nlohmann::json::array({"Apple", 3}));

    std::uint32_t bits = 0;
    REQUIRE(ReadBitmask(nlohmann::json{{"mask", written}}, "Test", "mask", kFruitMask, bits));
    CHECK(bits == 0b1001u);
}

TEST_CASE("A value the program named through EnumLabels is written and read by that name")
{
    const BitmaskNames labelled{kFruit, "Test::LabelledFruit", kAllFruit};
    RegisterEnumLabel("Test::LabelledFruit", 4, "Quince");

    const nlohmann::json written = BitmaskToJson(0b10010u, labelled);
    CHECK(written == nlohmann::json::array({"Pear", "Quince"}));

    std::uint32_t bits = 0;
    REQUIRE(ReadBitmask(nlohmann::json{{"mask", written}}, "Test", "mask", labelled, bits));
    CHECK(bits == 0b10010u);
}

TEST_CASE("A mask the reader cannot make sense of is refused and leaves the field alone")
{
    std::uint32_t bits = 0b10u;
    CHECK_FALSE(Read(R"({"mask": ["Apple", "Banana"]})", bits));
    CHECK_FALSE(Read(R"({"mask": [32]})", bits));
    CHECK_FALSE(Read(R"({"mask": 5})", bits));
    CHECK_FALSE(Read(R"({"mask": "all"})", bits));
    CHECK(bits == 0b10u);
}

TEST_CASE("An absent mask leaves the field alone")
{
    std::uint32_t bits = 0b10u;
    REQUIRE(Read(R"({})", bits));
    CHECK(bits == 0b10u);
}
