/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/AnimatorGraph.hpp>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Sigil/Verify.hpp>

#include <algorithm>
#include <utility>

namespace Assisi::Runtime
{

namespace
{

/// Raise when the payload's layout changes; an older payload is then refused
/// rather than misread.
constexpr uint32_t kAnimatorPayloadVersion = 1;

constexpr std::size_t kBitsPerByte = 8;

/// The fewest bytes each record a count covers can take, so a corrupt count
/// can't ask for more than the payload could hold.
constexpr std::size_t kMinWordBytes = 1;
constexpr std::size_t kMinStringBytes = 1;
constexpr std::size_t kMinNodeBytes = 5;
constexpr std::size_t kMinTransitionBytes = 4;
constexpr std::size_t kMinStateBytes = 36;
constexpr std::size_t kMinTransitionExtraBytes = 4;
constexpr std::size_t kMinSlotBytes = 3;
constexpr std::size_t kMinLayerBytes = 14;

/// Longest name a payload may carry: a state, a slot, a joint or a param.
constexpr std::size_t kMaxNameBytes = 256;

constexpr std::string_view kCorrupt = "the animator's cooked file is damaged";
constexpr std::string_view kOutdated = "the animator was cooked by another version of the engine";
constexpr std::string_view kLibrary = "the file is a Sigil library, which plays nothing";
constexpr std::string_view kUnsafe = "the animator's cooked code or tables would not run safely";

Core::AssetError Corrupt(std::string_view detail = kCorrupt)
{
    return Core::AssetError{Core::AssetErrorCode::CorruptAsset, detail};
}

// ── Writing ──────────────────────────────────────────────────────────────────

void WriteIndices(Core::BitWriter &writer, std::span<const uint32_t> values)
{
    writer.WriteVarUInt32(static_cast<uint32_t>(values.size()));
    for (const uint32_t value : values)
    {
        writer.WriteVarUInt32(value);
    }
}

void WriteGraph(Core::BitWriter &writer, const Sigil::Graph &graph)
{
    writer.WriteVarUInt32(static_cast<uint32_t>(graph.nodes.size()));
    for (const Sigil::Node &node : graph.nodes)
    {
        writer.WriteString(node.name);
        writer.WriteVarUInt32(node.firstChild);
        writer.WriteVarUInt32(node.childCount);
        writer.WriteVarUInt32(node.firstTransition);
        writer.WriteVarUInt32(node.transitionCount);
    }
    writer.WriteVarUInt32(static_cast<uint32_t>(graph.transitions.size()));
    for (const Sigil::GraphTransition &transition : graph.transitions)
    {
        WriteIndices(writer, transition.sources);
        WriteIndices(writer, transition.triggersRead);
        writer.WriteVarUInt32(transition.condition);
        writer.WriteVarUInt32(transition.target);
    }
}

void WriteLayer(Core::BitWriter &writer, const AnimatorLayer &layer)
{
    writer.WriteString(layer.name);
    WriteGraph(writer, layer.graph);
    for (const AnimatorState &state : layer.states)
    {
        Core::WriteAssetId(writer, state.clip.asset);
        writer.WriteUInt32(state.clip.param);
        writer.WriteUInt32(state.rate);
        writer.WriteUInt32(state.x);
        writer.WriteUInt32(state.y);
        writer.WriteUInt32(state.then);
        writer.WriteBool(state.pose);
    }
    for (const AnimatorTransition &transition : layer.transitions)
    {
        writer.WriteUInt32(transition.fade);
        writer.WriteBool(transition.interrupt);
    }
    writer.WriteVarUInt32(static_cast<uint32_t>(layer.exclusions.size()));
    for (const Core::InternedString &exclusion : layer.exclusions)
    {
        writer.WriteString(exclusion.View());
    }
    writer.WriteString(layer.maskRoot.View());
    writer.WriteUInt32(layer.weight);
    writer.WriteUInt32(layer.fade);
    writer.WriteUInt8(static_cast<uint8_t>(layer.mode));
}

void WriteLayout(Core::BitWriter &writer, const Sigil::Layout &layout)
{
    writer.WriteVarUInt32(layout.paramCount);
    writer.WriteVarUInt32(layout.impliedCount);
    writer.WriteVarUInt32(layout.letCount);
    writer.WriteVarUInt32(static_cast<uint32_t>(layout.slots.size()));
    for (const Sigil::Slot &slot : layout.slots)
    {
        writer.WriteString(slot.name);
        writer.WriteUInt8(static_cast<uint8_t>(slot.type));
        writer.WriteUInt8(static_cast<uint8_t>(slot.kind));
    }
}

// ── Reading ──────────────────────────────────────────────────────────────────

/// Reads a count, refusing one larger than the bytes left could hold at
/// @p recordBytes each.
bool ReadCount(Core::BitReader &reader, std::size_t recordBytes, uint32_t &count)
{
    count = reader.ReadVarUInt32();
    if (reader.Failed())
    {
        return false;
    }
    const std::size_t bytesLeft = reader.BitsRemaining() / kBitsPerByte;
    return static_cast<std::size_t>(count) <= bytesLeft / recordBytes;
}

bool ReadIndices(Core::BitReader &reader, std::vector<uint32_t> &values)
{
    uint32_t count = 0;
    if (!ReadCount(reader, kMinWordBytes, count))
    {
        return false;
    }
    values.resize(count);
    for (uint32_t &value : values)
    {
        value = reader.ReadVarUInt32();
    }
    return !reader.Failed();
}

bool ReadGraph(Core::BitReader &reader, Sigil::Graph &graph)
{
    uint32_t nodes = 0;
    if (!ReadCount(reader, kMinNodeBytes, nodes))
    {
        return false;
    }
    graph.nodes.resize(nodes);
    for (Sigil::Node &node : graph.nodes)
    {
        node.name = reader.ReadString(kMaxNameBytes);
        node.firstChild = reader.ReadVarUInt32();
        node.childCount = reader.ReadVarUInt32();
        node.firstTransition = reader.ReadVarUInt32();
        node.transitionCount = reader.ReadVarUInt32();
    }
    uint32_t transitions = 0;
    if (reader.Failed() || !ReadCount(reader, kMinTransitionBytes, transitions))
    {
        return false;
    }
    graph.transitions.resize(transitions);
    for (Sigil::GraphTransition &transition : graph.transitions)
    {
        if (!ReadIndices(reader, transition.sources) || !ReadIndices(reader, transition.triggersRead))
        {
            return false;
        }
        transition.condition = reader.ReadVarUInt32();
        transition.target = reader.ReadVarUInt32();
    }
    return !reader.Failed();
}

bool ReadStrings(Core::BitReader &reader, std::vector<Core::InternedString> &values)
{
    uint32_t count = 0;
    if (!ReadCount(reader, kMinStringBytes, count))
    {
        return false;
    }
    values.clear();
    for (uint32_t i = 0; i < count; ++i)
    {
        values.emplace_back(reader.ReadString(kMaxNameBytes));
    }
    return !reader.Failed();
}

bool ReadLayer(Core::BitReader &reader, AnimatorLayer &layer)
{
    layer.name = reader.ReadString(kMaxNameBytes);
    if (reader.Failed() || !ReadGraph(reader, layer.graph))
    {
        return false;
    }
    const std::size_t bytesLeft = reader.BitsRemaining() / kBitsPerByte;
    if (layer.graph.nodes.size() > bytesLeft / kMinStateBytes ||
        layer.graph.transitions.size() > bytesLeft / kMinTransitionExtraBytes)
    {
        return false;
    }
    layer.states.resize(layer.graph.nodes.size());
    for (AnimatorState &state : layer.states)
    {
        state.clip.asset = Core::ReadAssetId(reader);
        state.clip.param = reader.ReadUInt32();
        state.rate = reader.ReadUInt32();
        state.x = reader.ReadUInt32();
        state.y = reader.ReadUInt32();
        state.then = reader.ReadUInt32();
        state.pose = reader.ReadBool();
    }
    layer.transitions.resize(layer.graph.transitions.size());
    for (AnimatorTransition &transition : layer.transitions)
    {
        transition.fade = reader.ReadUInt32();
        transition.interrupt = reader.ReadBool();
    }
    if (reader.Failed() || !ReadStrings(reader, layer.exclusions))
    {
        return false;
    }
    layer.maskRoot = Core::InternedString{reader.ReadString(kMaxNameBytes)};
    layer.weight = reader.ReadUInt32();
    layer.fade = reader.ReadUInt32();
    const uint8_t mode = reader.ReadUInt8();
    if (reader.Failed() || mode >= static_cast<uint8_t>(LayerMode::Count_))
    {
        return false;
    }
    layer.mode = static_cast<LayerMode>(mode);
    return true;
}

bool ReadLayout(Core::BitReader &reader, Sigil::Layout &layout)
{
    layout.paramCount = reader.ReadVarUInt32();
    layout.impliedCount = reader.ReadVarUInt32();
    layout.letCount = reader.ReadVarUInt32();
    uint32_t slots = 0;
    if (reader.Failed() || !ReadCount(reader, kMinSlotBytes, slots))
    {
        return false;
    }
    layout.slots.resize(slots);
    for (Sigil::Slot &slot : layout.slots)
    {
        slot.name = reader.ReadString(kMaxNameBytes);
        const uint8_t type = reader.ReadUInt8();
        const uint8_t kind = reader.ReadUInt8();
        if (reader.Failed() || type >= static_cast<uint8_t>(Sigil::SlotType::Count_) ||
            kind >= static_cast<uint8_t>(Sigil::SlotKind::Count_))
        {
            return false;
        }
        slot.type = static_cast<Sigil::SlotType>(type);
        slot.kind = static_cast<Sigil::SlotKind>(kind);
    }
    return true;
}

// ── Checking ─────────────────────────────────────────────────────────────────

/// Whether @p entry is code that runs safely against the block, or not written.
bool EntrySafe(const AnimatorGraph &graph, uint32_t entry)
{
    if (entry == kNotWritten)
    {
        return true;
    }
    return Sigil::Verify(graph.code, entry, static_cast<uint32_t>(graph.layout.slots.size())).has_value();
}

bool SlotInside(const AnimatorGraph &graph, uint32_t slot)
{
    return slot < graph.layout.slots.size();
}

/// How many states the block holding each node has: its parent's count, or 0
/// for the root, which a `then` can't lead out of.
std::vector<uint32_t> SiblingCounts(const Sigil::Graph &graph)
{
    std::vector<uint32_t> counts(graph.nodes.size(), 0);
    for (const Sigil::Node &node : graph.nodes)
    {
        for (uint32_t child = node.firstChild; child < node.firstChild + node.childCount; ++child)
        {
            counts[child] = node.childCount;
        }
    }
    return counts;
}

bool StateSafe(const AnimatorGraph &graph, const AnimatorState &state, uint32_t siblings)
{
    const bool paramInside = state.clip.param == kNotWritten || state.clip.param < graph.clipParams.size();
    const bool thenInside = state.then == kNotWritten || state.then < siblings;
    return paramInside && thenInside && EntrySafe(graph, state.rate) && EntrySafe(graph, state.x) &&
           EntrySafe(graph, state.y);
}

bool LayerSafe(const AnimatorGraph &graph, const AnimatorLayer &layer)
{
    const uint32_t slotCount = static_cast<uint32_t>(graph.layout.slots.size());
    if (!Sigil::VerifyGraph(layer.graph, graph.code, slotCount).has_value() ||
        layer.states.size() != layer.graph.nodes.size() ||
        layer.transitions.size() != layer.graph.transitions.size())
    {
        return false;
    }
    const std::vector<uint32_t> siblings = SiblingCounts(layer.graph);
    for (std::size_t node = 0; node < layer.states.size(); ++node)
    {
        if (!StateSafe(graph, layer.states[node], siblings[node]))
        {
            return false;
        }
    }
    const bool transitionsSafe =
        std::ranges::all_of(layer.transitions,
                            [&graph](const AnimatorTransition &transition) { return EntrySafe(graph, transition.fade); });
    return transitionsSafe && EntrySafe(graph, layer.weight) && EntrySafe(graph, layer.fade);
}

/// The first layer is the player's base, which has no mask, mode or weight.
bool BaseSafe(const AnimatorLayer &base)
{
    return base.maskRoot.View().empty() && base.exclusions.empty() && base.mode == LayerMode::Override &&
           base.weight == kNotWritten;
}

bool GraphSafe(const AnimatorGraph &graph)
{
    const Sigil::Layout &layout = graph.layout;
    const std::size_t declared =
        static_cast<std::size_t>(layout.paramCount) + layout.impliedCount + layout.letCount;
    if (declared != layout.slots.size() || graph.letEntries.size() != layout.letCount)
    {
        return false;
    }
    const bool letsSafe = std::ranges::all_of(
        graph.letEntries, [&graph](uint32_t entry) { return entry != kNotWritten && EntrySafe(graph, entry); });
    const bool triggersInside =
        std::ranges::all_of(graph.triggerSlots, [&graph](uint32_t slot) { return SlotInside(graph, slot); });
    const bool progressInside = graph.progressSlot == kNotWritten || SlotInside(graph, graph.progressSlot);
    if (!letsSafe || !triggersInside || !progressInside || !BaseSafe(graph.layers.front()))
    {
        return false;
    }
    return std::ranges::all_of(graph.layers,
                               [&graph](const AnimatorLayer &layer) { return LayerSafe(graph, layer); });
}

} // namespace

std::vector<std::byte> WriteAnimatorGraph(const AnimatorGraph &graph)
{
    Core::BitWriter writer;
    writer.WriteVarUInt32(kAnimatorPayloadVersion);
    WriteLayout(writer, graph.layout);
    writer.WriteVarUInt32(static_cast<uint32_t>(graph.code.size()));
    for (const Sigil::Word word : graph.code)
    {
        writer.WriteUInt32(word);
    }
    WriteIndices(writer, graph.letEntries);
    WriteIndices(writer, graph.triggerSlots);
    writer.WriteVarUInt32(static_cast<uint32_t>(graph.clipParams.size()));
    for (const std::string &name : graph.clipParams)
    {
        writer.WriteString(name);
    }
    writer.WriteVarUInt32(static_cast<uint32_t>(graph.layers.size()));
    for (const AnimatorLayer &layer : graph.layers)
    {
        WriteLayer(writer, layer);
    }
    Core::WriteAssetId(writer, graph.skeleton);
    writer.WriteUInt32(graph.progressSlot);
    const std::span<const std::byte> bytes = writer.Data();
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

std::expected<AnimatorGraph, Core::AssetError> ReadAnimatorGraph(std::span<const std::byte> payload)
{
    Core::BitReader reader{payload};
    if (reader.ReadVarUInt32() != kAnimatorPayloadVersion || reader.Failed())
    {
        return std::unexpected(Corrupt(kOutdated));
    }
    AnimatorGraph graph;
    uint32_t words = 0;
    if (!ReadLayout(reader, graph.layout) || !ReadCount(reader, sizeof(Sigil::Word), words))
    {
        return std::unexpected(Corrupt());
    }
    graph.code.resize(words);
    for (Sigil::Word &word : graph.code)
    {
        word = reader.ReadUInt32();
    }
    uint32_t clipParams = 0;
    if (!ReadIndices(reader, graph.letEntries) || !ReadIndices(reader, graph.triggerSlots) ||
        !ReadCount(reader, kMinStringBytes, clipParams))
    {
        return std::unexpected(Corrupt());
    }
    for (uint32_t i = 0; i < clipParams; ++i)
    {
        graph.clipParams.push_back(reader.ReadString(kMaxNameBytes));
    }
    uint32_t layers = 0;
    if (reader.Failed() || !ReadCount(reader, kMinLayerBytes, layers))
    {
        return std::unexpected(Corrupt());
    }
    graph.layers.resize(layers);
    for (AnimatorLayer &layer : graph.layers)
    {
        if (!ReadLayer(reader, layer))
        {
            return std::unexpected(Corrupt());
        }
    }
    graph.skeleton = Core::ReadAssetId(reader);
    graph.progressSlot = reader.ReadUInt32();
    if (reader.Failed())
    {
        return std::unexpected(Corrupt());
    }
    if (graph.layers.empty())
    {
        return std::unexpected(Corrupt(kLibrary));
    }
    if (!GraphSafe(graph))
    {
        return std::unexpected(Corrupt(kUnsafe));
    }
    return graph;
}

} // namespace Assisi::Runtime
