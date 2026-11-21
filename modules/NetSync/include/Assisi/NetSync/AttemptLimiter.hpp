/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AttemptLimiter.hpp
/// @brief Slows down one address that keeps sending the wrong secret.
///
/// A host counts wrong secrets per remote address. After a few in a short time
/// the address is made to wait, and each further wrong secret doubles the wait,
/// so guessing a 6-symbol code takes longer than anyone will try for. While an
/// address waits, its secrets are not even compared: a right one is refused too,
/// or a guesser could keep going and simply notice which answer came back
/// different.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Assisi::NetSync
{

/// @brief Wrong secrets an address may send within kAttemptWindowSeconds before it waits.
inline constexpr std::uint32_t kAttemptsBeforeCooldown = 5;
/// @brief How far back the wrong secrets that start a wait are counted (s).
inline constexpr double kAttemptWindowSeconds = 60.0;
/// @brief The first wait (s). Each later wrong secret doubles it.
inline constexpr double kFirstCooldownSeconds = 5.0;
/// @brief The longest wait (s). After this long with neither a wrong secret nor
/// a wait, the address starts over.
inline constexpr double kMaxCooldownSeconds = 300.0;

class AttemptLimiter
{
public:
    /// @brief Seconds on any steady clock.
    using Clock = std::function<double ()>;

    /// @param clock What "now" is; a test passes its own. Defaults to the
    ///   process's steady clock.
    explicit AttemptLimiter(Clock clock = {});

    /// @brief Whether @p address must wait before a secret it sends is compared.
    [[nodiscard]] bool IsWaiting(std::string_view address) const;

    /// @brief @p address sent a wrong secret.
    void RecordFailure(std::string_view address);

    /// @brief @p address sent the right secret, and starts over.
    void RecordSuccess(std::string_view address);

private:
    struct Record
    {
        double windowStart = 0.0;   ///< When the failures counted so far began.
        double lastFailure = 0.0;
        double waitUntil = 0.0;
        double waitSeconds = 0.0;   ///< 0 until the first wait; doubled after.
        std::uint32_t failures = 0; ///< Since windowStart.
    };

    [[nodiscard]] double Now() const;

    /// How long @p record's address has gone without failing or waiting.
    [[nodiscard]] static double QuietFor(const Record &record, double now);

    /// Forgets every address that has been quiet long enough to start over, so
    /// a stream of addresses cannot grow the table without bound.
    void ForgetQuiet(double now);

    std::unordered_map<std::string, Record> _records;
    Clock _clock;
};

} // namespace Assisi::NetSync
