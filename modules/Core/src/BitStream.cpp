/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file BitStream.cpp
/// @brief BitWriter / BitReader implementation. See BitStream.hpp for the wire
///        conventions (LSB-first bit order, sticky reader failure).

#include <Assisi/Core/BitStream.hpp>

#include <algorithm>
#include <bit>

#include <Assisi/Core/Assert.hpp>

namespace Assisi::Core
{
namespace
{

/// Groups in a 64-bit LEB128 varint: ceil(64/7). A stream claiming more than
/// this is malformed — without the cap a corrupted buffer full of continuation
/// bits would keep the decoder spinning until it happened to run out of data.
constexpr std::uint32_t kMaxVarUInt64Groups = 10;

/// Mask of the low @p bitCount bits, defined for bitCount == 64 (where the
/// obvious `(1 << 64) - 1` is UB).
constexpr std::uint64_t LowBitMask(std::uint32_t bitCount)
{
    return bitCount >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bitCount) - 1u;
}

} // namespace

// ── BitWriter ─────────────────────────────────────────────────────────────────

void BitWriter::WriteBits64(std::uint64_t value, std::uint32_t bitCount)
{
    ASSISI_ASSERT(bitCount <= 64, "BitWriter::WriteBits64 accepts at most 64 bits");
    if (bitCount == 0)
        return;
    bitCount = std::min(bitCount, 64u);

    // Ignore anything above bitCount rather than asserting: callers routinely
    // pass a wider value they have already range-checked (an enum read at its
    // underlying width, a masked field id).
    value &= LowBitMask(bitCount);

    // Straight-line loop over at most nine partial-byte writes. Deliberately not
    // a 64-bit scratch register: this keeps Data() trivially const (no pending
    // flush) and the whole thing auditable, and snapshot assembly is dominated by
    // the pool walks above it, not by these shifts. Revisit only if a profile
    // says so.
    std::uint32_t remaining = bitCount;
    while (remaining > 0)
    {
        const std::uint32_t bitInByte = static_cast<std::uint32_t>(_bitCount % 8u);
        if (bitInByte == 0)
            _bytes.push_back(std::byte{0});

        const std::uint32_t take  = std::min(8u - bitInByte, remaining);
        const std::uint32_t chunk = static_cast<std::uint32_t>(value & LowBitMask(take));

        _bytes.back() |= static_cast<std::byte>(static_cast<std::uint8_t>(chunk << bitInByte));

        value >>= take;
        remaining -= take;
        _bitCount += take;
    }
}

void BitWriter::WriteBits(std::uint32_t value, std::uint32_t bitCount)
{
    ASSISI_ASSERT(bitCount <= 32, "BitWriter::WriteBits accepts at most 32 bits; use WriteBits64");
    WriteBits64(value, std::min(bitCount, 32u));
}

void BitWriter::WriteInt32(std::int32_t value)
{
    // Two's-complement pattern, not zig-zag: these are whole-value fields, and a
    // zig-zag transform only pays off under a varint, which the fixed-width
    // integer fields deliberately do not use.
    WriteBits(std::bit_cast<std::uint32_t>(value), 32);
}

void BitWriter::WriteInt64(std::int64_t value)
{
    WriteBits64(std::bit_cast<std::uint64_t>(value), 64);
}

void BitWriter::WriteFloat(float value)
{
    WriteBits(std::bit_cast<std::uint32_t>(value), 32);
}

void BitWriter::WriteDouble(double value)
{
    WriteBits64(std::bit_cast<std::uint64_t>(value), 64);
}

void BitWriter::WriteVarUInt64(std::uint64_t value)
{
    // LEB128: low 7 bits per group, high bit set while more groups follow. Groups
    // are written as plain 8-bit chunks of the bitstream — they are not aligned to
    // byte boundaries, so a varint after an odd-width field costs no padding.
    do
    {
        const std::uint32_t group = static_cast<std::uint32_t>(value & 0x7Fu);
        value >>= 7;
        WriteBits(group | (value != 0 ? 0x80u : 0u), 8);
    } while (value != 0);
}

void BitWriter::WriteBytes(std::span<const std::byte> bytes)
{
    for (const std::byte byte : bytes)
        WriteBits(std::to_integer<std::uint32_t>(byte), 8);
}

void BitWriter::WriteString(std::string_view text)
{
    WriteVarUInt64(text.size());
    WriteBytes(std::as_bytes(std::span{text.data(), text.size()}));
}

void BitWriter::WriteFloatQuantized(float value, float min, float max, std::uint32_t bits)
{
    ASSISI_ASSERT(bits >= 1 && bits <= 32, "WriteFloatQuantized: bits must be in 1..32");
    ASSISI_ASSERT(max > min, "WriteFloatQuantized: max must exceed min");
    if (bits == 0 || bits > 32 || !(max > min))
        return;

    const std::uint32_t levels = static_cast<std::uint32_t>(LowBitMask(bits)); // 2^bits - 1
    const float         span   = max - min;
    const float         t      = std::clamp((value - min) / span, 0.f, 1.f);

    // Round-to-nearest, so the quantization error is symmetric (±half a step)
    // rather than always biased toward min the way truncation would be.
    const float scaled = t * static_cast<float>(levels);
    WriteBits(static_cast<std::uint32_t>(scaled + 0.5f), bits);
}

void BitWriter::Align()
{
    const std::uint32_t bitInByte = static_cast<std::uint32_t>(_bitCount % 8u);
    if (bitInByte != 0)
        WriteBits(0, 8u - bitInByte);
}

void BitWriter::Clear()
{
    _bytes.clear(); // keeps capacity — the point of reusing a writer across packets
    _bitCount = 0;
}

// ── BitReader ─────────────────────────────────────────────────────────────────

std::uint64_t BitReader::ReadBits64(std::uint32_t bitCount)
{
    ASSISI_ASSERT(bitCount <= 64, "BitReader::ReadBits64 accepts at most 64 bits");
    if (bitCount == 0)
        return 0;
    bitCount = std::min(bitCount, 64u);

    if (!Ensure(bitCount))
        return 0;

    // Mirror of WriteBits64, byte-partial chunk at a time. Ensure() above already
    // guaranteed every byte this touches is in range, so the indexing needs no
    // further checks — the one bounds test covers the whole read.
    std::uint64_t value     = 0;
    std::uint32_t filled    = 0;
    std::uint32_t remaining = bitCount;
    while (remaining > 0)
    {
        const std::size_t   byteIndex = _bitPos / 8u;
        const std::uint32_t bitInByte = static_cast<std::uint32_t>(_bitPos % 8u);
        const std::uint32_t take      = std::min(8u - bitInByte, remaining);

        const std::uint32_t byteValue = std::to_integer<std::uint32_t>(_data[byteIndex]);
        const std::uint64_t chunk     = (byteValue >> bitInByte) & LowBitMask(take);

        value |= chunk << filled;

        filled += take;
        remaining -= take;
        _bitPos += take;
    }
    return value;
}

std::uint32_t BitReader::ReadBits(std::uint32_t bitCount)
{
    ASSISI_ASSERT(bitCount <= 32, "BitReader::ReadBits accepts at most 32 bits; use ReadBits64");
    return static_cast<std::uint32_t>(ReadBits64(std::min(bitCount, 32u)));
}

std::int32_t BitReader::ReadInt32()
{
    return std::bit_cast<std::int32_t>(ReadBits(32));
}

std::int64_t BitReader::ReadInt64()
{
    return std::bit_cast<std::int64_t>(ReadBits64(64));
}

float BitReader::ReadFloat()
{
    return std::bit_cast<float>(ReadBits(32));
}

double BitReader::ReadDouble()
{
    return std::bit_cast<double>(ReadBits64(64));
}

std::uint64_t BitReader::ReadVarUInt64()
{
    std::uint64_t value = 0;
    for (std::uint32_t group = 0; group < kMaxVarUInt64Groups; ++group)
    {
        if (!Ensure(8))
            return 0;
        const std::uint32_t byteValue = ReadBits(8);
        value |= static_cast<std::uint64_t>(byteValue & 0x7Fu) << (group * 7u);
        if ((byteValue & 0x80u) == 0)
            return value;
    }

    // Ten groups without a terminator means the encoding is malformed (or the
    // buffer is corrupt): reject rather than keep consuming.
    Fail();
    return 0;
}

std::uint32_t BitReader::ReadVarUInt32()
{
    // Decoded at full width and then range-checked, rather than open-coding a
    // 5-group loop with a partial-final-group overflow test: same rejection, one
    // obviously-correct path. A value that does not fit 32 bits is malformed —
    // silently truncating it would hand the caller a different number than the
    // sender wrote.
    const std::uint64_t value = ReadVarUInt64();
    if (_failed)
        return 0;
    if (value > UINT32_MAX)
    {
        Fail();
        return 0;
    }
    return static_cast<std::uint32_t>(value);
}

void BitReader::ReadBytes(std::span<std::byte> out)
{
    if (!Ensure(out.size() * 8u))
    {
        std::fill(out.begin(), out.end(), std::byte{0});
        return;
    }
    for (std::byte &byte : out)
        byte = static_cast<std::byte>(static_cast<std::uint8_t>(ReadBits(8)));
}

std::size_t BitReader::ReadStringInto(char *buffer, std::size_t capacity)
{
    const std::uint64_t length = ReadVarUInt64();
    if (_failed)
        return 0;

    // Length is attacker-controlled, so it is checked against the destination and
    // the global cap *before* anything is read, and against the bits actually
    // present so a short buffer cannot be padded with zeroes into a valid-looking
    // string.
    if (length > capacity || length > kMaxStringBytes || length * 8u > BitsRemaining())
    {
        Fail();
        return 0;
    }

    for (std::size_t i = 0; i < length; ++i)
        buffer[i] = static_cast<char>(static_cast<std::uint8_t>(ReadBits(8)));

    return static_cast<std::size_t>(length);
}

std::string BitReader::ReadString(std::size_t maxBytes)
{
    maxBytes = std::min(maxBytes, kMaxStringBytes);

    const std::uint64_t length = ReadVarUInt64();
    if (_failed)
        return {};
    if (length > maxBytes || length * 8u > BitsRemaining())
    {
        Fail();
        return {};
    }

    // Sized only after both bounds checks pass, so a bogus length prefix can
    // never drive an allocation.
    std::string out(static_cast<std::size_t>(length), '\0');
    for (char &character : out)
        character = static_cast<char>(static_cast<std::uint8_t>(ReadBits(8)));
    return out;
}

float BitReader::ReadFloatQuantized(float min, float max, std::uint32_t bits)
{
    ASSISI_ASSERT(bits >= 1 && bits <= 32, "ReadFloatQuantized: bits must be in 1..32");
    ASSISI_ASSERT(max > min, "ReadFloatQuantized: max must exceed min");
    if (bits == 0 || bits > 32 || !(max > min))
        return min;

    const std::uint32_t levels = static_cast<std::uint32_t>(LowBitMask(bits));
    const std::uint32_t raw    = ReadBits(bits);
    return min + (static_cast<float>(raw) / static_cast<float>(levels)) * (max - min);
}

void BitReader::Align()
{
    const std::uint32_t bitInByte = static_cast<std::uint32_t>(_bitPos % 8u);
    if (bitInByte != 0)
        (void)ReadBits(8u - bitInByte); // bounds-checked like any other read
}

} // namespace Assisi::Core
