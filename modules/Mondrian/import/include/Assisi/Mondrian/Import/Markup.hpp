/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Markup.hpp
/// @brief An XML subset, parsed into a tree of elements that still knows where
/// each of its parts came from.
///
/// Elements, attributes, text and comments, and nothing else: no DTD, no
/// namespaces, no processing instructions. Each of those is refused by name
/// rather than skipped, because a file carrying one was written against a
/// different language and silently ignoring it would hide that.
///
/// Every element and every attribute keeps its line and column. That is the
/// whole reason this produces a tree rather than calling straight into a
/// builder: the error a person reads comes from the compiler above, long after
/// the text has been consumed, and it has to be able to say where.
///
/// Cook-side only. The shipped game parses no source text.

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Mondrian::Import
{

/// @brief How deep elements may nest. A bound, so a file cannot drive the
/// parser's recursion off the stack.
inline constexpr uint32_t kMaxMarkupDepth = 64;

/// @brief What went wrong, and where a person should look.
///
/// Line and column are one-based, counted the way an editor shows them, so the
/// message can be pasted at a file and land on the character.
struct MarkupError
{
    std::string message;
    uint32_t line = 1;
    uint32_t column = 1;
};

/// @brief One attribute, with where it was written.
struct MarkupAttribute
{
    std::string name;
    std::string value;
    /// Where the name starts, which is what an error about either half points
    /// at — an attribute is short enough that the name is close enough.
    uint32_t line = 1;
    uint32_t column = 1;
};

/// @brief One element: its name, its attributes, its text and its children.
struct MarkupElement
{
    std::string name;

    /// The text written directly inside this element, with the whitespace at
    /// each end removed and everything between it kept. A label is written as
    /// text, and what somebody typed between two words is what they meant.
    std::string text;

    std::vector<MarkupAttribute> attributes;
    std::vector<MarkupElement> children;

    /// Where the opening tag's name starts.
    uint32_t line = 1;
    uint32_t column = 1;

    /// @brief The attribute called @p wanted, or null.
    [[nodiscard]] const MarkupAttribute *Find(std::string_view wanted) const;
};

/// @brief Parses @p text into its single root element.
///
/// A document has exactly one root: the text before and after it may be
/// whitespace and comments, and anything else is an error naming where.
[[nodiscard]] std::expected<MarkupElement, MarkupError> ParseMarkup(std::string_view text);

} // namespace Assisi::Mondrian::Import
