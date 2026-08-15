/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBitStream.cpp
/// @brief BitWriter/BitReader: primitive round trips, odd bit widths, varint
/// boundaries, cross-byte writes, the on-wire bit order, and — the part that
/// matters most — the reader's behaviour on truncated and corrupted buffers.

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <Assisi/Core/BitStream.hpp>

using Assisi::Core::BitReader;
using Assisi::Core::BitWriter;

namespace
{

/// Deterministic xorshift64*, so a fuzz failure reproduces exactly. std::mt19937
/// would do as well; what matters is that std::random_device never appears in a
/// test — an irreproducible failure in a codec fuzz harness is barely a failure
/// report at all.
class Rng
{
public:
    explicit Rng(std::uint64_t seed) : _state(seed ? seed : 0x9E3779B97F4A7C15ULL) {}

    std::uint64_t Next()
    {
        _state ^= _state >> 12;
        _state ^= _state << 25;
        _state ^= _state >> 27;
        return _state * 0x2545F4914F6CDD1DULL;
    }

    std::uint32_t Below(std::uint32_t bound) { return static_cast<std::uint32_t>(Next() % bound); }

private:
    std::uint64_t _state;
};

std::vector<std::byte> ToBytes(const BitWriter &writer)
{
    return std::vector<std::byte>(writer.Data().begin(), writer.Data().end());
}

} // namespace

TEST_CASE("BitStream: every primitive round-trips")
{
    BitWriter writer;
    writer.WriteBool(true);
    writer.WriteBool(false);
    writer.WriteUInt32(0xDEADBEEFu);
    writer.WriteInt32(-1234567);
    writer.WriteUInt64(0x0123456789ABCDEFULL);
    writer.WriteInt64(-9007199254740993LL);
    writer.WriteFloat(3.14159f);
    writer.WriteDouble(-2.718281828459045);
    writer.WriteString("hello wire");

    const std::array<std::byte, 4> blob{std::byte{0x01}, std::byte{0xFE}, std::byte{0x80}, std::byte{0x7F}};
    writer.WriteBytes(blob);

    BitReader reader(writer.Data());
    CHECK(reader.ReadBool() == true);
    CHECK(reader.ReadBool() == false);
    CHECK(reader.ReadUInt32() == 0xDEADBEEFu);
    CHECK(reader.ReadInt32() == -1234567);
    CHECK(reader.ReadUInt64() == 0x0123456789ABCDEFULL);
    CHECK(reader.ReadInt64() == -9007199254740993LL);
    CHECK(reader.ReadFloat() == doctest::Approx(3.14159f));
    CHECK(reader.ReadDouble() == doctest::Approx(-2.718281828459045));
    CHECK(reader.ReadString() == "hello wire");

    std::array<std::byte, 4> blobOut{};
    reader.ReadBytes(blobOut);
    CHECK(blobOut == blob);

    CHECK(reader.Ok());
    // Everything written has been read back; what is left is at most the final
    // byte's zero padding, since the stream is bit- not byte-granular.
    CHECK(reader.BitsRemaining() < 8);
}

TEST_CASE("BitStream: bit order is LSB-first within each byte")
{
    // Pinning the wire convention: the first bit written is bit 0 of byte 0, and
    // a byte-aligned 32-bit write lands little-endian. A reader on the other end
    // of a socket depends on this being nailed down, not merely consistent with
    // whatever the implementation happens to do.
    BitWriter writer;
    writer.WriteBits(1u, 1); // bit 0
    writer.WriteBits(0u, 6);
    writer.WriteBits(1u, 1); // bit 7
    REQUIRE(writer.BytesWritten() == 1);
    CHECK(writer.Data()[0] == std::byte{0x81});

    BitWriter aligned;
    aligned.WriteUInt32(0x11223344u);
    REQUIRE(aligned.BytesWritten() == 4);
    CHECK(aligned.Data()[0] == std::byte{0x44});
    CHECK(aligned.Data()[1] == std::byte{0x33});
    CHECK(aligned.Data()[2] == std::byte{0x22});
    CHECK(aligned.Data()[3] == std::byte{0x11});
}

TEST_CASE("BitStream: odd bit widths round-trip and cross byte boundaries")
{
    // Widths chosen to leave the cursor at a different bit-in-byte after each
    // write, so most of these values straddle a byte boundary — the case a naive
    // implementation gets wrong and a byte-aligned test would never reach.
    const std::array<std::uint32_t, 12> widths{1, 2, 3, 5, 7, 9, 11, 13, 17, 19, 31, 32};

    BitWriter writer;
    std::vector<std::uint32_t> values;
    Rng rng(0xA55A);
    for (const std::uint32_t width : widths)
    {
        const std::uint32_t mask  = width >= 32 ? 0xFFFFFFFFu : ((1u << width) - 1u);
        const std::uint32_t value = static_cast<std::uint32_t>(rng.Next()) & mask;
        values.push_back(value);
        writer.WriteBits(value, width);
    }

    std::size_t expectedBits = 0;
    for (const std::uint32_t width : widths)
        expectedBits += width;
    CHECK(writer.BitsWritten() == expectedBits);

    BitReader reader(writer.Data());
    for (std::size_t i = 0; i < widths.size(); ++i)
        CHECK(reader.ReadBits(widths[i]) == values[i]);
    CHECK(reader.Ok());
}

TEST_CASE("BitStream: 64-bit widths round-trip, including the full width")
{
    BitWriter writer;
    writer.WriteBits64(0x0123456789ABCDEFULL, 64);
    writer.WriteBits64(0x1FFFFFFFFFFFFULL, 49);
    writer.WriteBits64(1ULL, 33);
    writer.WriteBits64(0u, 0); // no-op, must not shift the stream

    BitReader reader(writer.Data());
    CHECK(reader.ReadBits64(64) == 0x0123456789ABCDEFULL);
    CHECK(reader.ReadBits64(49) == 0x1FFFFFFFFFFFFULL);
    CHECK(reader.ReadBits64(33) == 1ULL);
    CHECK(reader.ReadBits64(0) == 0);
    CHECK(reader.Ok());
}

TEST_CASE("BitStream: values above the requested width are truncated, not smeared")
{
    // The documented contract: WriteBits ignores bits above bitCount. If it
    // instead let them bleed into the next value, every field after a
    // sloppy-but-legal call would silently decode wrong.
    BitWriter writer;
    writer.WriteBits(0xFFFFFFFFu, 4);
    writer.WriteBits(0xAu, 4);

    REQUIRE(writer.BytesWritten() == 1);
    CHECK(writer.Data()[0] == std::byte{0xAF});
}

TEST_CASE("BitStream: varints round-trip at their boundary values")
{
    const std::array<std::uint64_t, 12> values{0,
                                               1,
                                               126,
                                               127,   // last 1-group value
                                               128,   // first 2-group value
                                               16383, // last 2-group
                                               16384, // first 3-group
                                               0x0FFFFFFFULL,
                                               0x10000000ULL,
                                               std::numeric_limits<std::uint32_t>::max(),
                                               0x8000000000000000ULL,
                                               std::numeric_limits<std::uint64_t>::max()};

    BitWriter writer;
    for (const std::uint64_t value : values)
        writer.WriteVarUInt64(value);

    BitReader reader(writer.Data());
    for (const std::uint64_t value : values)
        CHECK(reader.ReadVarUInt64() == value);
    CHECK(reader.Ok());

    SUBCASE("a small varint costs exactly one byte")
    {
        BitWriter small;
        small.WriteVarUInt32(127);
        CHECK(small.BitsWritten() == 8);

        BitWriter medium;
        medium.WriteVarUInt32(128);
        CHECK(medium.BitsWritten() == 16);
    }

    SUBCASE("varints survive an unaligned start")
    {
        BitWriter unaligned;
        unaligned.WriteBits(1u, 3);
        unaligned.WriteVarUInt64(300);
        unaligned.WriteBits(5u, 3);

        BitReader reader2(unaligned.Data());
        CHECK(reader2.ReadBits(3) == 1u);
        CHECK(reader2.ReadVarUInt64() == 300);
        CHECK(reader2.ReadBits(3) == 5u);
        CHECK(reader2.Ok());
    }
}

TEST_CASE("BitStream: ReadVarUInt32 rejects a value that does not fit 32 bits")
{
    BitWriter writer;
    writer.WriteVarUInt64(0x1'0000'0000ULL); // one past UINT32_MAX

    BitReader reader(writer.Data());
    CHECK(reader.ReadVarUInt32() == 0);
    CHECK(reader.Failed()); // truncating silently would hand back a different number
}

TEST_CASE("BitStream: a varint of all-continuation bytes is rejected, not looped on")
{
    // 11 bytes with the continuation bit set: a corrupted stream must not keep
    // the decoder consuming.
    std::vector<std::byte> corrupt(11, std::byte{0xFF});
    BitReader reader(corrupt);
    CHECK(reader.ReadVarUInt64() == 0);
    CHECK(reader.Failed());
}

TEST_CASE("BitStream: strings round-trip, including empty and unaligned")
{
    BitWriter writer;
    writer.WriteBits(1u, 5); // knock the cursor off the byte boundary
    writer.WriteString("");
    writer.WriteString("a");
    writer.WriteString(std::string(200, 'x')); // needs a 2-group length varint

    BitReader reader(writer.Data());
    CHECK(reader.ReadBits(5) == 1u);
    CHECK(reader.ReadString().empty());
    CHECK(reader.ReadString() == "a");
    CHECK(reader.ReadString() == std::string(200, 'x'));
    CHECK(reader.Ok());
}

TEST_CASE("BitStream: ReadStringInto fails rather than truncating into a short buffer")
{
    BitWriter writer;
    writer.WriteString("0123456789");

    BitReader reader(writer.Data());
    char buffer[4] = {};
    CHECK(reader.ReadStringInto(buffer, sizeof(buffer)) == 0);
    CHECK(reader.Failed()); // a string that does not fit means the types disagree
}

TEST_CASE("BitStream: Align pads to the byte boundary on both ends")
{
    BitWriter writer;
    writer.WriteBits(0x5u, 3);
    writer.Align();
    CHECK(writer.BitsWritten() == 8);
    writer.WriteUInt32(0xCAFEBABEu);
    writer.Align(); // already aligned: a no-op
    CHECK(writer.BitsWritten() == 40);

    BitReader reader(writer.Data());
    CHECK(reader.ReadBits(3) == 0x5u);
    reader.Align();
    CHECK(reader.BitsRead() == 8);
    CHECK(reader.ReadUInt32() == 0xCAFEBABEu);
    reader.Align();
    CHECK(reader.Ok());
}

TEST_CASE("BitStream: quantized floats round-trip within their step size")
{
    // Not used by the v1 field encoders — this pins the primitive the deferred
    // quantizers will build on, so "bit-capable" is a tested claim.
    constexpr float kMin  = -100.f;
    constexpr float kMax  = 100.f;
    constexpr std::uint32_t kBits = 12;
    const float step  = (kMax - kMin) / static_cast<float>((1u << kBits) - 1u);

    const std::array<float, 6> values{-100.f, -37.5f, 0.f, 0.001f, 99.9f, 100.f};

    BitWriter writer;
    for (const float value : values)
        writer.WriteFloatQuantized(value, kMin, kMax, kBits);
    CHECK(writer.BitsWritten() == values.size() * kBits);

    BitReader reader(writer.Data());
    for (const float value : values)
    {
        // Absolute, not relative, tolerance: quantization error is a fraction of
        // the *range*, so a relative epsilon would be vacuous near zero and
        // over-strict at the endpoints.
        const float decoded = reader.ReadFloatQuantized(kMin, kMax, kBits);
        CAPTURE(value);
        CHECK(std::abs(decoded - value) <= step);
    }

    SUBCASE("out-of-range values clamp to the endpoints")
    {
        BitWriter clamped;
        clamped.WriteFloatQuantized(-1000.f, kMin, kMax, kBits);
        clamped.WriteFloatQuantized(1000.f, kMin, kMax, kBits);

        BitReader clampReader(clamped.Data());
        CHECK(clampReader.ReadFloatQuantized(kMin, kMax, kBits) == doctest::Approx(kMin));
        CHECK(clampReader.ReadFloatQuantized(kMin, kMax, kBits) == doctest::Approx(kMax));
    }
}

TEST_CASE("BitStream: a 32-bit quantized float round-trips at the top of its range" *
          doctest::should_fail())
{
    // ENG-117, open. At bits == 32 the level count is 0xFFFFFFFF, whose float
    // conversion rounds *up* to 4294967296.0f. `scaled + 0.5f` at the top of the
    // range therefore lands one past UINT32_MAX, and BitStream.cpp:138's
    // static_cast<std::uint32_t> of it is undefined. On x86-64 the out-of-range
    // cvttss2si yields 0x80000000, so the encoded value is not merely imprecise —
    // the maximum encodes as the midpoint, and every other bit width is fine.
    //
    // 12 bits is checked alongside as the control: same code, same endpoints, and
    // it round-trips, which is what makes 32 the outlier rather than the tolerance.
    //
    // should_fail until the conversion is fixed; the fix removes this decorator.
    constexpr float kMin = -100.f;
    constexpr float kMax = 100.f;

    BitWriter control;
    control.WriteFloatQuantized(kMax, kMin, kMax, 12);
    BitReader controlReader(control.Data());
    REQUIRE(controlReader.ReadFloatQuantized(kMin, kMax, 12) == doctest::Approx(kMax));

    BitWriter writer;
    writer.WriteFloatQuantized(kMax, kMin, kMax, 32);
    writer.WriteFloatQuantized(kMin, kMin, kMax, 32);

    BitReader reader(writer.Data());
    // The step at 32 bits is ~5e-8 of the range, so both endpoints are exact to
    // float precision under any correct encoding.
    CHECK(reader.ReadFloatQuantized(kMin, kMax, 32) == doctest::Approx(kMax));
    CHECK(reader.ReadFloatQuantized(kMin, kMax, 32) == doctest::Approx(kMin));
}

TEST_CASE("BitStream: Clear resets the stream but keeps the buffer")
{
    BitWriter writer;
    writer.WriteUInt64(~0ULL);
    writer.Clear();
    CHECK(writer.BitsWritten() == 0);
    CHECK(writer.BytesWritten() == 0);

    writer.WriteBits(1u, 1);
    REQUIRE(writer.BytesWritten() == 1);
    CHECK(writer.Data()[0] == std::byte{0x01}); // no residue from the cleared write
}

// ── Overrun and corruption ────────────────────────────────────────────────────
// The reader is the only part of this codec that touches untrusted bytes, so
// these are the tests that matter. Every one of them also runs under ASan.

TEST_CASE("BitStream: reading past the end fails stickily and returns zero")
{
    BitWriter writer;
    writer.WriteUInt32(0xFFFFFFFFu);

    BitReader reader(writer.Data());
    CHECK(reader.ReadUInt32() == 0xFFFFFFFFu);
    REQUIRE(reader.Ok());

    CHECK(reader.ReadBits(1) == 0); // one bit past the end
    CHECK(reader.Failed());
    CHECK(reader.BitsRemaining() == 0);

    // Sticky: every subsequent read, of any type, is an inert zero.
    CHECK(reader.ReadUInt64() == 0);
    CHECK(reader.ReadFloat() == 0.f);
    CHECK(reader.ReadVarUInt64() == 0);
    CHECK(reader.ReadString().empty());
    CHECK(reader.Failed());
}

TEST_CASE("BitStream: a partial read at the boundary consumes nothing")
{
    BitWriter writer;
    writer.WriteBits(0xFu, 4); // one byte, 4 bits used

    BitReader reader(writer.Data());
    CHECK(reader.ReadBits(4) == 0xFu);
    CHECK(reader.BitsRemaining() == 4); // the padding bits are readable...
    CHECK(reader.ReadBits(5) == 0);     // ...but 5 are not
    CHECK(reader.Failed());
}

TEST_CASE("BitStream: an empty buffer fails every read without touching memory")
{
    BitReader reader({});
    CHECK(reader.Ok()); // nothing read yet
    CHECK(reader.BitsRemaining() == 0);
    CHECK(reader.ReadBool() == false);
    CHECK(reader.Failed());

    BitReader other({});
    CHECK(other.ReadString().empty());
    CHECK(other.Failed());
}

TEST_CASE("BitStream: truncation at every length is a clean failure, never a crash")
{
    BitWriter writer;
    writer.WriteVarUInt64(12345);
    writer.WriteString("truncate me");
    writer.WriteFloat(1.5f);
    writer.WriteBits(0x2Au, 6);
    writer.WriteUInt64(0xFEEDFACECAFEBEEFULL);

    const std::vector<std::byte> full = ToBytes(writer);

    for (std::size_t length = 0; length < full.size(); ++length)
    {
        BitReader reader(std::span{full.data(), length});

        // Decode the whole sequence optimistically, exactly as a packet handler
        // would; nothing here may read outside the truncated span (ASan proves
        // that half), and the sequence must end in Failed rather than in a
        // plausible-looking value.
        (void)reader.ReadVarUInt64();
        (void)reader.ReadString();
        (void)reader.ReadFloat();
        (void)reader.ReadBits(6);
        (void)reader.ReadUInt64();

        CAPTURE(length);
        CHECK(reader.Failed());
    }

    // The untruncated buffer is the control: the same sequence must succeed.
    BitReader reader(full);
    CHECK(reader.ReadVarUInt64() == 12345);
    CHECK(reader.ReadString() == "truncate me");
    CHECK(reader.ReadFloat() == doctest::Approx(1.5f));
    CHECK(reader.ReadBits(6) == 0x2Au);
    CHECK(reader.ReadUInt64() == 0xFEEDFACECAFEBEEFULL);
    CHECK(reader.Ok());
}

TEST_CASE("BitStream: bit-flipped buffers never read out of bounds")
{
    BitWriter writer;
    writer.WriteVarUInt64(9001);
    writer.WriteString("corrupt me");
    writer.WriteUInt32(7);
    writer.WriteBits(0x3u, 3);

    const std::vector<std::byte> original = ToBytes(writer);
    Rng rng(0xC0FFEE);

    for (std::int32_t iteration = 0; iteration < 4000; ++iteration)
    {
        std::vector<std::byte> corrupt = original;

        // One to three flipped bits: enough to reach every decision the decoder
        // makes (length prefixes, continuation bits) without degenerating into
        // "random bytes", which exercises far less of the parser.
        const std::uint32_t flips = 1u + rng.Below(3);
        for (std::uint32_t f = 0; f < flips; ++f)
        {
            const std::size_t bitIndex = rng.Below(static_cast<std::uint32_t>(corrupt.size() * 8u));
            const std::uint32_t bit      = 1u << (bitIndex % 8u);
            corrupt[bitIndex / 8u] ^= static_cast<std::byte>(static_cast<std::uint8_t>(bit));
        }

        BitReader reader(corrupt);
        (void)reader.ReadVarUInt64();
        (void)reader.ReadString();
        (void)reader.ReadUInt32();
        (void)reader.ReadBits(3);

        // No assertion on the outcome: a flipped bit inside a value decodes to a
        // different but perfectly valid value, and that is fine. What is being
        // asserted is that we got here at all — no crash, no hang, and (under
        // ASan) no read outside the buffer.
        CHECK((reader.Ok() || reader.Failed()));
        CHECK(reader.BitsRead() <= corrupt.size() * 8u);
    }
}

TEST_CASE("BitStream: random buffers decoded as arbitrary structure stay in bounds")
{
    Rng rng(0x5EED);

    for (std::int32_t iteration = 0; iteration < 2000; ++iteration)
    {
        const std::size_t size = rng.Below(64);
        std::vector<std::byte> noise(size);
        for (std::byte &byte : noise)
            byte = static_cast<std::byte>(static_cast<std::uint8_t>(rng.Next()));

        BitReader reader(noise);
        // A decode schedule the sender never wrote — which is exactly what an
        // attacker sends.
        (void)reader.ReadVarUInt32();
        (void)reader.ReadString();
        (void)reader.ReadBits64(37);
        (void)reader.ReadDouble();
        char buffer[16] = {};
        (void)reader.ReadStringInto(buffer, sizeof(buffer));
        std::array<std::byte, 8> blob{};
        reader.ReadBytes(blob);

        CHECK(reader.BitsRead() <= noise.size() * 8u);
    }
}
