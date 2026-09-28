/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/DisplayedString.hpp>

#include <utility>

namespace Assisi::Core
{
namespace
{

DisplayedStringResolver &InstalledResolver()
{
    static DisplayedStringResolver resolver;
    return resolver;
}

/// Whether @p text starts with the escaped mark, `##`.
bool StartsEscaped(std::string_view text)
{
    return text.size() > 1 && text[0] == kKeyMark && text[1] == kKeyMark;
}

} // namespace

std::optional<TableKey> SplitKey(std::string_view qualified)
{
    const std::size_t separator = qualified.find(kTableSeparator);
    if (separator == std::string_view::npos || separator == 0 || separator + 1 == qualified.size())
    {
        return std::nullopt;
    }
    return TableKey{.table = qualified.substr(0, separator), .key = qualified.substr(separator + 1)};
}

DisplayedStringResolver SetDisplayedStringResolver(DisplayedStringResolver resolver)
{
    return std::exchange(InstalledResolver(), std::move(resolver));
}

DisplayedString DisplayedString::FromSource(std::string_view source)
{
    DisplayedString text;
    text._source = source;
    return text;
}

DisplayedString DisplayedString::FromKey(std::string_view table, std::string_view key)
{
    DisplayedString text;
    text._source.reserve(sizeof(kKeyMark) + table.size() + sizeof(kTableSeparator) + key.size());
    text._source += kKeyMark;
    text._source += table;
    text._source += kTableSeparator;
    text._source += key;
    return text;
}

DisplayedString DisplayedString::FromLiteral(std::string_view literal)
{
    DisplayedString text;
    if (!literal.empty() && literal[0] == kKeyMark)
    {
        text._source += kKeyMark;
    }
    text._source += literal;
    return text;
}

std::optional<std::string_view> DisplayedString::Key() const
{
    if (_source.empty() || _source[0] != kKeyMark || StartsEscaped(_source))
    {
        return std::nullopt;
    }
    return std::string_view{_source}.substr(1);
}

std::string DisplayedString::Resolve() const
{
    if (StartsEscaped(_source))
    {
        return _source.substr(1);
    }
    const std::optional<std::string_view> key = Key();
    if (!key)
    {
        return _source;
    }
    const DisplayedStringResolver &resolver = InstalledResolver();
    const std::optional<TableKey> split     = SplitKey(*key);
    if (resolver && split)
    {
        if (const std::optional<std::string_view> words = resolver(split->table, split->key))
        {
            return std::string{*words};
        }
    }
    return _source;
}

} // namespace Assisi::Core
