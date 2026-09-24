/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "MarkupValues.hpp"

#include <Assisi/Mondrian/Pattern.hpp>
#include <Assisi/Mondrian/Screen.hpp>

#include <charconv>
#include <cmath>
#include <span>
#include <string>

namespace Assisi::Mondrian::Import
{
namespace
{

constexpr std::array<NamedEnum<Alignment>, 3> kAlignments{
    {{"start", Alignment::Start}, {"center", Alignment::Center}, {"end", Alignment::End}}};

constexpr std::array<NamedEnum<Direction>, 2> kDirections{{{"row", Direction::Row}, {"column", Direction::Column}}};

constexpr std::array<NamedEnum<CornerStyle>, 3> kCornerStyles{
    {{"square", CornerStyle::Square}, {"rounded", CornerStyle::Rounded}, {"cut", CornerStyle::Cut}}};

constexpr std::array<NamedEnum<TextAlign>, 3> kTextAligns{
    {{"left", TextAlign::Left}, {"center", TextAlign::Center}, {"right", TextAlign::Right}}};

constexpr std::array<NamedEnum<ScrollBarVisibility>, 3> kScrollBarVisibilities{
    {{"never", ScrollBarVisibility::Never},
     {"when-needed", ScrollBarVisibility::WhenNeeded},
     {"always", ScrollBarVisibility::Always}}};

constexpr std::array<NamedEnum<ScrollBarDrag>, 2> kScrollBarDrags{
    {{"follows-pointer", ScrollBarDrag::FollowsPointer}, {"smoothed", ScrollBarDrag::Smoothed}}};

constexpr std::array<NamedEnum<FloatAnchor>, 2> kFloatAnchors{
    {{"parent", FloatAnchor::Parent}, {"root", FloatAnchor::Root}}};

constexpr std::array<NamedEnum<TextMask>, 2> kTextMasks{{{"none", TextMask::None}, {"dots", TextMask::Dots}}};

constexpr std::array<NamedEnum<TextCheck>, 3> kTextChecks{
    {{"refuse", TextCheck::Refuse}, {"on-change", TextCheck::OnChange}, {"on-commit", TextCheck::OnCommit}}};

/// The patterns a file names rather than spells. A name is what an author
/// writes for the rules every game wants; the expression behind it is what the
/// file carries, so nothing downstream looks a name up.
constexpr std::array<NamedEnum<std::string_view>, 5> kPatterns{{{"alphabetic", Patterns::kAlphabetic},
                                                                {"alphanumeric", Patterns::kAlphanumeric},
                                                                {"integer", Patterns::kInteger},
                                                                {"real", Patterns::kReal},
                                                                {"email", Patterns::kEmail}}};

/// The words a field's height is written with, after how many lines it holds.
constexpr std::string_view kUpToWord = "up-to";
constexpr std::string_view kExactlyWord = "exactly";

/// How many words `lines` has when it carries a height: the count, the word,
/// and the number of lines.
constexpr std::size_t kLinesWithHeight = 3;

/// The layers the engine names, which is what a file writes instead of the
/// numbers they happen to be.
constexpr std::array<NamedEnum<int32_t>, 4> kSortLayers{
    {{"hud", kSortHud}, {"menu", kSortMenu}, {"popup", kSortPopup}, {"overlay", kSortOverlay}}};

/// How many hex digits each colour form has.
constexpr std::size_t kHexRgbDigits = 6;
constexpr std::size_t kHexRgbaDigits = 8;
constexpr std::size_t kHexDigitsPerChannel = 2;

/// What a byte channel is divided by to reach the 0-to-1 a colour holds.
constexpr float kChannelMax = 255.f;

/// The counts each multi-number form accepts.
constexpr std::size_t kPaddingAllEdges = 1;
constexpr std::size_t kPaddingPerEdge = 4;

/// How many channels a colour call takes: red, green and blue, and alpha after
/// them when it is not opaque.
constexpr std::size_t kColorRgbChannels = 3;
constexpr std::size_t kColorRgbaChannels = 4;

/// The colour calls, by the scale their channels are written on.
constexpr std::string_view kRgbCall = "rgb";
constexpr std::string_view kRgbfCall = "rgbf";

/// What marks a colour as hex.
constexpr char kHexMark = '#';

/// What separates a call's name from its arguments, the arguments from each
/// other, and ends the call.
constexpr char kCallOpen = '(';
constexpr char kCallSeparator = ',';
constexpr char kCallClose = ')';

/// The whitespace an author may put around a call's name and arguments.
constexpr std::string_view kCallSpace = " \t\r\n";

std::optional<uint32_t> ParseHexPair(std::string_view digits)
{
    uint32_t value = 0;
    const std::from_chars_result read = std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
    if (read.ec != std::errc{} || read.ptr != digits.data() + digits.size())
    {
        return std::nullopt;
    }
    return value;
}

std::optional<Color> ParseHexColor(std::string_view text)
{
    const std::string_view digits = text.substr(1);
    if (digits.size() != kHexRgbDigits && digits.size() != kHexRgbaDigits)
    {
        return std::nullopt;
    }

    Color color{0.f, 0.f, 0.f, 1.f};
    for (std::size_t channel = 0; channel * kHexDigitsPerChannel < digits.size(); ++channel)
    {
        const std::optional<uint32_t> byte =
            ParseHexPair(digits.substr(channel * kHexDigitsPerChannel, kHexDigitsPerChannel));
        if (!byte)
        {
            return std::nullopt;
        }
        color[static_cast<glm::length_t>(channel)] = static_cast<float>(*byte) / kChannelMax;
    }
    return color;
}

/// Reads the `min`/`max` clauses a sizing may end with, in either order.
bool ApplyBounds(std::span<const std::string_view> words, Sizing &sizing)
{
    for (std::size_t at = 0; at < words.size(); at += 2)
    {
        if (at + 1 >= words.size())
        {
            return false;
        }
        const std::optional<float> bound = ParseFloat(words[at + 1]);
        if (!bound)
        {
            return false;
        }
        if (words[at] == "min")
        {
            sizing.min = *bound;
        }
        else if (words[at] == "max")
        {
            sizing.max = *bound;
        }
        else
        {
            return false;
        }
    }
    return true;
}

} // namespace

std::vector<std::string_view> SplitWords(std::string_view text)
{
    std::vector<std::string_view> words;
    std::size_t at = 0;
    while (at < text.size())
    {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r'))
        {
            ++at;
        }
        const std::size_t start = at;
        while (at < text.size() && text[at] != ' ' && text[at] != '\t' && text[at] != '\n' && text[at] != '\r')
        {
            ++at;
        }
        if (at > start)
        {
            words.push_back(text.substr(start, at - start));
        }
    }
    return words;
}

std::optional<bool> ParseBool(std::string_view text)
{
    if (text == "true")
    {
        return true;
    }
    if (text == "false")
    {
        return false;
    }
    return std::nullopt;
}

std::optional<float> ParseFloat(std::string_view text)
{
    float value = 0.f;
    const std::from_chars_result read = std::from_chars(text.data(), text.data() + text.size(), value);
    if (read.ec != std::errc{} || read.ptr != text.data() + text.size())
    {
        return std::nullopt;
    }
    return value;
}

std::optional<int32_t> ParseInt(std::string_view text)
{
    int32_t value = 0;
    const std::from_chars_result read = std::from_chars(text.data(), text.data() + text.size(), value);
    if (read.ec != std::errc{} || read.ptr != text.data() + text.size())
    {
        return std::nullopt;
    }
    return value;
}

std::optional<uint32_t> ParseUInt(std::string_view text)
{
    uint32_t value = 0;
    const std::from_chars_result read = std::from_chars(text.data(), text.data() + text.size(), value);
    if (read.ec != std::errc{} || read.ptr != text.data() + text.size())
    {
        return std::nullopt;
    }
    return value;
}

std::string_view TrimSpace(std::string_view text)
{
    const std::size_t first = text.find_first_not_of(kCallSpace);
    if (first == std::string_view::npos)
    {
        return {};
    }
    const std::size_t last = text.find_last_not_of(kCallSpace);
    return text.substr(first, last - first + 1);
}

std::expected<std::optional<MarkupCall>, std::string> SplitCall(std::string_view text)
{
    const std::size_t open = text.find(kCallOpen);
    if (open == std::string_view::npos)
    {
        return std::optional<MarkupCall>{};
    }
    const std::size_t close = text.find(kCallClose, open);
    if (close == std::string_view::npos)
    {
        return std::unexpected("'" + std::string{text} + "' opens a call and never closes it with " + kCallClose + ".");
    }
    const std::string_view after = TrimSpace(text.substr(close + 1));
    if (!after.empty())
    {
        return std::unexpected("'" + std::string{after} + "' follows the call in '" + std::string{text} +
                               "'. An attribute holds one call.");
    }

    MarkupCall call;
    call.name = TrimSpace(text.substr(0, open));
    const std::string_view inside = TrimSpace(text.substr(open + 1, close - open - 1));
    // Nothing between the parens is no arguments, not one empty one.
    std::size_t start = 0;
    while (!inside.empty() && start <= inside.size())
    {
        std::size_t end = inside.find(kCallSeparator, start);
        end = end == std::string_view::npos ? inside.size() : end;
        call.arguments.push_back(TrimSpace(inside.substr(start, end - start)));
        start = end + 1;
    }
    return std::optional<MarkupCall>{call};
}

namespace
{

/// One channel of an `rgb` call: a whole number from 0 to 255, as the 0-to-1 a
/// colour holds. A fraction is the likeliest sign of a 0-to-1 value written in
/// the wrong call, so the message says which call takes those.
std::expected<float, std::string> ReadByteChannel(std::string_view written)
{
    const std::optional<float> value = ParseFloat(written);
    if (!value || *value != std::floor(*value))
    {
        return std::unexpected("'" + std::string{written} + "' is not a whole number from 0 to 255. For channels " +
                               "from 0 to 1, write " + std::string{kRgbfCall} + "(r, g, b).");
    }
    if (*value < 0.f || *value > kChannelMax)
    {
        return std::unexpected("'" + std::string{written} + "' is outside 0 to 255.");
    }
    return *value / kChannelMax;
}

/// One channel of an `rgbf` call: a number from 0 to 1. A value past 1 is the
/// likeliest sign of a 0-to-255 value written in the wrong call.
std::expected<float, std::string> ReadUnitChannel(std::string_view written)
{
    const std::optional<float> value = ParseFloat(written);
    if (!value)
    {
        return std::unexpected("'" + std::string{written} + "' is not a number.");
    }
    if (*value < 0.f || *value > 1.f)
    {
        return std::unexpected("'" + std::string{written} + "' is outside 0 to 1. For channels from 0 to 255, " +
                               "write " + std::string{kRgbCall} + "(r, g, b).");
    }
    return *value;
}

/// The colour an `rgb` or `rgbf` call names.
ParsedColor ReadColorCall(const MarkupCall &call)
{
    const bool bytes = call.name == kRgbCall;
    if (!bytes && call.name != kRgbfCall)
    {
        return std::unexpected("'" + std::string{call.name} + "' is not a colour function. A colour is #rrggbb, " +
                               "#rrggbbaa, " + std::string{kRgbCall} + "(r, g, b[, a]) with whole numbers from 0 " +
                               "to 255, or " + std::string{kRgbfCall} + "(r, g, b[, a]) with numbers from 0 to 1.");
    }
    if (call.arguments.size() != kColorRgbChannels && call.arguments.size() != kColorRgbaChannels)
    {
        return std::unexpected(std::string{call.name} + " takes 3 channels, or 4 with alpha, and was given " +
                               std::to_string(call.arguments.size()) + ".");
    }

    // Opaque unless a fourth channel says otherwise.
    Color color{0.f, 0.f, 0.f, 1.f};
    for (std::size_t channel = 0; channel < call.arguments.size(); ++channel)
    {
        const std::expected<float, std::string> value =
            bytes ? ReadByteChannel(call.arguments[channel]) : ReadUnitChannel(call.arguments[channel]);
        if (!value)
        {
            return std::unexpected(value.error());
        }
        color[static_cast<glm::length_t>(channel)] = *value;
    }
    return color;
}

} // namespace

ParsedColor ParseColor(std::string_view text)
{
    if (text.starts_with(kHexMark))
    {
        const std::optional<Color> hex = ParseHexColor(text);
        if (!hex)
        {
            return std::unexpected("'" + std::string{text} + "' is not a colour: hex is # and then 6 or 8 hex " +
                                   "digits, #rrggbb or #rrggbbaa.");
        }
        return *hex;
    }

    const std::expected<std::optional<MarkupCall>, std::string> call = SplitCall(text);
    if (!call)
    {
        return std::unexpected(call.error());
    }
    if (!call->has_value())
    {
        // Bare numbers say no scale: `1 1 1` is white on one and nearly black
        // on the other, so they are refused rather than guessed at.
        return std::unexpected("'" + std::string{text} + "' is not a colour. Write it as hex (#rrggbb), " +
                               std::string{kRgbCall} + "(r, g, b) with whole numbers from 0 to 255, or " +
                               std::string{kRgbfCall} + "(r, g, b) with numbers from 0 to 1; each takes an " +
                               "optional fourth channel for alpha.");
    }
    return ReadColorCall(**call);
}

std::optional<Sizing> ParseSizing(std::string_view text)
{
    const std::vector<std::string_view> words = SplitWords(text);
    if (words.empty())
    {
        return std::nullopt;
    }

    Sizing sizing;
    std::size_t consumed = 1;
    if (words[0] == "fit")
    {
        sizing = Sizing::Fit();
    }
    else if (words[0] == "grow")
    {
        sizing = Sizing::Grow();
    }
    else if (words[0] == "fixed" || words[0] == "percent")
    {
        if (words.size() < 2)
        {
            return std::nullopt;
        }
        const std::optional<float> amount = ParseFloat(words[1]);
        if (!amount)
        {
            return std::nullopt;
        }
        sizing = words[0] == "fixed" ? Sizing::Fixed(*amount) : Sizing::Percent(*amount);
        consumed = 2;
    }
    else
    {
        return std::nullopt;
    }

    if (!ApplyBounds(std::span{words}.subspan(consumed), sizing))
    {
        return std::nullopt;
    }
    return sizing;
}

std::optional<Padding> ParsePadding(std::string_view text)
{
    const std::vector<std::string_view> words = SplitWords(text);
    if (words.size() != kPaddingAllEdges && words.size() != kPaddingPerEdge)
    {
        return std::nullopt;
    }

    std::array<float, kPaddingPerEdge> edges{};
    for (std::size_t edge = 0; edge < words.size(); ++edge)
    {
        const std::optional<float> value = ParseFloat(words[edge]);
        if (!value)
        {
            return std::nullopt;
        }
        edges[edge] = *value;
    }

    if (words.size() == kPaddingAllEdges)
    {
        return Padding::All(edges[0]);
    }
    return Padding{.left = edges[0], .top = edges[1], .right = edges[2], .bottom = edges[3]};
}

std::optional<std::array<Alignment, kAxisCount>> ParseAlignPair(std::string_view text)
{
    const std::vector<std::string_view> words = SplitWords(text);
    if (words.size() != kAxisCount)
    {
        return std::nullopt;
    }

    std::array<Alignment, kAxisCount> pair{Alignment::Start, Alignment::Start};
    for (std::size_t axis = 0; axis < kAxisCount; ++axis)
    {
        const std::optional<Alignment> alignment = ParseAlignment(words[axis]);
        if (!alignment)
        {
            return std::nullopt;
        }
        pair[axis] = *alignment;
    }
    return pair;
}

std::optional<std::array<bool, kAxisCount>> ParseAxes(std::string_view text)
{
    if (text == "none")
    {
        return std::array<bool, kAxisCount>{false, false};
    }
    if (text == "x")
    {
        return std::array<bool, kAxisCount>{true, false};
    }
    if (text == "y")
    {
        return std::array<bool, kAxisCount>{false, true};
    }
    if (text == "xy")
    {
        return std::array<bool, kAxisCount>{true, true};
    }
    return std::nullopt;
}

std::optional<int32_t> ParseSortKey(std::string_view text)
{
    if (const std::optional<int32_t> layer = LookUpEnum(text, kSortLayers))
    {
        return layer;
    }
    return ParseInt(text);
}

std::optional<LinesValue> ParseLines(std::string_view text)
{
    const std::vector<std::string_view> words = SplitWords(text);
    if (words.empty())
    {
        return std::nullopt;
    }

    LinesValue value;
    if (words[0] == "single")
    {
        value.kind = TextLines::Single;
    }
    else if (words[0] == "multi")
    {
        value.kind = TextLines::Multi;
    }
    else
    {
        return std::nullopt;
    }

    if (words.size() == 1)
    {
        return value;
    }
    // A height is counted in lines, which a field holding one does not have.
    if (value.kind == TextLines::Single || words.size() != kLinesWithHeight)
    {
        return std::nullopt;
    }

    if (words[1] == kUpToWord)
    {
        value.height = TextHeight::UpTo;
    }
    else if (words[1] == kExactlyWord)
    {
        value.height = TextHeight::Exactly;
    }
    else
    {
        return std::nullopt;
    }

    const std::optional<uint32_t> lines = ParseUInt(words[2]);
    if (!lines || *lines == 0)
    {
        return std::nullopt;
    }
    value.lines = *lines;
    return value;
}

std::optional<TextMask> ParseTextMask(std::string_view text)
{
    return LookUpEnum(text, kTextMasks);
}

std::optional<TextCheck> ParseTextCheck(std::string_view text)
{
    return LookUpEnum(text, kTextChecks);
}

std::optional<std::string_view> LookUpPattern(std::string_view name)
{
    return LookUpEnum(name, kPatterns);
}

std::string KnownPatterns()
{
    std::string names;
    for (const NamedEnum<std::string_view> &entry : kPatterns)
    {
        names += names.empty() ? "" : ", ";
        names += entry.name;
    }
    return names;
}

std::optional<Alignment> ParseAlignment(std::string_view text)
{
    return LookUpEnum(text, kAlignments);
}

std::optional<Direction> ParseDirection(std::string_view text)
{
    return LookUpEnum(text, kDirections);
}

std::optional<CornerStyle> ParseCornerStyle(std::string_view text)
{
    return LookUpEnum(text, kCornerStyles);
}

std::optional<TextAlign> ParseTextAlign(std::string_view text)
{
    return LookUpEnum(text, kTextAligns);
}

std::optional<ScrollBarVisibility> ParseScrollBarVisibility(std::string_view text)
{
    return LookUpEnum(text, kScrollBarVisibilities);
}

std::optional<ScrollBarDrag> ParseScrollBarDrag(std::string_view text)
{
    return LookUpEnum(text, kScrollBarDrags);
}

std::optional<FloatAnchor> ParseFloatAnchor(std::string_view text)
{
    return LookUpEnum(text, kFloatAnchors);
}

} // namespace Assisi::Mondrian::Import
