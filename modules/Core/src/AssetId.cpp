/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Core/AssetId.hpp>

#include <algorithm>
#include <array>

namespace Assisi::Core
{
namespace
{
constexpr char kHexDigits[] = "0123456789abcdef";

/// @brief Hex value of one ASCII character, or -1 if it is not a hex digit.
constexpr int HexValue(char character) noexcept
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
    AssetId     result{};
    std::size_t nibbleCount = 0;
    for (const char character : text)
    {
        if (character == '-')
        {
            continue;
        }
        const int value = HexValue(character);
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

std::span<const BuiltinAssetEntry> BuiltinAssets() noexcept
{
    // The reserved ids that stand in for the renderer's `prim://` primitives.
    // Nil is deliberately absent — it is the "no asset" sentinel, not a
    // resolvable primitive. Path spellings mirror Render/AssetCache.cpp.
    static constexpr std::array<BuiltinAssetEntry, 4> kEntries{{
        {BuiltinAssetId::Cube, "prim://cube"},
        {BuiltinAssetId::White, "prim://white"},
        {BuiltinAssetId::WhiteLinear, "prim://white-linear"},
        {BuiltinAssetId::FlatNormal, "prim://flat-normal"},
    }};
    return kEntries;
}

} // namespace Assisi::Core
