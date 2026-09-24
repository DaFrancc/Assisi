/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Mondrian/ScreenBlob.hpp>

#include <Assisi/Core/CookedBlob.hpp>

#include <cstddef>
#include <optional>
#include <string>

namespace Assisi::Mondrian
{
namespace
{

constexpr std::size_t kBitsPerByte = 8;

/// A floor on what one node occupies, not its true size: the fixed integer
/// fields and six empty strings. It exists only to stop a count no file could
/// hold from reaching a resize.
constexpr std::size_t kMinNodeBytes = 56;

/// The same for a system name: a length and at least one character.
constexpr std::size_t kMinSystemBytes = 2;

std::size_t BytesLeft(const Core::BitReader &reader)
{
    return reader.BitsRemaining() / kBitsPerByte;
}

/// Reads a table's entry count, refusing one the remaining bytes cannot hold.
std::optional<uint32_t> ReadCount(Core::BitReader &reader, std::size_t entryBytes)
{
    const uint32_t count = reader.ReadVarUInt32();
    if (reader.Failed() || count > BytesLeft(reader) / entryBytes)
    {
        return std::nullopt;
    }
    return count;
}

/// Whether @p value names an enumerator of an enum whose last is Count.
template <typename E> bool InRange(uint32_t value)
{
    return value < static_cast<uint32_t>(E::Count);
}

void WriteColor(Core::BitWriter &writer, const Math::Color4<Math::ColorSpace::Srgb> &color)
{
    writer.WriteFloat(color.r);
    writer.WriteFloat(color.g);
    writer.WriteFloat(color.b);
    writer.WriteFloat(color.a);
}

Math::Color4<Math::ColorSpace::Srgb> ReadColor(Core::BitReader &reader)
{
    Math::Color4<Math::ColorSpace::Srgb> color{0.f, 0.f, 0.f, 0.f};
    color.r = reader.ReadFloat();
    color.g = reader.ReadFloat();
    color.b = reader.ReadFloat();
    color.a = reader.ReadFloat();
    return color;
}

void WriteSizing(Core::BitWriter &writer, const Sizing &sizing)
{
    writer.WriteFloat(sizing.value);
    writer.WriteFloat(sizing.min);
    writer.WriteFloat(sizing.max);
    writer.WriteUInt8(static_cast<uint8_t>(sizing.kind));
}

Sizing ReadSizing(Core::BitReader &reader)
{
    Sizing sizing;
    sizing.value = reader.ReadFloat();
    sizing.min = reader.ReadFloat();
    sizing.max = reader.ReadFloat();
    sizing.kind = static_cast<SizingKind>(reader.ReadUInt8());
    return sizing;
}

void WriteSliderRange(Core::BitWriter &writer, const SliderRange &range)
{
    writer.WriteFloat(range.min);
    writer.WriteFloat(range.max);
    writer.WriteFloat(range.step);
}

SliderRange ReadSliderRange(Core::BitReader &reader)
{
    SliderRange range;
    range.min = reader.ReadFloat();
    range.max = reader.ReadFloat();
    range.step = reader.ReadFloat();
    return range;
}

void WriteFloating(Core::BitWriter &writer, const Floating &floating)
{
    writer.WriteFloat(floating.offset.x);
    writer.WriteFloat(floating.offset.y);
    for (std::size_t axis = 0; axis < kAxisCount; ++axis)
    {
        writer.WriteUInt8(static_cast<uint8_t>(floating.anchor[axis]));
        writer.WriteUInt8(static_cast<uint8_t>(floating.attach[axis]));
    }
    writer.WriteUInt8(static_cast<uint8_t>(floating.target));
    writer.WriteBool(floating.enabled);
    writer.WriteBool(floating.clipToParent);
}

Floating ReadFloating(Core::BitReader &reader)
{
    Floating floating;
    floating.offset.x = reader.ReadFloat();
    floating.offset.y = reader.ReadFloat();
    for (std::size_t axis = 0; axis < kAxisCount; ++axis)
    {
        floating.anchor[axis] = static_cast<Alignment>(reader.ReadUInt8());
        floating.attach[axis] = static_cast<Alignment>(reader.ReadUInt8());
    }
    floating.target = static_cast<FloatAnchor>(reader.ReadUInt8());
    floating.enabled = reader.ReadBool();
    floating.clipToParent = reader.ReadBool();
    return floating;
}

void WriteStyle(Core::BitWriter &writer, const Style &style)
{
    WriteColor(writer, style.background);
    WriteColor(writer, style.borderColor);
    WriteColor(writer, style.textColor);
    for (std::size_t axis = 0; axis < kAxisCount; ++axis)
    {
        WriteSizing(writer, style.sizing[axis]);
    }
    writer.WriteFloat(style.padding.left);
    writer.WriteFloat(style.padding.top);
    writer.WriteFloat(style.padding.right);
    writer.WriteFloat(style.padding.bottom);
    WriteFloating(writer, style.floating);
    writer.WriteFloat(style.gap);
    writer.WriteFloat(style.textSize);
    writer.WriteFloat(style.borderWidth);
    writer.WriteFloat(style.cornerRadius);
    writer.WriteUInt32(static_cast<uint32_t>(style.cornerStyle));
    writer.WriteUInt8(static_cast<uint8_t>(style.direction));
    for (std::size_t axis = 0; axis < kAxisCount; ++axis)
    {
        writer.WriteUInt8(static_cast<uint8_t>(style.childAlign[axis]));
    }
    writer.WriteUInt8(static_cast<uint8_t>(style.textAlign));
    writer.WriteFloat(style.scrollSmoothing);
    writer.WriteFloat(style.scrollBarMinLength);
    for (std::size_t axis = 0; axis < kAxisCount; ++axis)
    {
        writer.WriteBool(style.enabledScrollBars[axis]);
    }
    writer.WriteUInt8(static_cast<uint8_t>(style.scrollBarVisibility));
    writer.WriteUInt8(static_cast<uint8_t>(style.scrollBarDrag));
}

Style ReadStyle(Core::BitReader &reader)
{
    Style style;
    style.background = ReadColor(reader);
    style.borderColor = ReadColor(reader);
    style.textColor = ReadColor(reader);
    for (std::size_t axis = 0; axis < kAxisCount; ++axis)
    {
        style.sizing[axis] = ReadSizing(reader);
    }
    style.padding.left = reader.ReadFloat();
    style.padding.top = reader.ReadFloat();
    style.padding.right = reader.ReadFloat();
    style.padding.bottom = reader.ReadFloat();
    style.floating = ReadFloating(reader);
    style.gap = reader.ReadFloat();
    style.textSize = reader.ReadFloat();
    style.borderWidth = reader.ReadFloat();
    style.cornerRadius = reader.ReadFloat();
    style.cornerStyle = static_cast<CornerStyle>(reader.ReadUInt32());
    style.direction = static_cast<Direction>(reader.ReadUInt8());
    for (std::size_t axis = 0; axis < kAxisCount; ++axis)
    {
        style.childAlign[axis] = static_cast<Alignment>(reader.ReadUInt8());
    }
    style.textAlign = static_cast<TextAlign>(reader.ReadUInt8());
    style.scrollSmoothing = reader.ReadFloat();
    style.scrollBarMinLength = reader.ReadFloat();
    for (std::size_t axis = 0; axis < kAxisCount; ++axis)
    {
        style.enabledScrollBars[axis] = reader.ReadBool();
    }
    style.scrollBarVisibility = static_cast<ScrollBarVisibility>(reader.ReadUInt8());
    style.scrollBarDrag = static_cast<ScrollBarDrag>(reader.ReadUInt8());
    return style;
}

/// Whether every enumerator in @p style names something this build has. The
/// reader frames bytes; this is what says the frame holds a style the layout
/// can act on rather than a cast to a value no switch covers.
bool StyleIsInRange(const Style &style)
{
    for (std::size_t axis = 0; axis < kAxisCount; ++axis)
    {
        if (!InRange<SizingKind>(static_cast<uint32_t>(style.sizing[axis].kind)) ||
            !InRange<Alignment>(static_cast<uint32_t>(style.childAlign[axis])) ||
            !InRange<Alignment>(static_cast<uint32_t>(style.floating.anchor[axis])) ||
            !InRange<Alignment>(static_cast<uint32_t>(style.floating.attach[axis])))
        {
            return false;
        }
    }
    return InRange<FloatAnchor>(static_cast<uint32_t>(style.floating.target)) &&
           InRange<CornerStyle>(static_cast<uint32_t>(style.cornerStyle)) &&
           InRange<Direction>(static_cast<uint32_t>(style.direction)) &&
           InRange<TextAlign>(static_cast<uint32_t>(style.textAlign)) &&
           InRange<ScrollBarVisibility>(static_cast<uint32_t>(style.scrollBarVisibility)) &&
           InRange<ScrollBarDrag>(static_cast<uint32_t>(style.scrollBarDrag));
}

/// Whether the table describes a tree this build can walk in one pass, and
/// whether every node's own fields name something.
bool IsConsistent(const ScreenDocument &document)
{
    if (document.nodes.empty())
    {
        return false; // every screen has a root
    }
    if (document.focus != kNoNode && document.focus >= document.nodes.size())
    {
        return false;
    }
    if (!InRange<ScreenInput>(static_cast<uint32_t>(document.traits.input)) ||
        !InRange<ScreenBeneath>(static_cast<uint32_t>(document.traits.beneath)) ||
        !InRange<ScreenPause>(static_cast<uint32_t>(document.traits.pause)))
    {
        return false;
    }

    for (std::size_t index = 0; index < document.nodes.size(); ++index)
    {
        const ScreenNode &node = document.nodes[index];

        // Preorder with the parent first is what lets the loader build the tree
        // in one pass. A parent at or past its own child is also how a cycle
        // would arrive, so this is the check that makes the walk terminate.
        const bool rootParent = index == 0 && node.parent == kNoNode;
        const bool childParent = index > 0 && node.parent < index;
        if (!rootParent && !childParent)
        {
            return false;
        }

        // The loader indexes its table of built ids by this, so one past the
        // end is a read out of bounds. Whether the node there can be acted on
        // is the loader's question, asked with the verb in hand.
        if (node.target != kNoNode && node.target >= document.nodes.size())
        {
            return false;
        }

        if (!InRange<BuiltinWidget>(static_cast<uint32_t>(node.widget)) ||
            !InRange<ActionKind>(static_cast<uint32_t>(node.action)) ||
            !InRange<ScreenVerb>(static_cast<uint32_t>(node.verb)))
        {
            return false;
        }

        // The text field switches over these as layout switches over a style's,
        // so one out of range is a case no switch covers rather than a field
        // that merely looks wrong.
        if (!InRange<TextLines>(static_cast<uint32_t>(node.lines)) ||
            !InRange<TextMask>(static_cast<uint32_t>(node.mask)) ||
            !InRange<TextCheck>(static_cast<uint32_t>(node.check)) ||
            !InRange<TextHeight>(static_cast<uint32_t>(node.height)))
        {
            return false;
        }

        // An event action with no name is a button that would resolve to
        // nothing and silently do nothing.
        if (node.action == ActionKind::Event && node.eventName.empty())
        {
            return false;
        }

        if (!StyleIsInRange(node.style))
        {
            return false;
        }
    }
    return true;
}

} // namespace

std::string_view ToString(CookedScreenError error) noexcept
{
    switch (error)
    {
    case CookedScreenError::NotAScreen:
        return "not a cooked screen";
    case CookedScreenError::Truncated:
        return "ended part-way through";
    case CookedScreenError::UnsupportedVersion:
        return "a screen layout this build does not read";
    case CookedScreenError::Invalid:
        return "describes no usable screen";
    case CookedScreenError::Count:
        break;
    }
    return "unknown";
}

void WriteCookedScreen(Core::BitWriter &writer, const ScreenDocument &document)
{
    Core::WriteCookedHeader(writer, Core::CookedKind::Screen);
    writer.WriteUInt8(kScreenPayloadVersion);

    writer.WriteString(document.name);
    writer.WriteInt32(document.sortKey);
    writer.WriteUInt8(static_cast<uint8_t>(document.traits.input));
    writer.WriteUInt8(static_cast<uint8_t>(document.traits.beneath));
    writer.WriteUInt8(static_cast<uint8_t>(document.traits.pause));
    writer.WriteUInt32(document.focus);

    writer.WriteVarUInt32(static_cast<uint32_t>(document.systems.size()));
    for (const std::string &system : document.systems)
    {
        writer.WriteString(system);
    }

    writer.WriteVarUInt32(static_cast<uint32_t>(document.nodes.size()));
    for (const ScreenNode &node : document.nodes)
    {
        writer.WriteUInt32(node.parent);
        writer.WriteUInt32(node.target);
        writer.WriteUInt32(static_cast<uint32_t>(node.widget));
        writer.WriteUInt8(static_cast<uint8_t>(node.action));
        writer.WriteUInt8(static_cast<uint8_t>(node.verb));
        writer.WriteBool(node.visible);
        writer.WriteBool(node.enabled);
        writer.WriteBool(node.blocksPointer);
        writer.WriteBool(node.takesKeyboard);
        writer.WriteBool(node.selectable);
        writer.WriteString(node.name);
        writer.WriteString(node.text);
        writer.WriteString(node.styleName);
        writer.WriteString(node.eventName);
        writer.WriteString(node.placeholder);
        writer.WriteString(node.pattern);
        WriteSliderRange(writer, node.range);
        writer.WriteFloat(node.value);
        writer.WriteUInt32(node.maxLength);
        writer.WriteUInt32(node.lineLimit);
        writer.WriteInt32(node.steps);
        writer.WriteInt32(node.step);
        writer.WriteInt32(node.moves);
        writer.WriteUInt8(static_cast<uint8_t>(node.lines));
        writer.WriteUInt8(static_cast<uint8_t>(node.mask));
        writer.WriteUInt8(static_cast<uint8_t>(node.check));
        writer.WriteUInt8(static_cast<uint8_t>(node.height));
        writer.WriteBool(node.on);
        WriteStyle(writer, node.style);
    }
}

std::expected<ScreenDocument, CookedScreenError> ReadCookedScreen(std::span<const std::byte> bytes)
{
    Core::BitReader reader{bytes};

    const std::expected<Core::CookedKind, Core::CookedBlobError> blobKind = Core::ReadCookedHeader(reader);
    if (!blobKind || *blobKind != Core::CookedKind::Screen)
    {
        if (!blobKind && blobKind.error() == Core::CookedBlobError::Truncated)
        {
            return std::unexpected(CookedScreenError::Truncated);
        }
        return std::unexpected(CookedScreenError::NotAScreen);
    }

    const uint8_t version = reader.ReadUInt8();
    if (reader.Failed())
    {
        return std::unexpected(CookedScreenError::Truncated);
    }
    if (version != kScreenPayloadVersion)
    {
        return std::unexpected(CookedScreenError::UnsupportedVersion);
    }

    ScreenDocument document;
    document.name = reader.ReadString();
    document.sortKey = reader.ReadInt32();
    document.traits.input = static_cast<ScreenInput>(reader.ReadUInt8());
    document.traits.beneath = static_cast<ScreenBeneath>(reader.ReadUInt8());
    document.traits.pause = static_cast<ScreenPause>(reader.ReadUInt8());
    document.focus = reader.ReadUInt32();

    const std::optional<uint32_t> systemCount = ReadCount(reader, kMinSystemBytes);
    if (!systemCount)
    {
        return std::unexpected(CookedScreenError::Truncated);
    }
    document.systems.reserve(*systemCount);
    for (uint32_t index = 0; index < *systemCount; ++index)
    {
        document.systems.push_back(reader.ReadString());
    }

    const std::optional<uint32_t> nodeCount = ReadCount(reader, kMinNodeBytes);
    if (!nodeCount)
    {
        return std::unexpected(CookedScreenError::Truncated);
    }
    document.nodes.resize(*nodeCount);
    for (ScreenNode &node : document.nodes)
    {
        node.parent = reader.ReadUInt32();
        node.target = reader.ReadUInt32();
        node.widget = static_cast<BuiltinWidget>(reader.ReadUInt32());
        node.action = static_cast<ActionKind>(reader.ReadUInt8());
        node.verb = static_cast<ScreenVerb>(reader.ReadUInt8());
        node.visible = reader.ReadBool();
        node.enabled = reader.ReadBool();
        node.blocksPointer = reader.ReadBool();
        node.takesKeyboard = reader.ReadBool();
        node.selectable = reader.ReadBool();
        node.name = reader.ReadString();
        node.text = reader.ReadString();
        node.styleName = reader.ReadString();
        node.eventName = reader.ReadString();
        node.placeholder = reader.ReadString();
        node.pattern = reader.ReadString();
        node.range = ReadSliderRange(reader);
        node.value = reader.ReadFloat();
        node.maxLength = reader.ReadUInt32();
        node.lineLimit = reader.ReadUInt32();
        node.steps = reader.ReadInt32();
        node.step = reader.ReadInt32();
        node.moves = reader.ReadInt32();
        node.lines = static_cast<TextLines>(reader.ReadUInt8());
        node.mask = static_cast<TextMask>(reader.ReadUInt8());
        node.check = static_cast<TextCheck>(reader.ReadUInt8());
        node.height = static_cast<TextHeight>(reader.ReadUInt8());
        node.on = reader.ReadBool();
        node.style = ReadStyle(reader);
    }

    // Framing first: a document read out of bytes that ran out holds whatever
    // zeroes the reader handed back, and calling that inconsistent would name
    // the wrong fault.
    if (reader.Failed())
    {
        return std::unexpected(CookedScreenError::Truncated);
    }
    if (!IsConsistent(document))
    {
        return std::unexpected(CookedScreenError::Invalid);
    }
    return document;
}

} // namespace Assisi::Mondrian
