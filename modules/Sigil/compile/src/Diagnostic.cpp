/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Sigil/Compile/Diagnostic.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <string_view>

namespace Assisi::Sigil::Compile
{

namespace
{

constexpr std::string_view kReset = "\x1b[0m";
constexpr std::string_view kBold = "\x1b[1m";
constexpr std::string_view kRed = "\x1b[1;31m";
constexpr std::string_view kYellow = "\x1b[1;33m";
constexpr std::string_view kBlue = "\x1b[1;34m";

/// One underlined stretch of a line: `^` for the place itself, `-` for a
/// related one, with its label. Column is zero-based.
struct Mark
{
    std::string_view label;
    std::size_t column = 0;
    std::size_t length = 1;
    bool primary = false;
};

/// A source line and the marks on it, left to right.
struct MarkedLine
{
    std::string_view excerpt;
    std::vector<Mark> marks;
    uint32_t line = 0;
};

/// Something written at a column of a row under the source line.
struct Piece
{
    std::string text;
    std::string_view color;
    std::size_t column = 0;
};

class Writer
{
  public:
    Writer(const Diagnostic &diagnostic, Style style) : _diagnostic(diagnostic), _style(style) {}

    std::string Paint(std::string_view text, std::string_view color) const
    {
        if (_style == Style::Plain || color.empty())
        {
            return std::string{text};
        }
        return std::format("{}{}{}", color, text, kReset);
    }

    std::string_view SeverityColor() const
    {
        return _diagnostic.severity == Severity::Warning ? kYellow : kRed;
    }

    std::string_view MarkColor(const Mark &mark) const { return mark.primary ? SeverityColor() : kBlue; }

    /// The gutter: blank, or a line number, right-aligned to @p width, then a bar.
    std::string Gutter(std::size_t width, std::string_view number = {}) const
    {
        const std::string padded = std::format("{:>{}}", number, width);
        return std::format("{} {}", Paint(padded, kBlue), Paint("|", kBlue));
    }

    /// @p pieces laid out at their columns under @p excerpt. The gaps copy the
    /// excerpt's tabs, so the pieces line up however wide a reader's tabs are.
    std::string Row(std::string_view excerpt, std::vector<Piece> pieces) const
    {
        std::ranges::sort(pieces, {}, &Piece::column);
        std::string row;
        std::size_t at = 0;
        for (const Piece &piece : pieces)
        {
            for (; at < piece.column; ++at)
            {
                row += at < excerpt.size() && excerpt[at] == '\t' ? '\t' : ' ';
            }
            row += Paint(piece.text, piece.color);
            at += piece.text.size();
        }
        return row;
    }

    /// The rows under one source line: the marks with the rightmost label
    /// beside them, then a bar down from each other labelled mark to its
    /// label, the way the rightmost is written first.
    std::string Underlines(const MarkedLine &marked, std::size_t width) const
    {
        std::vector<Piece> markers;
        for (const Mark &mark : marked.marks)
        {
            const std::string line(std::max<std::size_t>(mark.length, 1), mark.primary ? '^' : '-');
            markers.push_back(Piece{.text = line, .color = MarkColor(mark), .column = mark.column});
        }
        const Mark &last = marked.marks.back();
        if (!last.label.empty())
        {
            markers.push_back(Piece{.text = std::string{last.label},
                                    .color = MarkColor(last),
                                    .column = last.column + std::max<std::size_t>(last.length, 1) + 1});
        }
        std::string text = std::format("{} {}\n", Gutter(width), Row(marked.excerpt, markers));

        std::vector<const Mark *> waiting;
        for (std::size_t i = 0; i + 1 < marked.marks.size(); ++i)
        {
            if (!marked.marks[i].label.empty())
            {
                waiting.push_back(&marked.marks[i]);
            }
        }
        while (!waiting.empty())
        {
            std::vector<Piece> bars;
            for (const Mark *mark : waiting)
            {
                bars.push_back(Piece{.text = "|", .color = MarkColor(*mark), .column = mark->column});
            }
            text += std::format("{} {}\n", Gutter(width), Row(marked.excerpt, bars));
            const Mark &labelled = *waiting.back();
            bars.back() = Piece{.text = std::string{labelled.label}, .color = MarkColor(labelled), .column = labelled.column};
            text += std::format("{} {}\n", Gutter(width), Row(marked.excerpt, bars));
            waiting.pop_back();
        }
        return text;
    }

    /// Adds @p mark to the line @p span is on in @p lines, adding the line first.
    static void AddMark(std::vector<MarkedLine> &lines, std::string_view excerpt, const Span &span, Mark mark)
    {
        std::vector<MarkedLine>::iterator line = std::ranges::find(lines, span.where.line, &MarkedLine::line);
        if (line == lines.end())
        {
            lines.push_back(MarkedLine{.excerpt = excerpt, .marks = {}, .line = span.where.line});
            line = lines.end() - 1;
        }
        mark.column = span.where.column - 1;
        mark.length = span.length;
        line->marks.push_back(mark);
    }

    /// The place and every related one, grouped by line, top to bottom.
    std::vector<MarkedLine> Lines() const
    {
        std::vector<MarkedLine> lines;
        AddMark(lines, _diagnostic.excerpt, Span{.where = _diagnostic.where, .length = _diagnostic.length},
                Mark{.label = _diagnostic.label, .column = 0, .length = 1, .primary = true});
        for (const Related &related : _diagnostic.related)
        {
            AddMark(lines, related.excerpt, related.span,
                    Mark{.label = related.label, .column = 0, .length = 1, .primary = false});
        }
        std::ranges::sort(lines, {}, &MarkedLine::line);
        for (MarkedLine &line : lines)
        {
            std::ranges::sort(line.marks, {}, &Mark::column);
        }
        return lines;
    }

    std::string Write() const
    {
        const bool warning = _diagnostic.severity == Severity::Warning;
        std::string text = std::format("{} {}\n", Paint(warning ? "warning:" : "error:", SeverityColor()),
                                       Paint(_diagnostic.message, kBold));
        const std::vector<MarkedLine> lines = Lines();
        std::size_t width = 1;
        for (const MarkedLine &line : lines)
        {
            width = std::max(width, std::to_string(line.line).size());
        }
        text += std::format("{}{} {}:{}:{}\n", std::string(width, ' '), Paint("-->", kBlue), _diagnostic.file,
                            _diagnostic.where.line, _diagnostic.where.column);
        if (!_diagnostic.excerpt.empty())
        {
            text += Gutter(width) + "\n";
            for (const MarkedLine &line : lines)
            {
                text += std::format("{} {}\n", Gutter(width, std::to_string(line.line)), line.excerpt);
                text += Underlines(line, width);
            }
        }
        if (!_diagnostic.help.empty())
        {
            text += std::format("{}\n{} {} {}\n", Gutter(width), std::string(width, ' '), Paint("= help:", kBold),
                                _diagnostic.help);
        }
        return text;
    }

  private:
    const Diagnostic &_diagnostic;
    Style _style;
};

} // namespace

std::string Format(const Diagnostic &diagnostic, Style style)
{
    return Writer{diagnostic, style}.Write();
}

bool HasErrors(std::span<const Diagnostic> diagnostics)
{
    return std::ranges::any_of(diagnostics,
                               [](const Diagnostic &diagnostic) { return diagnostic.severity == Severity::Error; });
}

} // namespace Assisi::Sigil::Compile
