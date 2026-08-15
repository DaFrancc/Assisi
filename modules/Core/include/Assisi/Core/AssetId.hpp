/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AssetId.hpp
/// @brief Stable 128-bit asset identity (a UUIDv4) — the reference token that
///        replaces raw asset paths.
///
/// An `AssetId` is a plain 16-byte value: trivially copyable, heap-free, cheap
/// to hash, safe to store inline in a component or serialize into a level. It is
/// the identity half of the asset-database architecture
/// (docs/asset-database-architecture.md §1): stored references point at an
/// `AssetId`, and the database maps that id to the file currently holding it, so
/// references survive any rename or move.
///
/// Minted ids are random UUIDv4s (see MintAssetId in AssetDatabase.hpp), so two
/// machines or branches never collide when their histories merge. Engine
/// built-ins that are not files (the `prim://` primitives) get **reserved**
/// constant ids from a fixed low range (`…0000`–`…00FF`); a real v4's version
/// and variant nibbles are non-zero, so a minted id can never land in that
/// range — the guarantee is structural, not probabilistic.
///
/// This type ships in every build (component fields reference it). Minting, the
/// database, and the loose-file provider are editor-only (see AssetDatabase.hpp).

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Assisi::Core
{

/// @brief A 128-bit asset identity (UUIDv4 layout). All-zero is the nil id
///        ("no asset"); ids with the first 15 bytes zero are the reserved
///        built-in range.
struct AssetId
{
    /// Raw bytes in big-endian textual order: bytes[0] is the first hex pair of
    /// the canonical string form. Default-constructed = nil (all zero).
    std::array<std::uint8_t, 16> bytes{};

    friend bool operator==(const AssetId &, const AssetId &)  = default;
    friend std::strong_ordering operator<=>(const AssetId &, const AssetId &) = default;

    /// @brief True when every byte is zero — the "no asset" sentinel.
    [[nodiscard]] bool IsNil() const noexcept;

    /// @brief True when this id is in the reserved built-in range (first 15
    ///        bytes zero, i.e. `…0000`–`…00FF`). Nil is reserved index 0.
    [[nodiscard]] bool IsReserved() const noexcept;

    /// @brief Canonical lowercase UUID string, `8-4-4-4-12` (36 chars).
    [[nodiscard]] std::string ToString() const;

    /// @brief Parse a UUID string. Accepts the canonical dashed form and the
    ///        bare 32-hex-digit form, upper- or lower-case; std::nullopt on any
    ///        malformed input. Dash placement is not validated — only that the
    ///        remaining characters are exactly 32 hex digits.
    [[nodiscard]] static std::optional<AssetId> Parse(std::string_view text) noexcept;
};

namespace detail
{
/// @brief Build a reserved built-in id from a low index (last byte = @p index,
///        all others zero). constexpr so the built-in constants below are
///        compile-time values.
[[nodiscard]] constexpr AssetId ReservedAssetId(std::uint8_t index) noexcept
{
    AssetId result{};
    result.bytes[15] = index;
    return result;
}
} // namespace detail

/// @brief Reserved constant ids for engine built-ins that are not files. These
///        map to the primitive factories the resolver already exposes via the
///        `prim://` scheme (see BuiltinAssets()).
namespace BuiltinAssetId
{
inline constexpr AssetId Nil         = detail::ReservedAssetId(0); ///< "no asset".
inline constexpr AssetId Cube        = detail::ReservedAssetId(1); ///< prim://cube.
inline constexpr AssetId White       = detail::ReservedAssetId(2); ///< prim://white.
inline constexpr AssetId WhiteLinear = detail::ReservedAssetId(3); ///< prim://white-linear.
inline constexpr AssetId FlatNormal  = detail::ReservedAssetId(4); ///< prim://flat-normal.
} // namespace BuiltinAssetId

/// @brief One reserved built-in id and the virtual path it stands in for.
struct BuiltinAssetEntry
{
    AssetId id;
    std::string_view virtualPath;
};

/// @brief The table of reserved built-ins, seeded into the database so a
///        reserved id resolves to its primitive exactly as a `prim://` path does
///        today. Nil is not included (it is not a resolvable asset).
[[nodiscard]] std::span<const BuiltinAssetEntry> BuiltinAssets() noexcept;

} // namespace Assisi::Core

template <> struct std::hash<Assisi::Core::AssetId>
{
    std::size_t operator()(const Assisi::Core::AssetId &assetId) const noexcept
    {
        // Two 64-bit reads combined, then run through the splitmix64 finalizer.
        // A minted id is already high-entropy, but the reserved built-in ids are
        // tiny sequential integers (…0001, …0002, …) — the finalizer's avalanche
        // is what keeps those from clustering into adjacent hash buckets.
        constexpr std::uint64_t kGamma      = 0x9e3779b97f4a7c15ULL; // 2^64 / golden ratio.
        constexpr std::uint64_t kMixMul1    = 0xbf58476d1ce4e5b9ULL; // splitmix64 finalizer multipliers,
        constexpr std::uint64_t kMixMul2    = 0x94d049bb133111ebULL; // searched for good bit avalanche.
        constexpr std::uint32_t kMixShift1  = 30;
        constexpr std::uint32_t kMixShift2  = 27;
        constexpr std::uint32_t kMixShift3  = 31;

        auto load = [&](std::size_t offset) noexcept
                    {
                        std::uint64_t acc = 0;
                        for (std::size_t i = 0; i < 8; ++i)
                        {
                            acc = (acc << 8) | assetId.bytes[offset + i];
                        }
                        return acc;
                    };
        std::uint64_t mixed = load(0) ^ (load(8) + kGamma);
        mixed = (mixed ^ (mixed >> kMixShift1)) * kMixMul1;
        mixed = (mixed ^ (mixed >> kMixShift2)) * kMixMul2;
        mixed = mixed ^ (mixed >> kMixShift3);
        return static_cast<std::size_t>(mixed);
    }
};
