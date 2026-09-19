/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Utf8.hpp>

#include <Assisi/Core/Assert.hpp>

#include <array>
#include <cstddef>

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

} // namespace Assisi::Mondrian
