/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
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

TEST_CASE("HashTextFileNormalized folds CRLF, so peers on different checkouts agree")
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "assisi-contenthash-test";
    std::filesystem::create_directories(dir);

    const auto write = [](const std::filesystem::path &path, std::string_view bytes)
                       {
                           std::ofstream out(path, std::ios::binary);
                           out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                       };

    // The same level, checked out by git on two platforms.
    const std::filesystem::path lf   = dir / "level-lf.alvl";
    const std::filesystem::path crlf = dir / "level-crlf.alvl";
    write(lf, "{\n  \"version\": 2\n}\n");
    write(crlf, "{\r\n  \"version\": 2\r\n}\r\n");

    const std::optional<std::uint64_t> lfHash   = HashTextFileNormalized(lf);
    const std::optional<std::uint64_t> crlfHash = HashTextFileNormalized(crlf);
    REQUIRE(lfHash.has_value());
    REQUIRE(crlfHash.has_value());
    CHECK(*lfHash == *crlfHash);

    // Raw bytes disagree — the refused join the fold exists to prevent.
    CHECK(ContentHash64(Bytes("{\n  \"version\": 2\n}\n")) !=
          ContentHash64(Bytes("{\r\n  \"version\": 2\r\n}\r\n")));

    // Content still matters: normalising line endings is not normalising away edits.
    const std::filesystem::path other = dir / "other.alvl";
    write(other, "{\n  \"version\": 3\n}\n");
    CHECK(HashTextFileNormalized(other) != lfHash);

    // A lone CR is not a line ending either translation produces, so it is kept.
    const std::filesystem::path loneCr = dir / "lone-cr.alvl";
    write(loneCr, "{\r  \"version\": 2\r}\r");
    CHECK(HashTextFileNormalized(loneCr) != lfHash);

    CHECK_FALSE(HashTextFileNormalized(dir / "does-not-exist.alvl").has_value());

    std::filesystem::remove_all(dir);
}

TEST_CASE("HashTextFileNormalized reports a read that failed after the file opened")
{
    // A directory stands in for any read error the open cannot predict: it
    // opens, and then every read off it fails. The missing-file path does not
    // cover this, and the answer must be nullopt rather than a hash of however
    // much was read — a caller comparing content sets treats a hash as an
    // answer about the file, and "nothing was read" is not one.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "assisi-contenthash-unreadable";
    std::filesystem::create_directories(dir);

    CHECK_FALSE(HashTextFileNormalized(dir).has_value());

    std::filesystem::remove_all(dir);
}
