/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SessionSecret.hpp
/// @brief Session codes, passwords, and how a host compares what a joiner sent.
///
/// A secret never travels in a discovery reply. It goes only in the joiner's
/// ClientAuth, inside the connection, and the host compares it here.

#include <Assisi/Core/SessionId.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace Assisi::NetSync
{

/// @brief Who a host lets in.
enum class AccessRule : std::uint8_t
{
    Open,     ///< Anyone who finds the session.
    Password, ///< Whoever sends the password the host chose, exactly.
    Code,     ///< Whoever sends the session's code, typed however they like.
    Count_
};

/// @brief Who a host lets in, and how many.
struct SessionAccess
{
    /// The password, or the code in its normalized form. Unused by an open session.
    std::string secret{};
    /// The most connections admitted at once; the default admits any number,
    /// and 0 admits nobody. A listen server's own player is not a connection, so
    /// its caller counts it out.
    std::uint32_t maxClients = std::numeric_limits<std::uint32_t>::max();
    AccessRule rule = AccessRule::Open;
};

/// @brief The symbols a session code is drawn from: digits and uppercase letters
/// without the look-alikes 0, O, 1, I and L, so a code read aloud or off a
/// screen is typed right. 31 symbols, like Crockford's base32.
inline constexpr std::string_view kSessionCodeAlphabet = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";

/// @brief How many symbols a session code has: 31^6, about 887 million codes,
/// which a guesser slowed by the host's limiter never gets through.
inline constexpr std::size_t kSessionCodeLength = 6;

/// @brief The longest secret a host reads. A password longer than this is
/// refused when hosting rather than cut, so what the host typed is what joiners
/// must type.
inline constexpr std::size_t kMaxSecretBytes = 64;

/// @brief A fresh session code: kSessionCodeLength symbols from
/// kSessionCodeAlphabet, each equally likely, from the system's random source.
[[nodiscard]] std::string GenerateSessionCode();

/// @brief A code as a person typed or pasted it, in the form the host compares:
/// uppercase, with spaces, dashes and invisible characters (zero-width spaces
/// and joiners, the byte-order mark, the non-breaking space) removed. So
/// `" k7m-q4x"` is `"K7MQ4X"`. Nothing else is changed: a symbol outside the
/// alphabet stays and simply does not match.
[[nodiscard]] std::string NormalizeSessionCode(std::string_view input);

/// @brief Whether @p expected and @p presented are the same bytes, taking the
/// same time wherever they first differ, so the time a refusal takes says
/// nothing about how much of a guess was right.
[[nodiscard]] bool SecretsEqual(std::string_view expected, std::string_view presented);

/// @brief Whether @p presented opens a session whose rule is @p rule and whose
/// secret is @p expected: always for an open session, exactly for a password,
/// and after NormalizeSessionCode for a code.
[[nodiscard]] bool SecretAdmits(AccessRule rule, std::string_view expected, std::string_view presented);

/// @brief A fresh session id: 128 bits from the system's random source.
[[nodiscard]] Core::SessionId GenerateSessionId();

} // namespace Assisi::NetSync
