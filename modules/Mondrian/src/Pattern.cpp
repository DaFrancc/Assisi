/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Pattern.hpp>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace Assisi::Mondrian
{
namespace
{

/// How far the matcher may go before it gives up: the number of times it is
/// allowed to try a different way through the pattern, and how deep it may
/// nest while doing so. A pattern and a subject that together need more than
/// this are the pathological pair — a field's text is tens of characters, and
/// anything sane finishes in a handful of steps.
constexpr uint32_t kMatchLimit = 10000;
constexpr uint32_t kDepthLimit = 1000;

/// How long an error message from the library may be, in bytes. Its longest is
/// well under this; anything that does not fit is truncated rather than lost.
constexpr std::size_t kMessageLength = 256;

/// What a pattern is wrapped in so that it must account for the whole field.
///
/// The end anchor is written into the pattern rather than asked for as an
/// option because the library refuses to end-anchor and partial-match at once,
/// and the partial match is the half that cannot be given up: it is what tells
/// a half-typed field from a wrong one. The group keeps a pattern with
/// alternatives at its top level — "yes|no" — from binding the anchor to only
/// its last branch.
constexpr std::string_view kWrapBefore = "(?:";
constexpr std::string_view kWrapAfter = ")\\z";

const PCRE2_UCHAR8 *Bytes(std::string_view text)
{
    return reinterpret_cast<const PCRE2_UCHAR8 *>(text.data());
}

} // namespace

/// A compiled pattern and the limits it is matched under. Holds the library's
/// own objects, which is why nothing outside this file may see inside it.
class Pattern
{
  public:
    Pattern(pcre2_code *code, pcre2_match_context *context) : _code(code), _context(context) {}

    ~Pattern()
    {
        pcre2_match_context_free(_context);
        pcre2_code_free(_code);
    }

    // Holds two owned handles, and is shared by pointer rather than copied.
    Pattern(const Pattern &) = delete;
    Pattern &operator=(const Pattern &) = delete;

    [[nodiscard]] const pcre2_code *Code() const { return _code; }
    [[nodiscard]] pcre2_match_context *Context() const { return _context; }

  private:
    pcre2_code *_code = nullptr;
    pcre2_match_context *_context = nullptr;
};

std::expected<std::shared_ptr<const Pattern>, PatternError> CompilePattern(std::string_view pattern)
{
    // UTF makes the pattern and the subject sequences of characters rather than
    // bytes; UCP makes the shorthands that name kinds of character — a letter,
    // a digit, a space — mean what they mean in every language rather than in
    // ASCII alone. ANCHORED holds the start; the wrapping holds the end,
    // because a pattern describes the whole of what a field holds: one that
    // matched a part of it would call "x42" an integer.
    constexpr uint32_t kOptions = PCRE2_UTF | PCRE2_UCP | PCRE2_ANCHORED;

    std::string whole;
    whole.reserve(kWrapBefore.size() + pattern.size() + kWrapAfter.size());
    whole.append(kWrapBefore).append(pattern).append(kWrapAfter);

    int32_t code = 0;
    PCRE2_SIZE offset = 0;
    pcre2_code *compiled = pcre2_compile(Bytes(whole), whole.size(), kOptions, &code, &offset, nullptr);
    if (compiled == nullptr)
    {
        std::array<PCRE2_UCHAR8, kMessageLength> message{};
        const int32_t written = pcre2_get_error_message(code, message.data(), message.size());
        // Back into the caller's own pattern, which is the only text they wrote
        // and the only one an offset can usefully point into.
        const std::size_t reported = offset > kWrapBefore.size() ? offset - kWrapBefore.size() : 0;
        return std::unexpected(PatternError{.message = written > 0
                                                           ? std::string(reinterpret_cast<const char *>(message.data()),
                                                                         static_cast<std::size_t>(written))
                                                           : std::string("the pattern could not be compiled"),
                                            .offset = static_cast<uint32_t>(std::min(reported, pattern.size()))});
    }

    pcre2_match_context *context = pcre2_match_context_create(nullptr);
    if (context == nullptr)
    {
        pcre2_code_free(compiled);
        return std::unexpected(PatternError{.message = "the pattern could not be prepared for matching", .offset = 0});
    }
    pcre2_set_match_limit(context, kMatchLimit);
    pcre2_set_depth_limit(context, kDepthLimit);
    return std::make_shared<const Pattern>(compiled, context);
}

PatternMatch MatchPattern(const Pattern &pattern, std::string_view text)
{
    // PARTIAL_SOFT is what separates "not yet" from "never": it reports a
    // partial match only when the subject ran out mid-pattern, and still
    // prefers a full match wherever one exists. The anchoring that makes this
    // the whole subject or nothing is compiled into the pattern.
    constexpr uint32_t kOptions = PCRE2_PARTIAL_SOFT;

    pcre2_match_data *data = pcre2_match_data_create_from_pattern(pattern.Code(), nullptr);
    if (data == nullptr)
    {
        return PatternMatch::No;
    }
    const int32_t result = pcre2_match(pattern.Code(), Bytes(text), text.size(), 0, kOptions, data, pattern.Context());
    pcre2_match_data_free(data);

    if (result >= 0)
    {
        return PatternMatch::Yes;
    }
    // Every other outcome — no match, a limit reached, a subject that is not
    // valid UTF-8 — is text this pattern does not accept.
    return result == PCRE2_ERROR_PARTIAL ? PatternMatch::Partial : PatternMatch::No;
}

} // namespace Assisi::Mondrian
