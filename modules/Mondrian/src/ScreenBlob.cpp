/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Mondrian/ScreenBlob.hpp>

#include <Assisi/Core/CookedBlob.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <optional>
#include <string>
#include <type_traits>

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

using Color = Math::Color4<Math::ColorSpace::Srgb>;

/// Whether @p Field is @p T, const or not: each field list below serves the
/// writer, which holds a const document, and the reader, which fills one.
template <typename Field, typename T>
concept FieldOf = std::same_as<std::remove_const_t<Field>, T>;

// Each struct the payload carries, as the fields it carries, in order. The
// writer and the reader both walk these lists, so a field is never written
// without being read, or read from a different place than it was written.

template <typename Archive, FieldOf<Length> L> void Fields(Archive &archive, L &length)
{
    archive(length.value, length.unit);
}

template <typename Archive, FieldOf<Sizing> S> void Fields(Archive &archive, S &sizing)
{
    archive(sizing.value, sizing.min, sizing.max, sizing.kind);
}

template <typename Archive, FieldOf<Padding> P> void Fields(Archive &archive, P &padding)
{
    archive(padding.left, padding.top, padding.right, padding.bottom);
}

template <typename Archive, FieldOf<Floating> F> void Fields(Archive &archive, F &floating)
{
    archive(floating.offset, floating.anchor, floating.attach, floating.target, floating.enabled,
            floating.clipToParent);
}

template <typename Archive, FieldOf<Style> S> void Fields(Archive &archive, S &style)
{
    // Box
    archive(style.sizing, style.padding, style.floating, style.direction, style.childAlign, style.gap);
    // Paint
    archive(style.background, style.borderColor, style.borderWidth, style.cornerRadius, style.cornerStyle);
    // Text
    archive(style.textColor, style.textSize, style.textAlign);
    // Scrolling
    archive(style.enabledScrollBars, style.scrollSmoothing, style.scrollBarMinLength, style.scrollBarVisibility,
            style.scrollBarDrag);
}

template <typename Archive, FieldOf<SliderRange> R> void Fields(Archive &archive, R &range)
{
    archive(range.min, range.max, range.step);
}

template <typename Archive, FieldOf<ScreenTraits> T> void Fields(Archive &archive, T &traits)
{
    archive(traits.input, traits.beneath, traits.pause);
}

template <typename Archive, FieldOf<ScreenNode> N> void Fields(Archive &archive, N &node)
{
    // Place in the tree
    archive(node.parent, node.name, node.style, node.styleName);
    // State
    archive(node.visible, node.enabled, node.blocksPointer, node.takesKeyboard, node.selectable);
    // Control and what it does
    archive(node.widget, node.text, node.action, node.verb, node.target, node.moves, node.eventName);
    // Sliders and toggles
    archive(node.range, node.value, node.steps, node.step, node.on);
    // Text fields
    archive(node.placeholder, node.pattern, node.maxLength, node.lineLimit, node.lines, node.mask, node.check,
            node.height);
}

/// Writes each field it is handed, in the order handed.
class PayloadWriter
{
  public:
    explicit PayloadWriter(Core::BitWriter &bits) : _bits(bits) {}

    template <typename... Field> void operator()(const Field &...fields) { (Write(fields), ...); }

  private:
    void Write(bool value) { _bits.WriteBool(value); }
    void Write(float value) { _bits.WriteFloat(value); }
    void Write(int32_t value) { _bits.WriteInt32(value); }
    void Write(uint32_t value) { _bits.WriteUInt32(value); }
    void Write(const std::string &value) { _bits.WriteString(value); }

    void Write(const Color &value)
    {
        _bits.WriteFloat(value.r);
        _bits.WriteFloat(value.g);
        _bits.WriteFloat(value.b);
        _bits.WriteFloat(value.a);
    }

    template <typename E>
        requires std::is_enum_v<E>
    void Write(E value)
    {
        _bits.WriteVarUInt32(static_cast<uint32_t>(value));
    }

    template <typename T, std::size_t N> void Write(const std::array<T, N> &values)
    {
        for (const T &value : values)
        {
            Write(value);
        }
    }

    template <typename T>
        requires requires(PayloadWriter &writer, const T &value) { Fields(writer, value); }
    void Write(const T &value)
    {
        Fields(*this, value);
    }

    Core::BitWriter &_bits;
};

/// Reads each field it is handed, in the order handed. An enumerator this
/// build does not have is left at its default and refuses the payload, so
/// nothing downstream switches over a value no case covers.
class PayloadReader
{
  public:
    explicit PayloadReader(Core::BitReader &bits) : _bits(bits) {}

    template <typename... Field> void operator()(Field &...fields) { (Read(fields), ...); }

    /// Whether an enumerator was out of range. Framing faults are the bit
    /// reader's own Failed().
    [[nodiscard]] bool Refused() const { return _refused; }

  private:
    void Read(bool &value) { value = _bits.ReadBool(); }
    void Read(float &value) { value = _bits.ReadFloat(); }
    void Read(int32_t &value) { value = _bits.ReadInt32(); }
    void Read(uint32_t &value) { value = _bits.ReadUInt32(); }
    void Read(std::string &value) { value = _bits.ReadString(); }

    void Read(Color &value)
    {
        value.r = _bits.ReadFloat();
        value.g = _bits.ReadFloat();
        value.b = _bits.ReadFloat();
        value.a = _bits.ReadFloat();
    }

    template <typename E>
        requires std::is_enum_v<E>
    void Read(E &value)
    {
        const uint32_t raw = _bits.ReadVarUInt32();
        if (raw >= static_cast<uint32_t>(E::Count))
        {
            _refused = true;
            return;
        }
        value = static_cast<E>(raw);
    }

    template <typename T, std::size_t N> void Read(std::array<T, N> &values)
    {
        for (T &value : values)
        {
            Read(value);
        }
    }

    template <typename T>
        requires requires(PayloadReader &reader, T &value) { Fields(reader, value); }
    void Read(T &value)
    {
        Fields(*this, value);
    }

    Core::BitReader &_bits;
    bool _refused = false;
};

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

/// Whether the table describes a tree this build can walk in one pass.
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

        // An event action with no name is a button that would resolve to
        // nothing and silently do nothing.
        if (node.action == ActionKind::Event && node.eventName.empty())
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

    PayloadWriter payload{writer};
    payload(document.sortKey, document.traits, document.focus);

    writer.WriteVarUInt32(static_cast<uint32_t>(document.systems.size()));
    for (const std::string &system : document.systems)
    {
        payload(system);
    }

    writer.WriteVarUInt32(static_cast<uint32_t>(document.nodes.size()));
    for (const ScreenNode &node : document.nodes)
    {
        payload(node);
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
    PayloadReader payload{reader};
    payload(document.sortKey, document.traits, document.focus);

    const std::optional<uint32_t> systemCount = ReadCount(reader, kMinSystemBytes);
    if (!systemCount)
    {
        return std::unexpected(CookedScreenError::Truncated);
    }
    document.systems.resize(*systemCount);
    for (std::string &system : document.systems)
    {
        payload(system);
    }

    const std::optional<uint32_t> nodeCount = ReadCount(reader, kMinNodeBytes);
    if (!nodeCount)
    {
        return std::unexpected(CookedScreenError::Truncated);
    }
    document.nodes.resize(*nodeCount);
    for (ScreenNode &node : document.nodes)
    {
        payload(node);
    }

    // Framing first: a document read out of bytes that ran out holds whatever
    // zeroes the reader handed back, and calling that inconsistent would name
    // the wrong fault.
    if (reader.Failed())
    {
        return std::unexpected(CookedScreenError::Truncated);
    }
    if (payload.Refused() || !IsConsistent(document))
    {
        return std::unexpected(CookedScreenError::Invalid);
    }
    return document;
}

} // namespace Assisi::Mondrian
