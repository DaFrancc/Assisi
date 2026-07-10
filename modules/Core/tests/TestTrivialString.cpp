/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

// doctest forward-declares std::ostream; stringifying a std::string_view in a
// CHECK instantiates operator<<(ostream&, string_view), which needs the complete
// type. Pull it in so string_view comparisons can be reported on failure.
#include <ostream>

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

#include <Assisi/Core/TrivialString.hpp>

using Assisi::Core::TrivialString;

TEST_CASE("TrivialString: round-trips and reports truncation at its capacity")
{
    TrivialString<8> s;
    CHECK(s.Assign("abcd"));
    CHECK(s.View() == "abcd");
    CHECK(s.Size() == 4);

    CHECK(s.Assign("exactly8")); // 8 chars, fits exactly
    CHECK(s.View() == "exactly8");

    CHECK_FALSE(s.Assign("waytoolong")); // truncated to 8
    CHECK(s.Size() == 8);
    CHECK(s.View() == "waytoolo");
}

TEST_CASE("TrivialString: equality is defined over the live view, not the tail")
{
    // Reassigning long->short must compare equal to a freshly-made short value.
    // Equality reads View() (the first Size() bytes), so whatever the previous
    // longer contents left in the tail is irrelevant.
    TrivialString<16> a{std::string_view{"longer-value"}};
    CHECK(a.Assign("short"));
    CHECK(a == TrivialString<16>{std::string_view{"short"}});
    CHECK_FALSE(a == TrivialString<16>{std::string_view{"longer-value"}});
}

TEST_CASE("TrivialString: truncation backs off a split UTF-8 code point")
{
    // 7 ASCII bytes + "é" (0xC3 0xA9, two bytes) = 9 bytes into a cap-8 string.
    // A naive cut to 8 bytes would keep the lead byte 0xC3 alone (invalid UTF-8);
    // the boundary back-off must drop the whole 'é', leaving 7 valid bytes.
    std::string text = "aaaaaaa";
    text.push_back(static_cast<char>(0xC3));
    text.push_back(static_cast<char>(0xA9));

    TrivialString<8> s;
    CHECK_FALSE(s.Assign(text)); // truncated
    CHECK(s.Size() == 7);
    CHECK(s.View() == "aaaaaaa");
}

TEST_CASE("TrivialString: exports a std::string copy")
{
    TrivialString<16> s{std::string_view{"hello"}};
    const std::string out = s.ToString();
    CHECK(out == "hello");
}

TEST_CASE("TrivialString: exports a null-terminated C-string copy into a caller buffer")
{
    TrivialString<16> s{std::string_view{"hello"}};

    char buf[8] = {};
    const std::size_t written = s.ToCStr(buf, sizeof(buf));
    CHECK(written == 5);
    CHECK(std::string_view(buf) == "hello");
    CHECK(buf[5] == '\0');

    // Undersized buffer: writes bufferSize-1 chars + terminator, never overruns.
    char small[4] = {};
    const std::size_t writtenSmall = s.ToCStr(small, sizeof(small));
    CHECK(writtenSmall == 3);
    CHECK(std::string_view(small) == "hel");
    CHECK(small[3] == '\0');

    // Zero-size buffer is a no-op (and must not write).
    CHECK(s.ToCStr(nullptr, 0) == 0);
}

TEST_CASE("TrivialString: capacities beyond a byte work (uint16_t length prefix)")
{
    // The length prefix is a flat uint16_t, so capacities past 255 hold fine.
    TrivialString<300> big;
    const std::string   text(300, 'z');
    CHECK(big.Assign(text));
    CHECK(big.Size() == 300);
    CHECK(big.View() == text);
    CHECK_FALSE(big.Assign(std::string(301, 'z'))); // truncated to 300
    CHECK(big.Size() == 300);
}

TEST_CASE("TrivialString: stays trivially copyable at any capacity")
{
    static_assert(std::is_trivially_copyable_v<TrivialString<8>>);
    static_assert(std::is_trivially_copyable_v<TrivialString<300>>);
}
