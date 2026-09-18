/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Core/AssetId.hpp>

#include <Assisi/Core/ContentHash.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

namespace Assisi::Core
{
namespace
{
constexpr char kHexDigits[] = "0123456789abcdef";

constexpr std::size_t kBitsPerByte = 8;

/// A derived id is two 64-bit hashes laid end to end, most significant byte
/// first, so each half fills this many of the sixteen bytes.
constexpr std::size_t kBytesPerHalf = sizeof(std::uint64_t);

/// Where RFC 4122 keeps an id's version nibble, and the value a derived id puts
/// there. The standard defines no version 0xD, so a minted v4 never matches.
constexpr std::size_t kVersionByte = 6;
constexpr std::uint8_t kVersionHashedBits = 0x0F; ///< The low nibble keeps its hashed value.
constexpr std::uint8_t kDerivedVersion    = 0xD0;

/// Where RFC 4122 keeps an id's variant bits, and the value a derived id puts
/// there. 0b111 is the variant the standard reserves for future use.
constexpr std::size_t kVariantByte = 8;
constexpr std::uint8_t kVariantHashedBits = 0x1F; ///< The low five bits keep their hashed values.
constexpr std::uint8_t kDerivedVariant    = 0xE0;

/// Arbitrary public constants that make the two halves differ. Both halves come
/// from one 64-bit path hash, so a derived id carries 64 bits of it however it
/// is spread across sixteen bytes; nothing here is secret or adds strength.
constexpr std::uint64_t kHighSalt = 0x9E3779B97F4A7C15ULL;
constexpr std::uint64_t kLowSalt  = 0xC2B2AE3D27D4EB4FULL;

/// @brief Hex value of one ASCII character, or -1 if it is not a hex digit.
constexpr int32_t HexValue(char character) noexcept
{
    if (character >= '0' && character <= '9')
    {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f')
    {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F')
    {
        return character - 'A' + 10;
    }
    return -1;
}
} // namespace

bool AssetId::IsNil() const noexcept
{
    return std::ranges::all_of(bytes, [](std::uint8_t byte) { return byte == 0; });
}

bool AssetId::IsReserved() const noexcept
{
    // Reserved built-in range = first 15 bytes zero (`…0000`–`…00FF`). A real
    // UUIDv4 always has a non-zero version nibble (byte 6) and variant bits
    // (byte 8), so it can never satisfy this.
    return std::all_of(bytes.begin(), bytes.begin() + 15, [](std::uint8_t byte) { return byte == 0; });
}

std::string AssetId::ToString() const
{
    // 32 hex digits + 4 dashes.
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        if (i == 4 || i == 6 || i == 8 || i == 10)
        {
            out.push_back('-');
        }
        const std::uint8_t byte = bytes[i];
        out.push_back(kHexDigits[byte >> 4]);
        out.push_back(kHexDigits[byte & 0x0F]);
    }
    return out;
}

std::optional<AssetId> AssetId::Parse(std::string_view text) noexcept
{
    // Collect hex digits, ignoring dashes. Dash placement is intentionally not
    // validated — the invariant we enforce is "exactly 32 hex digits".
    AssetId result{};
    std::size_t nibbleCount = 0;
    for (const char character : text)
    {
        if (character == '-')
        {
            continue;
        }
        const int32_t value = HexValue(character);
        if (value < 0 || nibbleCount >= 32)
        {
            return std::nullopt;
        }
        std::uint8_t &target = result.bytes[nibbleCount / 2];
        if (nibbleCount % 2 == 0)
        {
            target = static_cast<std::uint8_t>(value << 4);
        }
        else
        {
            target = static_cast<std::uint8_t>(target | value);
        }
        ++nibbleCount;
    }
    if (nibbleCount != 32)
    {
        return std::nullopt;
    }
    return result;
}

AssetId DerivedAssetId(std::string_view vpath) noexcept
{
    // Bound to a plain name first: a qualified one on the right of a `*` reads
    // to the formatter as a pointer declaration, and it rewrites the space to
    // match.
    constexpr std::uint64_t prime = kFnvPrime;

    const std::uint64_t base = ContentHash64(std::as_bytes(std::span{vpath}));
    const std::uint64_t high = base ^ kHighSalt;
    const std::uint64_t low  = (base * prime) ^ kLowSalt;

    AssetId id{};
    for (std::size_t i = 0; i < kBytesPerHalf; ++i)
    {
        const std::size_t shift = kBitsPerByte * (kBytesPerHalf - 1 - i);
        id.bytes[i]                 = static_cast<std::uint8_t>(high >> shift);
        id.bytes[kBytesPerHalf + i] = static_cast<std::uint8_t>(low >> shift);
    }

    // Stamped over the hash, so a derived id can never collide with a minted v4
    // however the hash falls.
    id.bytes[kVersionByte] = static_cast<std::uint8_t>((id.bytes[kVersionByte] & kVersionHashedBits) | kDerivedVersion);
    id.bytes[kVariantByte] = static_cast<std::uint8_t>((id.bytes[kVariantByte] & kVariantHashedBits) | kDerivedVariant);

    // The reserved built-in range is the first fifteen bytes zero. The version
    // byte is non-zero by the line above, so an id from here is never in it.
    return id;
}

std::span<const BuiltinAssetEntry> BuiltinAssets() noexcept
{
    // The reserved ids that stand in for the renderer's `prim://` primitives.
    // Nil is deliberately absent — it is the "no asset" sentinel, not a
    // resolvable primitive. Path spellings mirror Render/AssetCache.cpp.
    static constexpr std::array<BuiltinAssetEntry, 12> kEntries{{
        {BuiltinAssetId::Cube, "prim://cube"},
        {BuiltinAssetId::White, "prim://white"},
        {BuiltinAssetId::WhiteLinear, "prim://white-linear"},
        {BuiltinAssetId::FlatNormal, "prim://flat-normal"},
        {BuiltinAssetId::SphereLow, "prim://sphere-low"},
        {BuiltinAssetId::Sphere, "prim://sphere"},
        {BuiltinAssetId::SphereHigh, "prim://sphere-high"},
        {BuiltinAssetId::IcosphereLow, "prim://icosphere-low"},
        {BuiltinAssetId::Icosphere, "prim://icosphere"},
        {BuiltinAssetId::IcosphereHigh, "prim://icosphere-high"},
        {BuiltinAssetId::Cylinder, "prim://cylinder"},
        {BuiltinAssetId::CylinderHigh, "prim://cylinder-high"},
    }};
    return kEntries;
}

} // namespace Assisi::Core
