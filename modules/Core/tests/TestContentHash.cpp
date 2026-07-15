/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <Assisi/Core/ContentHash.hpp>

using namespace Assisi::Core;

namespace
{
std::span<const std::byte> Bytes(std::string_view text)
{
    return {reinterpret_cast<const std::byte *>(text.data()), text.size()};
}
} // namespace

TEST_CASE("ContentHash64 is deterministic and input-sensitive")
{
    CHECK(ContentHash64(Bytes("hello world")) == ContentHash64(Bytes("hello world")));
    CHECK(ContentHash64(Bytes("hello world")) != ContentHash64(Bytes("hello worlD")));
    // Empty input hashes to the FNV offset basis.
    CHECK(ContentHash64({}) == kFnvOffsetBasis);
}

TEST_CASE("ToHex64/FromHex64 round-trip")
{
    for (const std::uint64_t value : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0xdeadbeefcafef00dULL},
                                      ~std::uint64_t{0}, ContentHash64(Bytes("some asset bytes"))})
    {
        const std::string hex = ToHex64(value);
        CHECK(hex.size() == 16);
        CHECK(FromHex64(hex) == value);
    }

    // Fixed-width, lowercase, zero-padded.
    CHECK(ToHex64(0) == "0000000000000000");
    CHECK(ToHex64(0xABCUL) == "0000000000000abc");
}

TEST_CASE("FromHex64 rejects malformed input")
{
    CHECK_FALSE(FromHex64("").has_value());              // wrong length
    CHECK_FALSE(FromHex64("abc").has_value());           // wrong length
    CHECK_FALSE(FromHex64("00000000000000000").has_value()); // 17 chars
    CHECK_FALSE(FromHex64("000000000000000g").has_value());  // non-hex digit
}
