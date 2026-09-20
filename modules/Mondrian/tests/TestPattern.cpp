/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Pattern.hpp>

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <string_view>

using namespace Assisi::Mondrian;

namespace
{

/// @p pattern compiled, which the cases below all expect to succeed.
std::shared_ptr<const Pattern> Compiled(std::string_view pattern)
{
    const std::expected<std::shared_ptr<const Pattern>, PatternError> compiled = CompilePattern(pattern);
    REQUIRE(compiled.has_value());
    REQUIRE(*compiled != nullptr);
    return *compiled;
}

PatternMatch Match(std::string_view pattern, std::string_view text)
{
    return MatchPattern(*Compiled(pattern), text);
}

} // namespace

TEST_CASE("Pattern: a pattern that does not compile says so, and where")
{
    const std::expected<std::shared_ptr<const Pattern>, PatternError> compiled = CompilePattern("[unclosed");
    REQUIRE_FALSE(compiled.has_value());
    CHECK_FALSE(compiled.error().message.empty());
    CHECK(compiled.error().offset > 0);
}

TEST_CASE("Pattern: text the pattern describes matches, and text it never could does not")
{
    CHECK(Match(Patterns::kInteger, "42") == PatternMatch::Yes);
    CHECK(Match(Patterns::kInteger, "-42") == PatternMatch::Yes);
    CHECK(Match(Patterns::kInteger, "4x") == PatternMatch::No);
    CHECK(Match(Patterns::kAlphabetic, "abc") == PatternMatch::Yes);
    CHECK(Match(Patterns::kAlphabetic, "ab3") == PatternMatch::No);
    CHECK(Match(Patterns::kAlphanumeric, "ab3") == PatternMatch::Yes);
    CHECK(Match(Patterns::kReal, "3.5") == PatternMatch::Yes);
    CHECK(Match(Patterns::kEmail, "jim@example.com") == PatternMatch::Yes);
}

TEST_CASE("Pattern: text on its way to matching is neither a match nor a refusal")
{
    // The whole point: each of these is what a field holds partway through
    // being typed, and refusing any of them would make the field untypeable.
    CHECK(Match(Patterns::kInteger, "-") == PatternMatch::Partial);
    CHECK(Match(Patterns::kReal, "3.") == PatternMatch::Partial);
    CHECK(Match(Patterns::kEmail, "jim@") == PatternMatch::Partial);
    CHECK(Match(Patterns::kEmail, "jim@example") == PatternMatch::Partial);
}

TEST_CASE("Pattern: empty text is a refusal rather than a beginning")
{
    // Nothing is on its way to anywhere: a pattern that wants a character and
    // is given none reports no match, not a partial one. A field therefore
    // allows itself to be emptied on its own account rather than asking a
    // pattern whether emptying is allowed — otherwise a filled field could
    // never be cleared.
    for (const std::string_view pattern :
         {Patterns::kInteger, Patterns::kReal, Patterns::kEmail, Patterns::kAlphabetic, Patterns::kAlphanumeric})
    {
        CAPTURE(pattern);
        CHECK(Match(pattern, "") == PatternMatch::No);
    }

    // Unless the pattern accepts nothing at all, which is then a full match.
    CHECK(Match("[0-9]*", "") == PatternMatch::Yes);
}

TEST_CASE("Pattern: a pattern matches whole text rather than finding itself inside it")
{
    CHECK(Match(Patterns::kInteger, "x42") == PatternMatch::No);
    CHECK(Match("abc", "abcd") == PatternMatch::No);
}

TEST_CASE("Pattern: a pattern counts codepoints rather than the bytes they take")
{
    // Ten bytes, four codepoints, three characters to a reader: an e, the
    // accent written on it, an emoji, a b. A dot is one codepoint, so the
    // accent costs one of its own.
    constexpr std::string_view kMixed = "e\xCC\x81\xF0\x9F\x98\x80"
                                        "b";
    CHECK(Match("....", kMixed) == PatternMatch::Yes);
    CHECK(Match(".....", kMixed) == PatternMatch::Partial);
    CHECK(Match("...", kMixed) == PatternMatch::No);

    // What a caret counts is a whole character, which a pattern spells \X.
    CHECK(Match("\\X\\X\\X", kMixed) == PatternMatch::Yes);
}

TEST_CASE("Pattern: a pattern that would backtrack without end gives up instead")
{
    // The classic blowup: without a bound this runs for longer than anyone will
    // wait. What matters is that it returns at all, and refuses rather than
    // claiming a match it never found.
    CHECK(Match("(a+)+b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa!") == PatternMatch::No);
}
