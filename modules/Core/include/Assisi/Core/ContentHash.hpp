/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ContentHash.hpp
/// @brief A small non-cryptographic content hash for editor source-change
///        detection (the `.aast` `sourceHash`, D5).
///
/// FNV-1a (64-bit): fast, dependency-free, and stable across runs and machines
/// for identical bytes — enough to answer "did this file change since we last
/// processed it?". It is **not** a security primitive and is not stable across a
/// change to this definition; both are fine for its one job (stale detection),
/// where a false "changed" only triggers a harmless re-reconcile.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Assisi::Core
{

inline constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL; ///< FNV-1a 64-bit offset basis.
inline constexpr std::uint64_t kFnvPrime       = 0x00000100000001b3ULL; ///< FNV-1a 64-bit prime.

/// @brief FNV-1a 64-bit hash of @p bytes.
[[nodiscard]] inline std::uint64_t ContentHash64(std::span<const std::byte> bytes) noexcept
{
    std::uint64_t hash = kFnvOffsetBasis;
    for (const std::byte byte : bytes)
    {
        hash ^= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(byte));
        hash *= kFnvPrime;
    }
    return hash;
}

/// @brief ContentHash64 of a text file, with CRLF folded to LF first; nullopt if
///        it could not be read.
///
/// For any hash two machines must agree on. Read in binary so the platform
/// translates nothing behind our back, then normalise explicitly: text reaches
/// disk through two different line-ending translations — git checks `* text=auto`
/// out as CRLF on Windows and LF everywhere else, and a text-mode write on
/// Windows expands every newline on the way out. Either produces a byte-different
/// but semantically identical file, and hashing raw bytes turned that into a
/// refused join between a Windows and a Linux machine sitting on the same level
/// (`562aa5d`).
///
/// Folding CR before LF is enough, because that is the only transformation either
/// path applies. It does mean a literal "\r\n" inside a JSON string value would
/// collide with "\n" — the strings in the files this hashes are asset paths and
/// entity names, which have neither.
///
/// **Every content hash compared across machines must come from here.** Two
/// spellings of this function is not a style problem: it is a pair of peers that
/// refuse each other over an identical file, which is what happened when the
/// editor learned to normalise and the dedicated server did not.
[[nodiscard]] std::optional<std::uint64_t> HashTextFileNormalized(const std::filesystem::path &path);

/// @brief Fixed-width 16-char lowercase hex of @p value — the on-disk form (a
///        64-bit hash overflows JSON's double, so it is stored as a string).
[[nodiscard]] inline std::string ToHex64(std::uint64_t value)
{
    static constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out(16, '0');
    for (std::size_t i = 16; i-- > 0;)
    {
        out[i] = kDigits[value & 0xFU];
        value >>= 4;
    }
    return out;
}

/// @brief Parse a 16-char hex string (as ToHex64 writes); nullopt if malformed.
[[nodiscard]] inline std::optional<std::uint64_t> FromHex64(std::string_view text)
{
    if (text.size() != 16)
    {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char hexChar : text)
    {
        std::uint64_t digit = 0;
        if (hexChar >= '0' && hexChar <= '9')
        {
            digit = static_cast<std::uint64_t>(hexChar - '0');
        }
        else if (hexChar >= 'a' && hexChar <= 'f')
        {
            digit = static_cast<std::uint64_t>(hexChar - 'a') + 10;
        }
        else if (hexChar >= 'A' && hexChar <= 'F')
        {
            digit = static_cast<std::uint64_t>(hexChar - 'A') + 10;
        }
        else
        {
            return std::nullopt;
        }
        value = (value << 4) | digit;
    }
    return value;
}

} // namespace Assisi::Core
