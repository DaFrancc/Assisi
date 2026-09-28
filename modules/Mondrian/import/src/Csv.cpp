/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Mondrian/Import/Csv.hpp>

#include <cstddef>
#include <utility>

namespace Assisi::Mondrian::Import
{
namespace
{

/// What a spreadsheet's "CSV UTF-8" puts before the first byte of text.
constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";

constexpr char kSeparator = ',';
constexpr char kQuote = '"';
constexpr char kLineFeed = '\n';
constexpr char kCarriageReturn = '\r';

/// Where the reader is, for an error to point at.
struct Position
{
    std::size_t index = 0;
    uint32_t line = 1;
    uint32_t column = 1;
};

/// Moves @p at past one character of @p text, counting a line feed as the start
/// of a line.
void Advance(std::string_view text, Position &at)
{
    if (text[at.index] == kLineFeed)
    {
        ++at.line;
        at.column = 1;
    }
    else
    {
        ++at.column;
    }
    ++at.index;
}

/// Whether @p at is on a line break, LF or CRLF.
bool AtLineBreak(std::string_view text, const Position &at)
{
    if (text[at.index] == kLineFeed)
    {
        return true;
    }
    return text[at.index] == kCarriageReturn && at.index + 1 < text.size() && text[at.index + 1] == kLineFeed;
}

/// Moves @p at past the line break it is on.
void SkipLineBreak(std::string_view text, Position &at)
{
    if (text[at.index] == kCarriageReturn)
    {
        Advance(text, at);
    }
    Advance(text, at);
}

/// Reads the quoted field starting at @p at, leaving @p at on whatever follows
/// its closing quote.
std::expected<std::string, MarkupError> ReadQuoted(std::string_view text, Position &at)
{
    const Position opened = at;
    Advance(text, at);
    std::string field;
    while (at.index < text.size())
    {
        if (text[at.index] != kQuote)
        {
            field += text[at.index];
            Advance(text, at);
            continue;
        }
        // A doubled quote is one quote inside the field.
        if (at.index + 1 < text.size() && text[at.index + 1] == kQuote)
        {
            field += kQuote;
            Advance(text, at);
            Advance(text, at);
            continue;
        }
        const Position closed = at;
        Advance(text, at);
        if (at.index < text.size() && text[at.index] != kSeparator && !AtLineBreak(text, at))
        {
            return std::unexpected(MarkupError{.message = "a quoted field ends here, but more follows it before the "
                                                          "next comma. Put the whole field inside the quotes.",
                                               .line = closed.line,
                                               .column = closed.column});
        }
        return field;
    }
    return std::unexpected(MarkupError{.message = "this quote is never closed. A quote inside a quoted field is "
                                                  "written twice: \"\".",
                                       .line = opened.line,
                                       .column = opened.column});
}

/// The rows read so far, and the row and field being read.
struct Rows
{
    std::vector<CsvRow> done;
    CsvRow row;
    std::string field;
    /// Whether the field being read was quoted, so an empty quoted field at the
    /// end of the text still counts as a field.
    bool quoted = false;

    /// Ends the field being read.
    void EndField()
    {
        row.fields.push_back(std::move(field));
        field.clear();
        quoted = false;
    }

    /// Ends the field being read and the row it is in. The next row starts on
    /// @p nextLine. A row holding one empty field is a blank line, and dropped.
    void EndRow(uint32_t nextLine)
    {
        EndField();
        if (row.fields.size() != 1 || !row.fields[0].empty())
        {
            done.push_back(std::move(row));
        }
        row = CsvRow{.fields = {}, .line = nextLine};
    }

    /// Whether anything of a row has been read since the last one ended.
    [[nodiscard]] bool Started() const { return !field.empty() || quoted || !row.fields.empty(); }
};

} // namespace

std::expected<std::vector<CsvRow>, MarkupError> ReadCsv(std::string_view text)
{
    if (text.starts_with(kByteOrderMark))
    {
        text.remove_prefix(kByteOrderMark.size());
    }

    Rows rows;
    Position at;
    while (at.index < text.size())
    {
        const char next = text[at.index];
        // A quote opens a field only where the field starts; inside one it is
        // an ordinary character.
        if (next == kQuote && rows.field.empty() && !rows.quoted)
        {
            std::expected<std::string, MarkupError> read = ReadQuoted(text, at);
            if (!read)
            {
                return std::unexpected(read.error());
            }
            rows.field = std::move(*read);
            rows.quoted = true;
            continue;
        }
        if (next == kSeparator)
        {
            Advance(text, at);
            rows.EndField();
            continue;
        }
        if (AtLineBreak(text, at))
        {
            SkipLineBreak(text, at);
            rows.EndRow(at.line);
            continue;
        }
        rows.field += next;
        Advance(text, at);
    }
    // A last row with no line break after it.
    if (rows.Started())
    {
        rows.EndRow(at.line);
    }
    return std::move(rows.done);
}

} // namespace Assisi::Mondrian::Import
