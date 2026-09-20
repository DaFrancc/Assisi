/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Utf8.hpp>

#include <Assisi/Core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace Assisi::Mondrian
{
namespace
{

/// A continuation byte is 10xxxxxx and carries six bits.
constexpr uint8_t kContinuationMask = 0xC0;
constexpr uint8_t kContinuationTag = 0x80;
constexpr uint8_t kContinuationValue = 0x3F;
constexpr uint32_t kContinuationBits = 6;

/// The first codepoints UTF-8 does not allow: surrogates and past the end.
constexpr uint32_t kSurrogateFirst = 0xD800;
constexpr uint32_t kSurrogateLast = 0xDFFF;
constexpr uint32_t kLastCodepoint = 0x10FFFF;

/// One multi-byte form: the lead bytes that start it, the bits a lead byte
/// carries, and the smallest codepoint it may encode, below which it is overlong.
struct Form
{
    uint32_t minimum = 0;
    uint8_t leadMask = 0; ///< the lead byte's tag bits
    uint8_t leadTag = 0;
    uint8_t length = 0; ///< bytes in the sequence, lead included
};

constexpr std::array kForms{
    Form{.minimum = 0x80, .leadMask = 0xE0, .leadTag = 0xC0, .length = 2},
    Form{.minimum = 0x800, .leadMask = 0xF0, .leadTag = 0xE0, .length = 3},
    Form{.minimum = 0x10000, .leadMask = 0xF8, .leadTag = 0xF0, .length = 4},
};

/// Below this a byte is a whole codepoint.
constexpr uint8_t kFirstMultiByte = 0x80;

/// One run of codepoints, from first to last inclusive.
struct Range
{
    uint32_t first = 0;
    uint32_t last = 0;
};

/// The accents and other marks a writer puts on a letter, in the blocks Unicode
/// spreads them across. Each sits on the letter before it rather than beside it.
constexpr Range kCombiningDiacriticals{.first = 0x0300, .last = 0x036F};
constexpr Range kCombiningDiacriticalsExtended{.first = 0x1AB0, .last = 0x1AFF};
constexpr Range kCombiningDiacriticalsSupplement{.first = 0x1DC0, .last = 0x1DFF};
constexpr Range kCombiningHalfMarks{.first = 0xFE20, .last = 0xFE2F};
constexpr Range kCombiningForSymbols{.first = 0x20D0, .last = 0x20F0};
constexpr Range kCombiningCyrillic{.first = 0x0483, .last = 0x0489};
constexpr Range kHebrewPoints{.first = 0x0591, .last = 0x05BD};
constexpr Range kArabicMarks{.first = 0x0610, .last = 0x061A};
constexpr Range kArabicVowels{.first = 0x064B, .last = 0x065F};
constexpr Range kThaiVowelsAndTones{.first = 0x0E31, .last = 0x0E3A};

/// The codepoints that choose between a glyph's forms, and the ones that set an
/// emoji's skin tone. Both modify the character before them and draw nothing of
/// their own.
constexpr Range kVariationSelectors{.first = 0xFE00, .last = 0xFE0F};
constexpr Range kVariationSelectorsSupplement{.first = 0xE0100, .last = 0xE01EF};
constexpr Range kSkinToneModifiers{.first = 0x1F3FB, .last = 0x1F3FF};

/// Everything that continues the character before it rather than starting one
/// of its own. Scanned in full, so the order is the reader's convenience.
constexpr std::array kExtendRanges{
    kCombiningDiacriticals,
    kCombiningDiacriticalsExtended,
    kCombiningDiacriticalsSupplement,
    kCombiningHalfMarks,
    kCombiningForSymbols,
    kCombiningCyrillic,
    kHebrewPoints,
    kArabicMarks,
    kArabicVowels,
    kThaiVowelsAndTones,
    kVariationSelectors,
    kVariationSelectorsSupplement,
    kSkinToneModifiers,
};

/// Binds what follows it to what precedes it: the joiner that makes several
/// emoji draw as one picture.
constexpr uint32_t kZeroWidthJoiner = 0x200D;

/// A pair of these is one flag.
constexpr Range kRegionalIndicators{.first = 0x1F1E6, .last = 0x1F1FF};

bool In(const Range &range, uint32_t codepoint)
{
    return codepoint >= range.first && codepoint <= range.last;
}

bool Extends(uint32_t codepoint)
{
    return std::ranges::any_of(kExtendRanges, [codepoint](const Range &range) { return In(range, codepoint); });
}

/// The codepoint at @p offset without moving it, or nothing at the end.
std::optional<uint32_t> PeekAt(std::string_view text, uint32_t offset)
{
    if (offset >= text.size())
    {
        return std::nullopt;
    }
    uint32_t after = offset;
    return DecodeUtf8(text, after);
}

/// Moves @p offset past the codepoint it is on, which the caller has already
/// looked at and does not need again.
void SkipCodepoint(std::string_view text, uint32_t &offset)
{
    static_cast<void>(DecodeUtf8(text, offset));
}

/// Where the character starting at @p offset ends. @p offset must be where a
/// character begins; everything below reaches one through BoundaryAt first.
uint32_t StepCharacter(std::string_view text, uint32_t offset)
{
    uint32_t after = offset;
    const uint32_t base = DecodeUtf8(text, after);

    // A flag is a pair and nothing more: a run of four indicators is two flags,
    // not one, so this takes two and leaves the rest to the next step.
    if (In(kRegionalIndicators, base))
    {
        const std::optional<uint32_t> second = PeekAt(text, after);
        if (second && In(kRegionalIndicators, *second))
        {
            SkipCodepoint(text, after);
        }
        return after;
    }

    while (const std::optional<uint32_t> next = PeekAt(text, after))
    {
        if (Extends(*next))
        {
            SkipCodepoint(text, after);
            continue;
        }
        if (*next != kZeroWidthJoiner)
        {
            break;
        }
        // The joiner takes what follows it into this character. A joiner with
        // nothing after it joins nothing and stands alone.
        uint32_t joined = after;
        SkipCodepoint(text, joined);
        if (joined >= text.size())
        {
            break;
        }
        SkipCodepoint(text, joined);
        after = joined;
    }
    return after;
}

/// Where the character containing byte @p offset begins.
uint32_t BoundaryAt(std::string_view text, uint32_t offset)
{
    uint32_t start = 0;
    while (start < text.size())
    {
        const uint32_t next = StepCharacter(text, start);
        if (next > offset)
        {
            return start;
        }
        start = next;
    }
    return start;
}

uint8_t ByteAt(std::string_view text, std::size_t index)
{
    return static_cast<uint8_t>(text[index]);
}

} // namespace

uint32_t DecodeUtf8(std::string_view text, uint32_t &offset)
{
    ASSISI_ASSERT(offset < text.size(), "DecodeUtf8 read past the end of its text");

    const uint8_t lead = ByteAt(text, offset);
    if (lead < kFirstMultiByte)
    {
        ++offset;
        return lead;
    }

    for (const Form &form : kForms)
    {
        if ((lead & form.leadMask) != form.leadTag)
        {
            continue;
        }
        if (text.size() - offset < form.length)
        {
            break;
        }
        uint32_t codepoint = lead & static_cast<uint8_t>(~form.leadMask);
        bool continued = true;
        for (uint32_t i = 1; i < form.length; ++i)
        {
            const uint8_t next = ByteAt(text, offset + i);
            continued = continued && (next & kContinuationMask) == kContinuationTag;
            codepoint = (codepoint << kContinuationBits) | (next & kContinuationValue);
        }
        const bool surrogate = codepoint >= kSurrogateFirst && codepoint <= kSurrogateLast;
        if (!continued || codepoint < form.minimum || surrogate || codepoint > kLastCodepoint)
        {
            break;
        }
        offset += form.length;
        return codepoint;
    }

    ++offset;
    return kReplacementCharacter;
}

void EncodeUtf8(uint32_t codepoint, std::string &text)
{
    const bool surrogate = codepoint >= kSurrogateFirst && codepoint <= kSurrogateLast;
    const uint32_t written = surrogate || codepoint > kLastCodepoint ? kReplacementCharacter : codepoint;
    if (written < kFirstMultiByte)
    {
        text.push_back(static_cast<char>(written));
        return;
    }

    // The longest form whose range the codepoint has reached, which is the
    // shortest that can carry it.
    const Form *form = &kForms.front();
    for (const Form &candidate : kForms)
    {
        if (written >= candidate.minimum)
        {
            form = &candidate;
        }
    }

    const uint32_t leadBits = kContinuationBits * (form->length - 1u);
    text.push_back(static_cast<char>(form->leadTag | static_cast<uint8_t>(written >> leadBits)));
    for (uint32_t remaining = form->length - 1u; remaining > 0; --remaining)
    {
        const uint32_t shift = kContinuationBits * (remaining - 1u);
        text.push_back(
            static_cast<char>(kContinuationTag | static_cast<uint8_t>((written >> shift) & kContinuationValue)));
    }
}

uint32_t NextCharacter(std::string_view text, uint32_t offset)
{
    if (offset >= text.size())
    {
        return static_cast<uint32_t>(text.size());
    }
    return StepCharacter(text, BoundaryAt(text, offset));
}

uint32_t PreviousCharacter(std::string_view text, uint32_t offset)
{
    const uint32_t limit = std::min(offset, static_cast<uint32_t>(text.size()));
    uint32_t previous = 0;
    for (uint32_t start = 0; start < text.size();)
    {
        const uint32_t next = StepCharacter(text, start);
        if (next >= limit)
        {
            return start;
        }
        previous = next;
        start = next;
    }
    return previous;
}

uint32_t CharacterCount(std::string_view text)
{
    uint32_t characters = 0;
    for (uint32_t start = 0; start < text.size(); start = StepCharacter(text, start))
    {
        ++characters;
    }
    return characters;
}

uint32_t CharacterOffset(std::string_view text, uint32_t index)
{
    uint32_t offset = 0;
    for (uint32_t stepped = 0; stepped < index && offset < text.size(); ++stepped)
    {
        offset = StepCharacter(text, offset);
    }
    return offset;
}

uint32_t CharacterIndex(std::string_view text, uint32_t offset)
{
    const uint32_t limit = std::min(offset, static_cast<uint32_t>(text.size()));
    uint32_t characters = 0;
    for (uint32_t start = 0; start < text.size();)
    {
        const uint32_t next = StepCharacter(text, start);
        if (next > limit)
        {
            break;
        }
        ++characters;
        start = next;
    }
    return characters;
}

} // namespace Assisi::Mondrian
