/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestSessionAccess.cpp
/// @brief Session codes, how a host compares secrets, and how it slows a guesser.

#include <doctest/doctest.h>

#include <Assisi/NetSync/AttemptLimiter.hpp>
#include <Assisi/NetSync/SessionSecret.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <unordered_set>

using namespace Assisi;
using namespace Assisi::NetSync;

namespace
{

/// Enough draws that every one of 31 symbols turns up with near certainty.
constexpr std::int32_t kCodeDraws = 10000;

/// A clock a test moves by hand.
struct FakeClock
{
    double now = 1000.0;
};

AttemptLimiter MakeLimiter(FakeClock &clock)
{
    return AttemptLimiter([&clock] { return clock.now; });
}

} // namespace

TEST_CASE("a session code is six symbols from the alphabet, and uses all of it")
{
    std::set<char> seen;
    for (std::int32_t i = 0; i < kCodeDraws; ++i)
    {
        const std::string code = GenerateSessionCode();
        REQUIRE(code.size() == kSessionCodeLength);
        for (const char symbol : code)
        {
            REQUIRE(kSessionCodeAlphabet.find(symbol) != std::string_view::npos);
            seen.insert(symbol);
        }
    }
    CHECK(seen.size() == kSessionCodeAlphabet.size());
    for (const char lookAlike : std::string_view("0O1IL"))
    {
        CHECK(seen.count(lookAlike) == 0);
    }
}

TEST_CASE("two session codes in a row are different")
{
    CHECK(GenerateSessionCode() != GenerateSessionCode());
}

TEST_CASE("a typed code is read forgivingly")
{
    CHECK(NormalizeSessionCode("k7m q4x") == "K7MQ4X");
    CHECK(NormalizeSessionCode(" k7m-q4x\xE2\x80\x8B") == "K7MQ4X");
    CHECK(NormalizeSessionCode("\xEF\xBB\xBFK7M\xC2\xA0Q4X\n") == "K7MQ4X");
    // A wrong symbol stays wrong: forgiving is not guessing.
    CHECK(NormalizeSessionCode("K7MQ4?") == "K7MQ4?");
}

TEST_CASE("secrets compare by every byte and by length")
{
    CHECK(SecretsEqual("hunter2", "hunter2"));
    CHECK_FALSE(SecretsEqual("hunter2", "hunter3"));
    CHECK_FALSE(SecretsEqual("hunter2", "hunter"));
    CHECK_FALSE(SecretsEqual("hunter", "hunter2"));
    CHECK_FALSE(SecretsEqual("hunter2", ""));
    CHECK(SecretsEqual("", ""));
}

TEST_CASE("a code is normalized before comparing, a password is not")
{
    CHECK(SecretAdmits(AccessRule::Code, "K7MQ4X", "k7m q4x"));
    CHECK_FALSE(SecretAdmits(AccessRule::Code, "K7MQ4X", "K7MQ4Y"));
    CHECK(SecretAdmits(AccessRule::Password, "Hunter 2", "Hunter 2"));
    CHECK_FALSE(SecretAdmits(AccessRule::Password, "Hunter2", "hunter2"));
    CHECK_FALSE(SecretAdmits(AccessRule::Password, "hunter2", ""));
    CHECK(SecretAdmits(AccessRule::Open, "", "anything at all"));
}

TEST_CASE("session ids are valid and differ")
{
    std::unordered_set<Core::SessionId> ids;
    for (std::int32_t i = 0; i < 100; ++i)
    {
        const Core::SessionId id = GenerateSessionId();
        CHECK(id.IsValid());
        ids.insert(id);
    }
    CHECK(ids.size() == 100);
}

TEST_CASE("an address waits after five wrong secrets, even with the right one")
{
    FakeClock clock;
    AttemptLimiter limiter = MakeLimiter(clock);
    for (std::uint32_t i = 0; i + 1 < kAttemptsBeforeCooldown; ++i)
    {
        limiter.RecordFailure("192.168.1.5");
        clock.now += 1.0;
    }
    CHECK_FALSE(limiter.IsWaiting("192.168.1.5"));
    limiter.RecordFailure("192.168.1.5");
    CHECK(limiter.IsWaiting("192.168.1.5"));
    CHECK_FALSE(limiter.IsWaiting("192.168.1.6"));

    clock.now += kFirstCooldownSeconds + 0.1;
    CHECK_FALSE(limiter.IsWaiting("192.168.1.5"));
}

TEST_CASE("the wait doubles with each further wrong secret, up to its limit")
{
    FakeClock clock;
    AttemptLimiter limiter = MakeLimiter(clock);
    for (std::uint32_t i = 0; i < kAttemptsBeforeCooldown; ++i)
    {
        limiter.RecordFailure("10.0.0.2");
    }
    double wait = kFirstCooldownSeconds;
    for (std::int32_t round = 0; round < 10; ++round)
    {
        clock.now += wait - 0.1;
        CHECK(limiter.IsWaiting("10.0.0.2"));
        clock.now += 0.2;
        CHECK_FALSE(limiter.IsWaiting("10.0.0.2"));
        limiter.RecordFailure("10.0.0.2");
        wait = std::min(wait * 2.0, kMaxCooldownSeconds);
    }
    CHECK(wait == kMaxCooldownSeconds);
}

TEST_CASE("wrong secrets spread over more than a minute never start a wait")
{
    FakeClock clock;
    AttemptLimiter limiter = MakeLimiter(clock);
    for (std::int32_t i = 0; i < 20; ++i)
    {
        limiter.RecordFailure("10.0.0.3");
        CHECK_FALSE(limiter.IsWaiting("10.0.0.3"));
        clock.now += kAttemptWindowSeconds / (kAttemptsBeforeCooldown - 1) + 1.0;
    }
}

TEST_CASE("the right secret starts an address over")
{
    FakeClock clock;
    AttemptLimiter limiter = MakeLimiter(clock);
    for (std::uint32_t i = 0; i + 1 < kAttemptsBeforeCooldown; ++i)
    {
        limiter.RecordFailure("10.0.0.4");
    }
    limiter.RecordSuccess("10.0.0.4");
    limiter.RecordFailure("10.0.0.4");
    CHECK_FALSE(limiter.IsWaiting("10.0.0.4"));
}
