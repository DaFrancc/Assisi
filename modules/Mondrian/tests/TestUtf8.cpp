/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Utf8.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <string_view>
#include <vector>

using namespace Assisi::Mondrian;

namespace
{

/// Every codepoint in @p text, decoded from the start.
std::vector<uint32_t> DecodeAll(std::string_view text)
{
    std::vector<uint32_t> codepoints;
    uint32_t offset = 0;
    while (offset < text.size())
    {
        codepoints.push_back(DecodeUtf8(text, offset));
    }
    return codepoints;
}

} // namespace

TEST_CASE("Utf8: each sequence length decodes to its codepoint and moves past its bytes")
{
    struct Case
    {
        std::string_view text;
        uint32_t codepoint;
    };
    const Case cases[] = {
        {"A", 0x41},                    // one byte
        {"\xC3\xA9", 0xE9},             // e acute, two bytes
        {"\xE2\x82\xAC", 0x20AC},       // euro sign, three bytes
        {"\xF0\x9F\x98\x80", 0x1F600},  // an emoji, four bytes
        {"\xF4\x8F\xBF\xBF", 0x10FFFF}, // the last codepoint
    };
    for (const Case &c : cases)
    {
        CAPTURE(c.codepoint);
        uint32_t offset = 0;
        CHECK(DecodeUtf8(c.text, offset) == c.codepoint);
        CHECK(offset == c.text.size());
    }
}

TEST_CASE("Utf8: bytes that are not UTF-8 read as one replacement character a byte")
{
    const std::string_view invalid[] = {
        "\xFF",             // never a lead byte
        "\x80",             // a continuation with no lead
        "\xC3",             // cut short at the end
        "\xC3\x41",         // a lead followed by no continuation
        "\xC0\x80",         // overlong two-byte form of U+0000
        "\xE0\x80\x80",     // overlong three-byte form
        "\xED\xA0\x80",     // a surrogate
        "\xF4\x90\x80\x80", // past U+10FFFF
        "\xF8\x88\x80\x80", // a five-byte lead
    };
    for (const std::string_view text : invalid)
    {
        CAPTURE(text.size());
        uint32_t offset = 0;
        CHECK(DecodeUtf8(text, offset) == kReplacementCharacter);
        CHECK(offset == 1);
    }
}

TEST_CASE("Utf8: a sequence cut short by the view's end is not read past it")
{
    constexpr std::string_view kEuro = "\xE2\x82\xAC";
    const std::vector<uint32_t> decoded = DecodeAll(kEuro.substr(0, 2));
    CHECK(decoded == std::vector<uint32_t>{kReplacementCharacter, kReplacementCharacter});
}

TEST_CASE("Utf8: a mixed string decodes codepoint by codepoint, recovering after an error")
{
    CHECK(DecodeAll("a\xC3\xA9\xFFz") == std::vector<uint32_t>{0x61, 0xE9, kReplacementCharacter, 0x7A});
}
