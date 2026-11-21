/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/AttemptLimiter.hpp>

#include <algorithm>
#include <chrono>
#include <utility>

namespace Assisi::NetSync
{
namespace
{

/// The table size above which quiet addresses are swept out on the next failure.
constexpr std::size_t kSweepAbove = 256;

double SteadySeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

AttemptLimiter::AttemptLimiter(Clock clock) : _clock(clock ? std::move(clock) : Clock{&SteadySeconds}) {}

double AttemptLimiter::Now() const { return _clock(); }

bool AttemptLimiter::IsWaiting(std::string_view address) const
{
    const auto it = _records.find(std::string(address));
    return it != _records.end() && Now() < it->second.waitUntil;
}

void AttemptLimiter::RecordFailure(std::string_view address)
{
    const double now = Now();
    if (_records.size() > kSweepAbove)
    {
        ForgetQuiet(now);
    }

    Record &record = _records[std::string(address)];
    if (QuietFor(record, now) > kMaxCooldownSeconds)
    {
        record = Record{.windowStart = now};
    }
    else if (record.waitSeconds == 0.0 && now - record.windowStart > kAttemptWindowSeconds)
    {
        record.windowStart = now;
        record.failures = 0;
    }
    record.lastFailure = now;
    ++record.failures;

    if (record.waitSeconds > 0.0)
    {
        record.waitSeconds = std::min(record.waitSeconds * 2.0, kMaxCooldownSeconds);
        record.waitUntil = now + record.waitSeconds;
    }
    else if (record.failures >= kAttemptsBeforeCooldown)
    {
        record.waitSeconds = kFirstCooldownSeconds;
        record.waitUntil = now + record.waitSeconds;
    }
}

void AttemptLimiter::RecordSuccess(std::string_view address) { _records.erase(std::string(address)); }

void AttemptLimiter::ForgetQuiet(double now)
{
    std::erase_if(_records, [now](const std::pair<const std::string, Record> &entry)
                  { return QuietFor(entry.second, now) > kMaxCooldownSeconds; });
}

double AttemptLimiter::QuietFor(const Record &record, double now)
{
    // From the end of its wait, not its last failure: a wait of the longest
    // length is itself that long, and the address has not been quiet during it.
    return now - std::max(record.lastFailure, record.waitUntil);
}

} // namespace Assisi::NetSync
