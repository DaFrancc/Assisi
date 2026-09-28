/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestDisplayedString.cpp
/// @brief A DisplayedString holds `#table:key` or literal text in one spelling,
/// and resolves a key through whatever resolver is installed.

#include <doctest/doctest.h>

#include <ostream>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <Assisi/Core/DisplayedString.hpp>

using Assisi::Core::DisplayedString;
using Assisi::Core::DisplayedStringResolver;
using Assisi::Core::SetDisplayedStringResolver;

namespace
{

/// Installs a resolver that knows one key for the life of a test, and puts the
/// previous one back after.
class ScopedResolver
{
public:
    ScopedResolver()
        : _previous(SetDisplayedStringResolver(
              [](std::string_view table, std::string_view key) -> std::optional<std::string_view>
              {
                  if (table == "pause" && key == "title")
                  {
                      return "Paused";
                  }
                  return std::nullopt;
              }))
    {
    }

    ScopedResolver(const ScopedResolver &)            = delete;
    ScopedResolver &operator=(const ScopedResolver &) = delete;

    ~ScopedResolver() { (void)SetDisplayedStringResolver(std::move(_previous)); }

private:
    DisplayedStringResolver _previous;
};

} // namespace

TEST_CASE("DisplayedString: literal text resolves to itself")
{
    const ScopedResolver resolver;
    const DisplayedString text = DisplayedString::FromSource("Resume");
    CHECK_FALSE(text.Key().has_value());
    CHECK(text.Resolve() == "Resume");
}

TEST_CASE("DisplayedString: a key resolves to its table's words")
{
    const ScopedResolver resolver;
    const DisplayedString text = DisplayedString::FromSource("#pause:title");
    REQUIRE(text.Key().has_value());
    CHECK(*text.Key() == "pause:title");
    CHECK(text.Resolve() == "Paused");
    CHECK(DisplayedString::FromKey("pause", "title") == text);
}

TEST_CASE("DisplayedString: a key no table gives words to shows as the key")
{
    const ScopedResolver resolver;
    CHECK(DisplayedString::FromSource("#pause:missing").Resolve() == "#pause:missing");
    CHECK(DisplayedString::FromSource("#notakey").Resolve() == "#notakey");
}

TEST_CASE("DisplayedString: with no resolver installed a key shows as the key")
{
    DisplayedStringResolver previous = SetDisplayedStringResolver({});
    CHECK(DisplayedString::FromSource("#pause:title").Resolve() == "#pause:title");
    (void)SetDisplayedStringResolver(std::move(previous));
}

TEST_CASE("DisplayedString: a literal starting with the mark is escaped and comes back whole")
{
    const ScopedResolver resolver;
    const DisplayedString text = DisplayedString::FromLiteral("#pause:title");
    CHECK(text.Source() == "##pause:title");
    CHECK_FALSE(text.Key().has_value());
    CHECK(text.Resolve() == "#pause:title");
    CHECK(DisplayedString::FromLiteral("plain").Source() == "plain");
}
