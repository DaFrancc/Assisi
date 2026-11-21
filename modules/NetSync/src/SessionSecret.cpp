/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/SessionSecret.hpp>

#include <algorithm>
#include <array>
#include <random>

namespace Assisi::NetSync
{
namespace
{

/// The invisible characters a pasted code can carry, as UTF-8: zero-width
/// space, non-joiner and joiner, the word joiner, the byte-order mark, and the
/// non-breaking space.
constexpr std::array<std::string_view, 6> kInvisible{"\xE2\x80\x8B", "\xE2\x80\x8C", "\xE2\x80\x8D",
                                                     "\xE2\x81\xA0", "\xEF\xBB\xBF", "\xC2\xA0"};

/// How many bytes of an invisible character start at @p at, or 0.
std::size_t InvisibleAt(std::string_view text, std::size_t at)
{
    for (const std::string_view mark : kInvisible)
    {
        if (text.substr(at, mark.size()) == mark)
        {
            return mark.size();
        }
    }
    return 0;
}

bool IsSeparator(char symbol)
{
    return symbol == ' ' || symbol == '-' || symbol == '\t' || symbol == '\r' || symbol == '\n';
}

std::uint64_t RandomWord(std::random_device &device)
{
    // random_device hands out 32 bits a call on every platform this builds for.
    constexpr std::uint32_t kHalfBits = 32;
    const std::uint64_t high = device();
    return (high << kHalfBits) | device();
}

} // namespace

std::string GenerateSessionCode()
{
    std::random_device device;
    // Rejection sampling: a draw from the top of the range that does not divide
    // evenly by 31 would make the first symbols slightly likelier than the rest.
    const std::uint32_t symbols = static_cast<std::uint32_t>(kSessionCodeAlphabet.size());
    const std::uint32_t limit = std::random_device::max() - (std::random_device::max() % symbols);
    std::string code;
    code.reserve(kSessionCodeLength);
    while (code.size() < kSessionCodeLength)
    {
        const std::uint32_t draw = device();
        if (draw >= limit)
        {
            continue;
        }
        code.push_back(kSessionCodeAlphabet[draw % symbols]);
    }
    return code;
}

std::string NormalizeSessionCode(std::string_view input)
{
    constexpr char kCaseOffset = 'a' - 'A';
    std::string code;
    code.reserve(input.size());
    std::size_t at = 0;
    while (at < input.size())
    {
        if (const std::size_t invisible = InvisibleAt(input, at); invisible > 0)
        {
            at += invisible;
            continue;
        }
        const char symbol = input[at];
        ++at;
        if (IsSeparator(symbol))
        {
            continue;
        }
        code.push_back(symbol >= 'a' && symbol <= 'z' ? static_cast<char>(symbol - kCaseOffset) : symbol);
    }
    return code;
}

bool SecretsEqual(std::string_view expected, std::string_view presented)
{
    // Over the longer of the two, every time, with the length difference folded
    // in: an early return on the first different byte, or on a length mismatch,
    // would let the time a refusal takes count how much of a guess was right.
    const std::size_t length = std::max(expected.size(), presented.size());
    std::uint32_t difference = expected.size() == presented.size() ? 0u : 1u;
    for (std::size_t i = 0; i < length; ++i)
    {
        const std::uint8_t a = i < expected.size() ? static_cast<std::uint8_t>(expected[i]) : 0u;
        const std::uint8_t b = i < presented.size() ? static_cast<std::uint8_t>(presented[i]) : 0u;
        difference |= static_cast<std::uint32_t>(a ^ b);
    }
    return difference == 0;
}

bool SecretAdmits(AccessRule rule, std::string_view expected, std::string_view presented)
{
    switch (rule)
    {
    case AccessRule::Open:
        return true;
    case AccessRule::Password:
        return SecretsEqual(expected, presented);
    case AccessRule::Code:
        return SecretsEqual(expected, NormalizeSessionCode(presented));
    case AccessRule::Count_:
        break;
    }
    return false;
}

Core::SessionId GenerateSessionId()
{
    std::random_device device;
    Core::SessionId id;
    // Redrawn on the one value that means "no session", so a fresh id is always valid.
    while (!id.IsValid())
    {
        id.high = RandomWord(device);
        id.low = RandomWord(device);
    }
    return id;
}

} // namespace Assisi::NetSync
