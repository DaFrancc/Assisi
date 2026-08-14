/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file BitStream.hpp
/// @brief Bit-granular little-endian writer/reader — the network codec's
///        lowest layer.
///
/// Bit-level from day one, deliberately: the two biggest wins a state-replication
/// codec has — a 1-bit "unchanged" flag per field and smallest-three quaternions
/// (2 + 9 + 9 + 9 = 29 bits) — are arithmetically impossible on a byte-aligned
/// writer, and retrofitting bit granularity under a shipped wire format is a
/// format *and* protocol-hash break at exactly the moment bandwidth starts
/// hurting. The v1 field encoders above this layer stay whole-value (see
/// Reflect/BinaryCodec.hpp); the primitive is bit-capable so quantizers drop in
/// later with no format break.
///
/// **Bit order: LSB-first within each byte.** The first bit written lands in bit
/// 0 of byte 0, the ninth in bit 0 of byte 1. A multi-bit value is written
/// low-bits-first, so a byte-aligned `WriteBits(v, 8)` stores exactly `v`, and a
/// byte-aligned 32-bit write stores little-endian bytes — the same order the
/// engine's target platforms use natively, so a future memcpy fast path is a
/// drop-in. (LSB-first is the conventional choice: Quake, Source, yojimbo and
/// GNS's own serializers all do it.) Reader and writer are exact mirrors; the
/// round-trip tests pin the symmetry.
///
/// **Trust boundary.** BitWriter serializes data this process owns and cannot
/// fail (it grows its own buffer). BitReader eats *untrusted network bytes*, so
/// every read is bounds-checked and a single overrun latches a sticky failure
/// state: from then on reads return zero, `Failed()` is true, and nothing throws,
/// asserts, or touches memory outside the span. A truncated or bit-flipped packet
/// must be a clean rejection, never UB — see TestBitStream.cpp's fuzz cases.

#include <Assisi/Core/StrongId.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Core
{

/// @brief Maximum bytes a single `ReadString` will accept, whatever the encoded
///        length claims.
///
/// A length prefix arriving off the wire is attacker-controlled: without a cap,
/// a 5-byte varint asks for a 4 GB allocation. Every string the codec writes is a
/// fixed-capacity inline string (`ShortString`, `AssetPath` — 127 bytes at the
/// widest), so this bound is generous by an order of magnitude and still refuses
/// the attack.
inline constexpr std::size_t kMaxStringBytes = 4096;

// The varint id encoding below is available to any type that opts in via
// Core::IsStrongId — see StrongId.hpp. Core never names those types: NetId and
// ClientId live in NetSync, InstanceId in ECS, and Core sits below both, so
// naming one would invert the dependency.
//
// The point of routing them through here rather than open-coding
// `WriteVarUInt32(id.value)`: the width comes from the id's own declaration, so
// widening one is an edit to that declaration and nothing else. An open-coded
// call silently keeps the old width, and a too-narrow *read* truncates — which
// does not fail, it just yields a valid id naming something else.

/// @brief Bit-granular writer over a growable byte buffer.
///
/// Cannot fail: the buffer grows on demand and the process owns everything it
/// serializes. Writes past 32 bits go through WriteBits64.
class BitWriter
{
public:
    BitWriter() = default;

    /// @brief Constructs a writer with @p reserveBytes of capacity pre-allocated
    /// — worth passing when the packet's rough size is known, to keep a snapshot
    /// build allocation-free.
    explicit BitWriter(std::size_t reserveBytes) { _bytes.reserve(reserveBytes); }

    /// @brief Appends the low @p bitCount bits of @p value, LSB first.
    /// Bits above @p bitCount are ignored, not asserted on: callers routinely
    /// pass a wider value they have already range-checked. @p bitCount > 32 is a
    /// contract violation (use WriteBits64).
    void WriteBits(std::uint32_t value, std::uint32_t bitCount);

    /// @brief 64-bit form of WriteBits. @p bitCount > 64 is a contract violation.
    void WriteBits64(std::uint64_t value, std::uint32_t bitCount);

    /// @brief One bit. The unit that makes per-field change masks cheap.
    void WriteBool(bool value) { WriteBits(value ? 1u : 0u, 1); }

    void WriteUInt32(std::uint32_t value) { WriteBits(value, 32); }
    void WriteInt32(std::int32_t value);
    void WriteUInt64(std::uint64_t value) { WriteBits64(value, 64); }
    void WriteInt64(std::int64_t value);

    /// @brief IEEE-754 binary32, written as its raw 32-bit pattern (bit_cast).
    /// Signalling NaNs and negative zero survive the round trip unaltered.
    void WriteFloat(float value);

    /// @brief IEEE-754 binary64, written as its raw 64-bit pattern.
    void WriteDouble(double value);

    /// @brief LEB128 varint: 7 payload bits per group, high bit = "more follows".
    /// One byte for values < 128 — which is what makes the per-block ComponentId
    /// prefix nearly free while still admitting a registry of any size.
    void WriteVarUInt32(std::uint32_t value) { WriteVarUInt64(value); }
    void WriteVarUInt64(std::uint64_t value);

    /// @brief A strong id as a varint, at whatever width its `value` is.
    ///
    /// Always the 64-bit writer: the encoding of any value a narrower id can
    /// hold is byte-identical, so the write side is width-independent already
    /// and only `BitReader::ReadVarId` has to track the type. See `StrongId`.
    void WriteVarId(StrongId auto id) { WriteVarUInt64(id.value); }

    /// @brief Raw bytes, 8 bits each in order. Not byte-aligned — the bytes land
    /// wherever the cursor is, so no padding is spent.
    void WriteBytes(std::span<const std::byte> bytes);

    /// @brief Length-prefixed string: `varint byteCount` then the raw bytes.
    /// Content is opaque (no terminator, no encoding validation) — same
    /// convention as TrivialString.
    void WriteString(std::string_view text);

    /// @brief Maps @p value from [@p min, @p max] onto @p bits of resolution.
    ///
    /// Reserved for the quantizers the design defers past v1 (positions,
    /// velocities, smallest-three quaternions): the field encoders in
    /// Reflect/BinaryCodec.hpp stay whole-value, so nothing calls this yet. It
    /// exists now so the primitive is provably bit-capable rather than
    /// aspirationally so. Values outside the range are clamped; @p bits must be
    /// in 1..32 and @p max must exceed @p min.
    void WriteFloatQuantized(float value, float min, float max, std::uint32_t bits);

    /// @brief Advances to the next byte boundary, zero-filling. Only needed by a
    /// future memcpy fast path or an externally byte-aligned sub-block; the codec
    /// never pads.
    void Align();

    /// @brief Total bits appended so far.
    [[nodiscard]] std::size_t BitsWritten() const { return _bitCount; }

    /// @brief Bytes the payload occupies — the tail byte counts even if partial.
    [[nodiscard]] std::size_t BytesWritten() const { return _bytes.size(); }

    /// @brief The payload. Any unwritten bits of the final byte are zero, so the
    /// span is safe to hash, compare, or hand to a transport as-is.
    [[nodiscard]] std::span<const std::byte> Data() const { return _bytes; }

    /// @brief Resets to empty, keeping the allocated capacity for reuse across
    /// packets.
    void Clear();

private:
    std::vector<std::byte> _bytes;
    std::size_t _bitCount = 0;            ///< Bits written; _bytes.size() == ceil(_bitCount / 8).
};

/// @brief Bit-granular reader over a borrowed byte span. Every read is
///        bounds-checked; the first overrun latches a sticky failure.
///
/// The failure state is sticky rather than per-call so a caller can decode a
/// whole block optimistically and test `Ok()` once at the end — the alternative
/// (checking every read) is the pattern that reliably grows a hole. Reads after a
/// failure are no-ops returning zero, so a partially-decoded component is filled
/// with zeroes, never with adjacent heap bytes.
class BitReader
{
public:
    /// @brief Reads from @p data, which must outlive the reader (it is borrowed,
    /// not copied).
    explicit BitReader(std::span<const std::byte> data) : _data(data) {}

    /// @brief Reads @p bitCount bits, LSB first. @p bitCount > 32 is a contract
    /// violation (use ReadBits64). Returns 0 and fails on overrun.
    std::uint32_t ReadBits(std::uint32_t bitCount);

    /// @brief 64-bit form of ReadBits. @p bitCount > 64 is a contract violation.
    std::uint64_t ReadBits64(std::uint32_t bitCount);

    bool          ReadBool() { return ReadBits(1) != 0; }
    std::uint32_t ReadUInt32() { return ReadBits(32); }
    std::int32_t  ReadInt32();
    std::uint64_t ReadUInt64() { return ReadBits64(64); }
    std::int64_t  ReadInt64();
    float         ReadFloat();
    double        ReadDouble();

    /// @brief Reads a varint. Fails on overrun and on a malformed encoding — a
    /// group count past the maximum for the width, which is how a corrupted
    /// stream would otherwise spin the decode loop.
    std::uint32_t ReadVarUInt32();
    std::uint64_t ReadVarUInt64();

    /// @brief Reads a strong id, refusing a value its `value` type cannot hold.
    ///
    /// This is the side that carries the width, and the reason `StrongId` exists:
    /// the range check comes from the id's own declaration, so widening the type
    /// widens what the wire accepts with no edit here and none at the call sites.
    ///
    /// Refused rather than truncated, on the same principle as ReadStringInto
    /// above — a truncated id is not a detectably broken id, it is a valid one
    /// naming a different object, and it propagates silently from there.
    template <StrongId T> T ReadVarId()
    {
        const std::uint64_t raw = ReadVarUInt64();
        if (_failed)
            return T{};
        if (raw > std::numeric_limits<decltype(T::value)>::max())
        {
            Fail();
            return T{};
        }
        return T{static_cast<decltype(T::value)>(raw)};
    }

    /// @brief Reads exactly `out.size()` bytes into @p out. On overrun, fails and
    /// zero-fills @p out.
    void ReadBytes(std::span<std::byte> out);

    /// @brief Reads a length-prefixed string into @p buffer (no terminator
    /// written).
    /// @return the number of bytes written, or 0 on failure.
    ///
    /// Fails — rather than truncating — when the encoded length exceeds
    /// @p capacity or kMaxStringBytes: a string that does not fit the destination
    /// means the sender and receiver disagree about the type, which is a protocol
    /// error, and silently truncating it would let the disagreement propagate.
    std::size_t ReadStringInto(char *buffer, std::size_t capacity);

    /// @brief Allocating convenience form of ReadStringInto; returns an empty
    /// string on failure. @p maxBytes is clamped to kMaxStringBytes.
    std::string ReadString(std::size_t maxBytes = kMaxStringBytes);

    /// @brief Inverse of BitWriter::WriteFloatQuantized. Deferred alongside it —
    /// nothing in v1 calls it; the round-trip test does.
    float ReadFloatQuantized(float min, float max, std::uint32_t bits);

    /// @brief Skips to the next byte boundary. Fails if that would pass the end.
    void Align();

    /// @brief Latches the failure state from outside.
    ///
    /// For a *semantic* rejection a layer above the bit level detects — an
    /// element count no legitimate sender would emit, an id not in the registry.
    /// Such a stream is as unusable as a truncated one, and routing both through
    /// one flag means a caller has exactly one thing to test (`Ok()`) rather than
    /// a per-layer error protocol.
    void Invalidate() { Fail(); }

    /// @brief True while no read has overrun or hit a malformed encoding.
    [[nodiscard]] bool Ok() const { return !_failed; }

    /// @brief True once a read has failed. Sticky — it never clears.
    [[nodiscard]] bool Failed() const { return _failed; }

    [[nodiscard]] std::size_t BitsRead() const { return _bitPos; }
    [[nodiscard]] std::size_t BitsRemaining() const { return TotalBits() - _bitPos; }

private:
    [[nodiscard]] std::size_t TotalBits() const { return _data.size() * 8u; }

    /// Latches failure and parks the cursor at the end, so a subsequent
    /// BitsRemaining() reports zero and nothing can back into a valid-looking
    /// state.
    void Fail()
    {
        _failed = true;
        _bitPos = TotalBits();
    }

    /// @return false (having failed) when @p bitCount bits are not available.
    bool Ensure(std::size_t bitCount)
    {
        if (_failed)
            return false;
        if (bitCount > BitsRemaining())
        {
            Fail();
            return false;
        }
        return true;
    }

    std::span<const std::byte> _data;
    std::size_t _bitPos = 0;
    bool _failed = false;
};

} // namespace Assisi::Core
