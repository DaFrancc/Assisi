/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "MarkupValues.hpp"

#include <Assisi/Mondrian/Screen.hpp>

#include <charconv>
#include <span>

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
constexpr std::size_t kColorRgbWords = 3;
constexpr std::size_t kColorRgbaWords = 4;

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

std::optional<Math::Color4<Math::ColorSpace::Srgb>> ParseHexColor(std::string_view text)
{
    const std::string_view digits = text.substr(1);
    if (digits.size() != kHexRgbDigits && digits.size() != kHexRgbaDigits)
    {
        return std::nullopt;
    }

    Math::Color4<Math::ColorSpace::Srgb> color{0.f, 0.f, 0.f, 1.f};
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

std::optional<Math::Color4<Math::ColorSpace::Srgb>> ParseColor(std::string_view text)
{
    if (text.starts_with('#'))
    {
        return ParseHexColor(text);
    }

    const std::vector<std::string_view> words = SplitWords(text);
    if (words.size() != kColorRgbWords && words.size() != kColorRgbaWords)
    {
        return std::nullopt;
    }

    Math::Color4<Math::ColorSpace::Srgb> color{0.f, 0.f, 0.f, 1.f};
    for (std::size_t channel = 0; channel < words.size(); ++channel)
    {
        const std::optional<float> value = ParseFloat(words[channel]);
        if (!value)
        {
            return std::nullopt;
        }
        color[static_cast<glm::length_t>(channel)] = *value;
    }
    return color;
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
