/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Mondrian/Import/Markup.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <utility>

namespace Assisi::Mondrian::Import
{
namespace
{

/// The five entities XML predefines. No numeric references and no others: a
/// file needing more is a file needing a different parser.
struct Entity
{
    std::string_view name;
    char character;
};

constexpr std::array<Entity, 5> kEntities{{{"lt", '<'}, {"gt", '>'}, {"amp", '&'}, {"quot", '"'}, {"apos", '\''}}};

/// The longest entity name, so an unterminated `&` is refused after a bounded
/// look rather than a scan to the end of the file.
constexpr std::size_t kMaxEntityNameBytes = 4;

bool IsSpace(char character)
{
    return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

/// What may start a name, and what may continue one. Deliberately without `:`,
/// which is how a namespaced document announces itself.
bool IsNameStart(char character)
{
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || character == '_';
}

bool IsNameChar(char character)
{
    return IsNameStart(character) || (character >= '0' && character <= '9') || character == '-' || character == '.';
}

/// The text, and where in it we are. Line and column are carried rather than
/// computed on demand, because every element and attribute records them and a
/// backwards count per token would be quadratic on a long file.
class Scanner
{
  public:
    explicit Scanner(std::string_view text) : _text(text) {}

    [[nodiscard]] bool Done() const { return _position >= _text.size(); }

    [[nodiscard]] char Peek(std::size_t ahead = 0) const
    {
        const std::size_t at = _position + ahead;
        return at < _text.size() ? _text[at] : '\0';
    }

    [[nodiscard]] bool Starts(std::string_view prefix) const { return _text.substr(_position).starts_with(prefix); }

    [[nodiscard]] uint32_t Line() const { return _line; }
    [[nodiscard]] uint32_t Column() const { return _column; }

    char Take()
    {
        const char character = _text[_position];
        ++_position;
        if (character == '\n')
        {
            ++_line;
            _column = 1;
        }
        else
        {
            ++_column;
        }
        return character;
    }

    void Skip(std::size_t count)
    {
        for (std::size_t taken = 0; taken < count && !Done(); ++taken)
        {
            (void)Take();
        }
    }

    void SkipSpace()
    {
        while (!Done() && IsSpace(Peek()))
        {
            (void)Take();
        }
    }

    /// @brief A name, or empty when what is here does not start one.
    std::string TakeName()
    {
        std::string name;
        if (Done() || !IsNameStart(Peek()))
        {
            return name;
        }
        while (!Done() && IsNameChar(Peek()))
        {
            name.push_back(Take());
        }
        return name;
    }

    [[nodiscard]] MarkupError Fail(std::string message) const
    {
        return MarkupError{.message = std::move(message), .line = _line, .column = _column};
    }

  private:
    std::string_view _text;
    std::size_t _position = 0;
    uint32_t _line = 1;
    uint32_t _column = 1;
};

/// Reads one `&name;`, appending what it stands for.
std::expected<void, MarkupError> TakeEntity(Scanner &scanner, std::string &out)
{
    const MarkupError where = scanner.Fail({});
    scanner.Skip(1); // &

    std::string name;
    while (!scanner.Done() && name.size() <= kMaxEntityNameBytes && scanner.Peek() != ';')
    {
        name.push_back(scanner.Take());
    }
    if (scanner.Done() || scanner.Peek() != ';')
    {
        return std::unexpected(MarkupError{.message = "'&' starts an entity that is never closed with ';'. "
                                                      "A literal ampersand is written &amp;.",
                                           .line = where.line,
                                           .column = where.column});
    }
    scanner.Skip(1); // ;

    for (const Entity &entity : kEntities)
    {
        if (entity.name == name)
        {
            out.push_back(entity.character);
            return {};
        }
    }
    return std::unexpected(MarkupError{.message = "'&" + name +
                                                  ";' is not one of the five entities this "
                                                  "markup knows: &lt; &gt; &amp; &quot; &apos;.",
                                       .line = where.line,
                                       .column = where.column});
}

/// Everything after the last non-space character is dropped, and so is
/// everything before the first. What is between them is left alone.
void TrimInPlace(std::string &text)
{
    std::size_t first = 0;
    while (first < text.size() && IsSpace(text[first]))
    {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first && IsSpace(text[last - 1]))
    {
        --last;
    }
    text = text.substr(first, last - first);
}

std::expected<void, MarkupError> SkipComment(Scanner &scanner)
{
    const MarkupError where = scanner.Fail({});
    scanner.Skip(4); // <!--
    while (!scanner.Done())
    {
        if (scanner.Starts("-->"))
        {
            scanner.Skip(3);
            return {};
        }
        (void)scanner.Take();
    }
    return std::unexpected(
        MarkupError{.message = "a comment is never closed with '-->'.", .line = where.line, .column = where.column});
}

/// Whitespace and comments, which may sit anywhere an element may.
std::expected<void, MarkupError> SkipIgnorable(Scanner &scanner)
{
    while (true)
    {
        scanner.SkipSpace();
        if (!scanner.Starts("<!--"))
        {
            return {};
        }
        if (const std::expected<void, MarkupError> skipped = SkipComment(scanner); !skipped)
        {
            return std::unexpected(skipped.error());
        }
    }
}

/// What a `<` that is not an element start announces: a document written
/// against a language this one is a subset of.
std::expected<void, MarkupError> RefuseUnsupported(const Scanner &scanner)
{
    if (scanner.Starts("<?"))
    {
        return std::unexpected(scanner.Fail("this markup has no processing instructions; a '<?xml ... ?>' "
                                            "declaration is not needed and not read."));
    }
    if (scanner.Starts("<!DOCTYPE"))
    {
        return std::unexpected(scanner.Fail("this markup has no DTD; there is nothing for a DOCTYPE to name."));
    }
    if (scanner.Starts("<![CDATA["))
    {
        return std::unexpected(scanner.Fail("this markup has no CDATA sections; write the five entities "
                                            "instead."));
    }
    return {};
}

std::expected<MarkupElement, MarkupError> ParseElement(Scanner &scanner, uint32_t depth);

/// The attributes between a tag's name and its closing bracket.
std::expected<void, MarkupError> ParseAttributes(Scanner &scanner, MarkupElement &element)
{
    while (true)
    {
        scanner.SkipSpace();
        if (scanner.Done() || scanner.Peek() == '>' || scanner.Peek() == '/')
        {
            return {};
        }

        MarkupAttribute attribute;
        attribute.line = scanner.Line();
        attribute.column = scanner.Column();
        attribute.name = scanner.TakeName();
        if (attribute.name.empty())
        {
            if (scanner.Peek() == ':')
            {
                return std::unexpected(scanner.Fail("this markup has no namespaces, so ':' cannot appear in a "
                                                    "name."));
            }
            return std::unexpected(scanner.Fail("expected an attribute name, '>' or '/>'."));
        }

        scanner.SkipSpace();
        if (scanner.Done() || scanner.Peek() != '=')
        {
            return std::unexpected(scanner.Fail("attribute '" + attribute.name +
                                                "' has no value; every attribute is written name=\"value\"."));
        }
        scanner.Skip(1); // =
        scanner.SkipSpace();

        if (scanner.Done() || (scanner.Peek() != '"' && scanner.Peek() != '\''))
        {
            return std::unexpected(
                scanner.Fail("attribute '" + attribute.name + "' has an unquoted value; values are quoted."));
        }
        const char quote = scanner.Take();

        while (!scanner.Done() && scanner.Peek() != quote)
        {
            if (scanner.Peek() == '<')
            {
                return std::unexpected(
                    scanner.Fail("attribute '" + attribute.name + "' holds a '<'; write &lt; instead."));
            }
            if (scanner.Peek() == '&')
            {
                if (const std::expected<void, MarkupError> entity = TakeEntity(scanner, attribute.value); !entity)
                {
                    return std::unexpected(entity.error());
                }
                continue;
            }
            attribute.value.push_back(scanner.Take());
        }
        if (scanner.Done())
        {
            return std::unexpected(scanner.Fail("attribute '" + attribute.name + "' is never closed."));
        }
        scanner.Skip(1); // the closing quote

        // Two of a name is a file whose author believes both took effect.
        for (const MarkupAttribute &existing : element.attributes)
        {
            if (existing.name == attribute.name)
            {
                return std::unexpected(
                    MarkupError{.message = "attribute '" + attribute.name + "' is written twice on the same element.",
                                .line = attribute.line,
                                .column = attribute.column});
            }
        }
        element.attributes.push_back(std::move(attribute));
    }
}

/// The children and text between an element's tags, up to its closing tag.
std::expected<void, MarkupError> ParseContent(Scanner &scanner, MarkupElement &element, uint32_t depth)
{
    std::string text;
    while (true)
    {
        if (scanner.Done())
        {
            return std::unexpected(MarkupError{.message = "'<" + element.name + ">' is never closed.",
                                               .line = element.line,
                                               .column = element.column});
        }

        if (scanner.Starts("</"))
        {
            scanner.Skip(2);
            const uint32_t line = scanner.Line();
            const uint32_t column = scanner.Column();
            const std::string closing = scanner.TakeName();
            if (closing != element.name)
            {
                // Reported where the wrong tag is, naming where the open one
                // started. Either end can be the mistake — a mistyped closing
                // tag, or a missing one — and only both together say which.
                return std::unexpected(MarkupError{.message = "'<" + element.name + ">', opened on line " +
                                                              std::to_string(element.line) + ", is never closed: '</" +
                                                              closing + ">' was found first.",
                                                   .line = line,
                                                   .column = column});
            }
            scanner.SkipSpace();
            if (scanner.Done() || scanner.Peek() != '>')
            {
                return std::unexpected(scanner.Fail("'</" + closing + "' is not closed with '>'."));
            }
            scanner.Skip(1);
            TrimInPlace(text);
            element.text = std::move(text);
            return {};
        }

        if (scanner.Starts("<!--"))
        {
            if (const std::expected<void, MarkupError> skipped = SkipComment(scanner); !skipped)
            {
                return std::unexpected(skipped.error());
            }
            continue;
        }

        if (scanner.Peek() == '<')
        {
            if (const std::expected<void, MarkupError> supported = RefuseUnsupported(scanner); !supported)
            {
                return std::unexpected(supported.error());
            }
            std::expected<MarkupElement, MarkupError> child = ParseElement(scanner, depth + 1);
            if (!child)
            {
                return std::unexpected(child.error());
            }
            element.children.push_back(std::move(*child));
            continue;
        }

        if (scanner.Peek() == '&')
        {
            if (const std::expected<void, MarkupError> entity = TakeEntity(scanner, text); !entity)
            {
                return std::unexpected(entity.error());
            }
            continue;
        }

        text.push_back(scanner.Take());
    }
}

std::expected<MarkupElement, MarkupError> ParseElement(Scanner &scanner, uint32_t depth)
{
    if (depth > kMaxMarkupDepth)
    {
        return std::unexpected(scanner.Fail("elements nest deeper than this markup allows."));
    }

    scanner.Skip(1); // <
    MarkupElement element;
    element.line = scanner.Line();
    element.column = scanner.Column();
    element.name = scanner.TakeName();
    if (element.name.empty())
    {
        return std::unexpected(scanner.Fail("expected an element name after '<'."));
    }
    if (scanner.Peek() == ':')
    {
        return std::unexpected(scanner.Fail("this markup has no namespaces, so ':' cannot appear in a name."));
    }

    if (const std::expected<void, MarkupError> attributes = ParseAttributes(scanner, element); !attributes)
    {
        return std::unexpected(attributes.error());
    }

    if (scanner.Starts("/>"))
    {
        scanner.Skip(2);
        return element;
    }
    if (scanner.Done() || scanner.Peek() != '>')
    {
        return std::unexpected(scanner.Fail("'<" + element.name + "' is not closed with '>' or '/>'."));
    }
    scanner.Skip(1); // >

    if (const std::expected<void, MarkupError> content = ParseContent(scanner, element, depth); !content)
    {
        return std::unexpected(content.error());
    }
    return element;
}

} // namespace

const MarkupAttribute *MarkupElement::Find(std::string_view wanted) const
{
    for (const MarkupAttribute &attribute : attributes)
    {
        if (attribute.name == wanted)
        {
            return &attribute;
        }
    }
    return nullptr;
}

std::expected<MarkupElement, MarkupError> ParseMarkup(std::string_view text)
{
    Scanner scanner{text};

    if (const std::expected<void, MarkupError> skipped = SkipIgnorable(scanner); !skipped)
    {
        return std::unexpected(skipped.error());
    }
    if (scanner.Done())
    {
        return std::unexpected(scanner.Fail("holds no element."));
    }
    if (const std::expected<void, MarkupError> supported = RefuseUnsupported(scanner); !supported)
    {
        return std::unexpected(supported.error());
    }
    if (scanner.Peek() != '<')
    {
        return std::unexpected(scanner.Fail("expected an element, which starts with '<'."));
    }

    std::expected<MarkupElement, MarkupError> root = ParseElement(scanner, 0);
    if (!root)
    {
        return root;
    }

    // One root, so that what a file describes is one thing. A second element
    // here is a file whose author expected both to be read.
    if (const std::expected<void, MarkupError> skipped = SkipIgnorable(scanner); !skipped)
    {
        return std::unexpected(skipped.error());
    }
    if (!scanner.Done())
    {
        return std::unexpected(scanner.Fail("holds more than one top-level element."));
    }
    return root;
}

} // namespace Assisi::Mondrian::Import
