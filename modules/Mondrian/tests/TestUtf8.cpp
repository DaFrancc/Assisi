/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Utf8.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
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

TEST_CASE("Utf8: every codepoint encodes back to the bytes it decoded from")
{
    for (const std::string_view text : {"A", "\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x98\x80", "\xF4\x8F\xBF\xBF"})
    {
        CAPTURE(text.size());
        uint32_t offset = 0;
        std::string encoded;
        EncodeUtf8(DecodeUtf8(text, offset), encoded);
        CHECK(encoded == text);
    }
}

TEST_CASE("Utf8: a codepoint UTF-8 cannot carry encodes as the replacement character")
{
    constexpr uint32_t kSurrogate = 0xD800;
    constexpr uint32_t kPastTheEnd = 0x110000;
    for (const uint32_t codepoint : {kSurrogate, kPastTheEnd})
    {
        CAPTURE(codepoint);
        std::string encoded;
        EncodeUtf8(codepoint, encoded);
        uint32_t offset = 0;
        CHECK(DecodeUtf8(encoded, offset) == kReplacementCharacter);
    }
}

TEST_CASE("Utf8: a character is a codepoint and everything that modifies it")
{
    struct Case
    {
        std::string_view text;
        std::string_view what;
    };
    // Each is one character to a reader, whatever it is made of.
    const Case cases[] = {
        {"A", "a letter"},
        {"\xC3\xA9", "e acute, written as one codepoint"},
        {"e\xCC\x81", "e acute, written as a letter and a combining accent"},
        {"\xF0\x9F\x98\x80", "an emoji"},
        {"\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD", "a thumbs up with a skin tone"},
        {"\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7", "a woman and a girl joined into a family"},
        {"\xF0\x9F\x87\xAA\xF0\x9F\x87\xB8", "a flag, which is two regional indicators"},
    };
    for (const Case &c : cases)
    {
        CAPTURE(c.what);
        CHECK(NextCharacter(c.text, 0) == c.text.size());
        CHECK(PreviousCharacter(c.text, static_cast<uint32_t>(c.text.size())) == 0);
        CHECK(CharacterCount(c.text) == 1);
    }
}

TEST_CASE("Utf8: stepping forward and back over a string lands on the same boundaries")
{
    // "a", an emoji, "e" with a combining accent, "b".
    constexpr std::string_view kMixed = "a\xF0\x9F\x98\x80"
                                        "e\xCC\x81"
                                        "b";
    const std::vector<uint32_t> expected{0, 1, 5, 8, 9};

    std::vector<uint32_t> forwards;
    for (uint32_t offset = 0; offset < kMixed.size(); offset = NextCharacter(kMixed, offset))
    {
        forwards.push_back(offset);
    }
    forwards.push_back(static_cast<uint32_t>(kMixed.size()));
    CHECK(forwards == expected);

    std::vector<uint32_t> backwards;
    for (uint32_t offset = static_cast<uint32_t>(kMixed.size()); offset > 0; offset = PreviousCharacter(kMixed, offset))
    {
        backwards.push_back(offset);
    }
    backwards.push_back(0);
    std::ranges::reverse(backwards);
    CHECK(backwards == expected);

    CHECK(CharacterCount(kMixed) == 4);
}

TEST_CASE("Utf8: an index names a character and an offset says how many came before it")
{
    constexpr std::string_view kMixed = "a\xF0\x9F\x98\x80"
                                        "e\xCC\x81"
                                        "b";
    CHECK(CharacterOffset(kMixed, 0) == 0);
    CHECK(CharacterOffset(kMixed, 1) == 1); // past "a"
    CHECK(CharacterOffset(kMixed, 2) == 5); // past the emoji's four bytes
    CHECK(CharacterOffset(kMixed, 3) == 8); // past the letter and its accent
    CHECK(CharacterIndex(kMixed, 5) == 2);
    CHECK(CharacterIndex(kMixed, 8) == 3);

    // An index past the end is the end, so a caret cannot run off the string.
    CHECK(CharacterOffset(kMixed, CharacterCount(kMixed) + 1) == kMixed.size());
    CHECK(CharacterIndex(kMixed, static_cast<uint32_t>(kMixed.size()) + 1) == CharacterCount(kMixed));
}

TEST_CASE("Utf8: stepping starts from the boundary an offset inside a character belongs to")
{
    // Two bytes into the emoji, which begins at 1 and ends at 5.
    constexpr std::string_view kMixed = "a\xF0\x9F\x98\x80"
                                        "b";
    CHECK(NextCharacter(kMixed, 3) == 5);
    CHECK(PreviousCharacter(kMixed, 3) == 1);
}

TEST_CASE("Utf8: bytes that are not UTF-8 step one at a time rather than stalling")
{
    constexpr std::string_view kBroken = "\xFF\xFF";
    CHECK(NextCharacter(kBroken, 0) == 1);
    CHECK(CharacterCount(kBroken) == 2);
    CHECK(NextCharacter(kBroken, 2) == 2);
}
