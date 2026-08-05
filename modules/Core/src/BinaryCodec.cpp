/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file BinaryCodec.cpp
/// @brief Reflection-driven component codec. See Reflect/BinaryCodec.hpp for the
///        block format, the trust boundary, and why Core encodes glm and ECS
///        types by raw layout instead of linking either.

#include <Assisi/Core/Reflect/BinaryCodec.hpp>

#include <bit>
#include <cstring>
#include <string>
#include <vector>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/Core/ContentHash.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ComponentMask.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/Core/Reflect/MessageRegistry.hpp>
#include <Assisi/Core/ShortString.hpp>

namespace Assisi::Core::Reflect
{
namespace
{

/// Float counts of the glm-typed fields. Core does not link glm (see the header),
/// so these are the layout facts this file owns: a vecN is N contiguous floats
/// and a mat4 is 16, in the same order glm stores them. Both ends of the wire run
/// the same build, so "same order glm stores them" is a complete specification —
/// notably it sidesteps the glm quaternion component-order build flags
/// (GLM_FORCE_QUAT_DATA_WXYZ / _XYZW) entirely, because the codec never
/// interprets the components, it only moves them.
constexpr std::uint32_t kVec2Floats = 2;
constexpr std::uint32_t kVec3Floats = 3;
constexpr std::uint32_t kVec4Floats = 4;
constexpr std::uint32_t kQuatFloats = 4;
constexpr std::uint32_t kMat4Floats = 16;

/// Bits of the smallest possible element of each vector-typed field, used to
/// reject an absurd element count against the bits actually left in the buffer
/// before anything is allocated. An AssetPath's minimum is its 1-byte
/// zero-length varint; an AssetId is a fixed 16 bytes.
constexpr std::size_t kMinAssetPathBits = 8;
constexpr std::size_t kMinAssetIdBits   = 128;

/// Mask of the low @p bitCount bits, defined at bitCount == 64.
constexpr FieldMask LowFieldMask(std::size_t bitCount)
{
    return bitCount >= kMaxCodecFields ? kAllFields : (FieldMask{1} << bitCount) - 1u;
}

const std::byte *FieldAddress(const void *component, std::size_t offset)
{
    return static_cast<const std::byte *>(component) + offset;
}

std::byte *FieldAddress(void *component, std::size_t offset)
{
    return static_cast<std::byte *>(component) + offset;
}

/// memcpy rather than a cast through the void*: the field pointer is derived from
/// a byte offset, so a reinterpret_cast would carry an alignment assumption the
/// compiler is entitled to act on. Every mainstream compiler folds these back to
/// a single load/store.
template <typename T> T LoadPod(const std::byte *address)
{
    T value{};
    std::memcpy(&value, address, sizeof(T));
    return value;
}

template <typename T> void StorePod(std::byte *address, const T &value)
{
    std::memcpy(address, &value, sizeof(T));
}

void WriteFloats(BitWriter &writer, const std::byte *address, std::uint32_t count)
{
    for (std::uint32_t i = 0; i < count; ++i)
        writer.WriteFloat(LoadPod<float>(address + i * sizeof(float)));
}

void ReadFloats(BitReader &reader, std::byte *address, std::uint32_t count)
{
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const float value = reader.ReadFloat();
        StorePod(address + i * sizeof(float), value);
    }
}

/// Reads an enum's underlying integer at its true width. Switching on the width
/// (rather than memcpy'ing into a uint64) keeps this correct on a big-endian host
/// as well: each branch loads a host-native integer of the declared type.
bool LoadEnum(const std::byte *address, std::uint8_t size, std::uint64_t &out)
{
    switch (size)
    {
    case 1: out = LoadPod<std::uint8_t>(address); return true;
    case 2: out = LoadPod<std::uint16_t>(address); return true;
    case 4: out = LoadPod<std::uint32_t>(address); return true;
    case 8: out = LoadPod<std::uint64_t>(address); return true;
    default: return false;
    }
}

bool StoreEnum(std::byte *address, std::uint8_t size, std::uint64_t value)
{
    switch (size)
    {
    case 1: StorePod(address, static_cast<std::uint8_t>(value)); return true;
    case 2: StorePod(address, static_cast<std::uint16_t>(value)); return true;
    case 4: StorePod(address, static_cast<std::uint32_t>(value)); return true;
    case 8: StorePod(address, value); return true;
    default: return false;
    }
}

/// Packs an ECS::Entity's two 32-bit halves into the wire's 64-bit handle:
/// index low, generation high (see kEntityRefBits).
std::uint64_t PackEntity(const std::byte *address)
{
    const std::uint32_t index      = LoadPod<std::uint32_t>(address);
    const std::uint32_t generation = LoadPod<std::uint32_t>(address + sizeof(std::uint32_t));
    return static_cast<std::uint64_t>(index) | (static_cast<std::uint64_t>(generation) << 32u);
}

void UnpackEntity(std::byte *address, std::uint64_t packed)
{
    StorePod(address, static_cast<std::uint32_t>(packed & 0xFFFFFFFFu));
    StorePod(address + sizeof(std::uint32_t), static_cast<std::uint32_t>(packed >> 32u));
}

template <std::size_t Capacity> void ReadTrivialString(BitReader &reader, TrivialString<Capacity> &out)
{
    char              buffer[Capacity];
    const std::size_t length = reader.ReadStringInto(buffer, Capacity);
    if (reader.Failed())
        return; // sticky failure already latched; leave the destination alone
    out.Assign(std::string_view(buffer, length));
}

/// Shared element-count guard for the vector-typed fields: rejects a count that
/// exceeds the hard cap or that could not possibly fit in the bits remaining,
/// *before* any allocation happens.
bool ReadElementCount(BitReader &reader, std::size_t minElementBits, std::size_t &out)
{
    const std::uint64_t count = reader.ReadVarUInt64();
    if (reader.Failed())
        return false;
    if (count > kMaxVectorElements || count * minElementBits > reader.BitsRemaining())
    {
        // Not a recoverable condition: the sender and receiver disagree about the
        // stream, so the connection is already lost. Latch it on the reader so the
        // caller sees it through the same Ok() it checks everywhere else.
        reader.Invalidate();
        out = 0;
        return false;
    }
    out = static_cast<std::size_t>(count);
    return true;
}

std::uint64_t ApplyRemap(const std::function<std::uint64_t(std::uint64_t)> &hook, std::uint64_t value)
{
    return hook ? hook(value) : value;
}

/// @return false only for a field the codec cannot encode at all (Unknown, or an
/// enum with a nonsensical width) — a reflection bug, reported by the caller.
bool WriteField(const FieldMeta &field, const std::byte *address, BitWriter &writer, const CodecContext *context)
{
    switch (field.type)
    {
    case FieldType::Float:
        writer.WriteFloat(LoadPod<float>(address));
        return true;
    case FieldType::Double:
        writer.WriteDouble(LoadPod<double>(address));
        return true;
    case FieldType::Int32:
        writer.WriteInt32(LoadPod<std::int32_t>(address));
        return true;
    case FieldType::UInt32:
        writer.WriteUInt32(LoadPod<std::uint32_t>(address));
        return true;
    case FieldType::InstanceRef:
    {
        // A local instance id, translated to the instance's baseNetId by the
        // caller's hook — the same shape EntityRef gets below, for the same reason:
        // the number means nothing on the other machine. A null hook writes it
        // through, which is right for every same-process round trip.
        const std::uint32_t local = LoadPod<std::uint32_t>(address);
        writer.WriteUInt32(context != nullptr && context->instanceToWire ? context->instanceToWire(local)
                                                                        : local);
        return true;
    }
    case FieldType::Int64:
        writer.WriteInt64(LoadPod<std::int64_t>(address));
        return true;
    case FieldType::UInt64:
        writer.WriteUInt64(LoadPod<std::uint64_t>(address));
        return true;
    case FieldType::Bool:
        // One bit, not one byte — the reason the whole stack is bit-granular.
        writer.WriteBool(LoadPod<bool>(address));
        return true;
    case FieldType::Vec2:
        WriteFloats(writer, address, kVec2Floats);
        return true;
    case FieldType::Vec3:
        WriteFloats(writer, address, kVec3Floats);
        return true;
    case FieldType::Vec4:
        WriteFloats(writer, address, kVec4Floats);
        return true;
    case FieldType::Quat:
        // Whole-value for v1. The smallest-three encoding (2 + 9 + 9 + 9 = 29
        // bits) is the obvious next quantizer and needs no format break — only a
        // codec-version bump and a protocol-hash change.
        WriteFloats(writer, address, kQuatFloats);
        return true;
    case FieldType::Mat4:
        WriteFloats(writer, address, kMat4Floats);
        return true;
    case FieldType::Enum:
    {
        std::uint64_t value = 0;
        if (!LoadEnum(address, field.enumSize, value))
            return false;
        writer.WriteBits64(value, static_cast<std::uint32_t>(field.enumSize) * 8u);
        return true;
    }
    case FieldType::String:
        writer.WriteString(reinterpret_cast<const ShortString *>(address)->View());
        return true;
    case FieldType::EntityRef:
    {
        // The raw handle, optionally translated by the caller's hook. The codec
        // never allocates or resolves identities itself — see CodecContext.
        const std::uint64_t packed = PackEntity(address);
        writer.WriteBits64(context ? ApplyRemap(context->entityToWire, packed) : packed, kEntityRefBits);
        return true;
    }
    case FieldType::AssetPath:
        writer.WriteString(reinterpret_cast<const AssetPath *>(address)->View());
        return true;
    case FieldType::AssetPathVector:
    {
        const auto &paths = *reinterpret_cast<const std::vector<AssetPath> *>(address);
        writer.WriteVarUInt64(paths.size());
        for (const AssetPath &path : paths)
            writer.WriteString(path.View());
        return true;
    }
    case FieldType::ComponentMask:
    {
        // Names, not the bytes underneath — even here, where a raw copy would be
        // smaller and this data never actually crosses the wire (the only mask
        // lives on the Replicated marker, which is not itself replicable). Bit
        // index is a *replicable ordinal*, and the protocol hash covers
        // serializable component layouts rather than the whole registry, so two
        // builds can hash equal and still number their ordinals differently —
        // one id-only registration apart is enough. Raw bits would silently
        // re-aim; names cannot.
        const auto                                 &mask       = *reinterpret_cast<const ComponentMask *>(address);
        const std::span<const ComponentMeta *const> replicable = ComponentRegistry::Instance().ReplicableComponents();

        std::size_t count = 0;
        for (std::size_t ordinal = 0; ordinal < replicable.size(); ++ordinal)
            count += mask.Test(ordinal) ? 1u : 0u;

        writer.WriteVarUInt64(count);
        for (std::size_t ordinal = 0; ordinal < replicable.size(); ++ordinal)
        {
            if (mask.Test(ordinal))
                writer.WriteString(replicable[ordinal]->name);
        }
        return true;
    }
    case FieldType::AssetId:
        writer.WriteBytes(std::as_bytes(std::span{reinterpret_cast<const AssetId *>(address)->bytes}));
        return true;
    case FieldType::AssetIdVector:
    {
        const auto &ids = *reinterpret_cast<const std::vector<AssetId> *>(address);
        writer.WriteVarUInt64(ids.size());
        for (const AssetId &id : ids)
            writer.WriteBytes(std::as_bytes(std::span{id.bytes}));
        return true;
    }
    case FieldType::Unknown:
        break;
    }
    return false;
}

/// @return false for a field the codec cannot decode. Buffer overruns are *not*
/// reported here — they latch on the reader, which the caller tests once.
bool ReadField(const FieldMeta &field, std::byte *address, BitReader &reader, const CodecContext *context)
{
    switch (field.type)
    {
    case FieldType::Float:
        StorePod(address, reader.ReadFloat());
        return true;
    case FieldType::Double:
        StorePod(address, reader.ReadDouble());
        return true;
    case FieldType::Int32:
        StorePod(address, reader.ReadInt32());
        return true;
    case FieldType::UInt32:
        StorePod(address, reader.ReadUInt32());
        return true;
    case FieldType::InstanceRef:
    {
        const std::uint32_t wire = reader.ReadUInt32();
        StorePod(address, context != nullptr && context->instanceFromWire ? context->instanceFromWire(wire)
                                                                         : wire);
        return true;
    }
    case FieldType::Int64:
        StorePod(address, reader.ReadInt64());
        return true;
    case FieldType::UInt64:
        StorePod(address, reader.ReadUInt64());
        return true;
    case FieldType::Bool:
        StorePod(address, reader.ReadBool());
        return true;
    case FieldType::Vec2:
        ReadFloats(reader, address, kVec2Floats);
        return true;
    case FieldType::Vec3:
        ReadFloats(reader, address, kVec3Floats);
        return true;
    case FieldType::Vec4:
        ReadFloats(reader, address, kVec4Floats);
        return true;
    case FieldType::Quat:
        ReadFloats(reader, address, kQuatFloats);
        return true;
    case FieldType::Mat4:
        ReadFloats(reader, address, kMat4Floats);
        return true;
    case FieldType::Enum:
    {
        const std::uint64_t value = reader.ReadBits64(static_cast<std::uint32_t>(field.enumSize) * 8u);
        if (reader.Failed())
            return true; // overrun: reported through the reader, not as a type error
        return StoreEnum(address, field.enumSize, value);
    }
    case FieldType::String:
        ReadTrivialString(reader, *reinterpret_cast<ShortString *>(address));
        return true;
    case FieldType::EntityRef:
    {
        const std::uint64_t wire = reader.ReadBits64(kEntityRefBits);
        UnpackEntity(address, context ? ApplyRemap(context->entityFromWire, wire) : wire);
        return true;
    }
    case FieldType::AssetPath:
        ReadTrivialString(reader, *reinterpret_cast<AssetPath *>(address));
        return true;
    case FieldType::AssetPathVector:
    {
        std::size_t count = 0;
        if (!ReadElementCount(reader, kMinAssetPathBits, count))
            return true; // the reader carries the failure
        auto &paths = *reinterpret_cast<std::vector<AssetPath> *>(address);
        paths.assign(count, AssetPath{});
        for (AssetPath &path : paths)
            ReadTrivialString(reader, path);
        return true;
    }
    case FieldType::ComponentMask:
    {
        std::size_t count = 0;
        // A name is a length-prefixed string, so it cannot occupy fewer bits than
        // an AssetPath does — reuse that floor to bound a hostile count against
        // the bits actually remaining, rather than trusting the varint.
        if (!ReadElementCount(reader, kMinAssetPathBits, count))
            return true; // the reader carries the failure

        auto &mask = *reinterpret_cast<ComponentMask *>(address);
        mask       = ComponentMask{};

        const ComponentRegistry &registry = ComponentRegistry::Instance();
        for (std::size_t i = 0; i < count; ++i)
        {
            const std::string name = reader.ReadString();
            if (!reader.Ok())
                return true;

            // A name this build does not know, or knows but does not consider
            // replicable, has no bit to land in. Dropped silently here rather
            // than warned: unlike the JSON path (which reads authored files and
            // where a typo deserves a shout), this decodes a peer's bytes, and a
            // capability-set difference is exactly what the handshake already
            // refuses to pair over.
            const ComponentMeta *meta = registry.Find(name);
            if (meta == nullptr)
                continue;
            const std::size_t ordinal = registry.ReplicableOrdinalOf(meta->id);
            if (ordinal != ComponentRegistry::kInvalidOrdinal)
                mask.Set(ordinal);
        }
        return true;
    }
    case FieldType::AssetId:
        reader.ReadBytes(std::as_writable_bytes(std::span{reinterpret_cast<AssetId *>(address)->bytes}));
        return true;
    case FieldType::AssetIdVector:
    {
        std::size_t count = 0;
        if (!ReadElementCount(reader, kMinAssetIdBits, count))
            return true;
        auto &ids = *reinterpret_cast<std::vector<AssetId> *>(address);
        ids.assign(count, AssetId{});
        for (AssetId &id : ids)
            reader.ReadBytes(std::as_writable_bytes(std::span{id.bytes}));
        return true;
    }
    case FieldType::Unknown:
        break;
    }
    return false;
}

/// Stable spelling of a FieldType for the protocol layout text. Deliberately not
/// the enumerator's numeric value: reordering FieldType must not silently change
/// the protocol hash of components that did not change, and a name reads in a
/// diff.
const char *FieldTypeName(FieldType type)
{
    switch (type)
    {
    case FieldType::Float: return "f32";
    case FieldType::Double: return "f64";
    case FieldType::Int32: return "i32";
    case FieldType::UInt32: return "u32";
    case FieldType::Int64: return "i64";
    case FieldType::UInt64: return "u64";
    case FieldType::Bool: return "bool";
    case FieldType::Vec2: return "vec2";
    case FieldType::Vec3: return "vec3";
    case FieldType::Vec4: return "vec4";
    case FieldType::Quat: return "quat";
    case FieldType::Mat4: return "mat4";
    case FieldType::Enum: return "enum";
    case FieldType::String: return "str";
    case FieldType::EntityRef: return "entity";
    case FieldType::AssetPath: return "assetpath";
    case FieldType::AssetPathVector: return "assetpath[]";
    case FieldType::AssetId: return "assetid";
    case FieldType::AssetIdVector: return "assetid[]";
    case FieldType::ComponentMask: return "compmask";
    // Distinct from "uint32" on purpose: the bytes are the same width and mean
    // different things — one is a number, the other is a baseNetId a peer has to
    // translate. This name is hashed, so two builds that disagree refuse to pair.
    case FieldType::InstanceRef: return "instance";
    case FieldType::Unknown: break;
    }
    return "unknown";
}

/// Exact decimal-free spelling of a float bound: the bounds are quantization
/// parameters, so two builds differing in the last mantissa bit must produce
/// different hashes. A formatted decimal could round them together.
std::string BoundText(bool present, float value)
{
    if (!present)
        return "-";
    return ToHex64(std::bit_cast<std::uint32_t>(value));
}

/// One line per wire field: its codec index, name, type, and every parameter
/// that changes what the bits *mean* rather than merely how many there are.
///
/// Shared by components and messages because they encode identically — the same
/// FieldMeta walk, the same wire-field filter — and a description that drifted
/// between the two would let a field type change be a protocol change on one
/// side of the wire and not the other.
void AppendWireFields(std::string &text, const std::vector<FieldMeta> &fields)
{
    std::size_t codecIndex = 0;
    for (const FieldMeta &field : fields)
    {
        if (!IsWireField(field))
            continue;

        text += "  ";
        text += std::to_string(codecIndex);
        text += ' ';
        text += field.name;
        text += ' ';
        text += FieldTypeName(field.type);

        // Quantization parameters travel in the hash, not just the layout: two
        // builds that quantize the same Vec3 over different ranges corrupt each
        // other *silently*, which is the one failure mode a handshake exists to
        // prevent. Today the parameters are the AFIELD min/max bounds and the
        // enum width; when per-field quantization bit counts land in FieldMeta
        // they must be appended here too.
        text += " min=";
        text += BoundText(field.hasMin, field.minValue);
        text += " max=";
        text += BoundText(field.hasMax, field.maxValue);

        if (field.type == FieldType::Enum)
        {
            text += " esize=";
            text += std::to_string(field.enumSize);
            text += field.enumSigned ? " signed" : " unsigned";
            // Enumerator *values* are wire semantics: renumbering one keeps the
            // layout identical while changing what the bits mean.
            for (const EnumConstant &constant : field.enumConstants)
            {
                text += ' ';
                text += constant.name;
                text += '=';
                text += std::to_string(constant.value);
            }
        }
        text += '\n';
        ++codecIndex;
    }
}

} // namespace

std::size_t CountCodecFields(const ComponentMeta &meta)
{
    std::size_t count = 0;
    for (const FieldMeta &field : meta.fields)
        if (IsWireField(field))
            ++count;
    return count;
}

bool WriteComponent(const ComponentMeta &meta, const void *component, BitWriter &writer, FieldMask mask,
                    const CodecContext *context)
{
    if (meta.id == kInvalidComponentId)
    {
        ASSISI_ASSERT(false, "WriteComponent: component id is not finalized — the registry finalizes "
                             "lazily on first query, so call this only after startup registration.");
        Log::Error("BinaryCodec: refusing to encode '{}' — its ComponentId is not finalized", meta.name);
        return false;
    }

    const std::size_t fieldCount = CountCodecFields(meta);
    if (fieldCount > kMaxCodecFields)
    {
        ASSISI_ASSERT(false, "WriteComponent: more wire fields than the change mask can address");
        Log::Error("BinaryCodec: '{}' has {} wire fields, over the {}-field limit", meta.name, fieldCount,
                   kMaxCodecFields);
        return false;
    }

    // Bits above the field count are noise from a kAllFields caller; clearing
    // them keeps the encoded mask byte-for-byte identical to what the reader
    // reconstructs, which the round-trip tests depend on.
    mask &= LowFieldMask(fieldCount);

    writer.WriteVarUInt32(meta.id);
    writer.WriteBits64(mask, static_cast<std::uint32_t>(fieldCount));

    std::size_t codecIndex = 0;
    for (const FieldMeta &field : meta.fields)
    {
        if (!IsWireField(field))
            continue;

        if ((mask & FieldMaskBit(codecIndex)) != 0)
        {
            if (!WriteField(field, FieldAddress(component, field.offset), writer, context))
            {
                // Loud, never silent: a field the codec cannot encode means the
                // receiver would misparse everything after it. Better a refused
                // component (and a log line naming the field) than a stream that
                // decodes into garbage.
                ASSISI_ASSERT(false, "WriteComponent: unencodable field type (FieldType::Unknown, or an enum "
                                     "with a width that is not 1/2/4/8 bytes)");
                Log::Error("BinaryCodec: cannot encode field '{}::{}' (type {}, enumSize {})", meta.name, field.name,
                           FieldTypeName(field.type), field.enumSize);
                return false;
            }
        }
        ++codecIndex;
    }

    return true;
}

bool WriteMessage(const MessageMeta &meta, const void *message, BitWriter &writer, const CodecContext *context)
{
    if (meta.id == kInvalidMessageId)
    {
        ASSISI_ASSERT(false, "WriteMessage: message id is not finalized — the registry finalizes lazily on "
                             "first query, so call this only after startup registration.");
        Log::Error("BinaryCodec: refusing to encode message '{}' — its MessageId is not finalized", meta.name);
        return false;
    }

    // The body is built apart from the packet because the length prefix has to
    // precede it and is only known once it is written. Copying it back in costs
    // one pass over a few dozen bits; messages are small and rare next to state,
    // and the alternative — reserving a fixed-width length and patching it — puts
    // a mutable hole in a stream whose whole virtue is that it is append-only.
    BitWriter body;
    for (const FieldMeta &field : meta.fields)
    {
        if (!IsWireField(field))
            continue;
        if (!WriteField(field, FieldAddress(message, field.offset), body, context))
        {
            ASSISI_ASSERT(false, "WriteMessage: unencodable field type");
            Log::Error("BinaryCodec: cannot encode message field '{}::{}' (type {}, enumSize {})", meta.name,
                       field.name, FieldTypeName(field.type), field.enumSize);
            return false;
        }
    }

    // Bits, not bytes: the stream is bit-packed and a message body starts
    // wherever the previous one ended, so byte-aligning it to make the prefix
    // prettier would cost up to seven bits per message for nothing.
    const std::size_t bodyBits = body.BitsWritten();
    writer.WriteVarUInt32(meta.id);
    writer.WriteVarUInt32(static_cast<std::uint32_t>(bodyBits));

    BitReader   copy(body.Data());
    std::size_t remaining = bodyBits;
    while (remaining > 0)
    {
        const std::uint32_t chunk = static_cast<std::uint32_t>(std::min<std::size_t>(remaining, 64));
        writer.WriteBits64(copy.ReadBits64(chunk), chunk);
        remaining -= chunk;
    }
    return true;
}

MessageId ReadMessageId(BitReader &reader)
{
    const MessageId id = reader.ReadVarUInt32();
    return reader.Failed() ? kInvalidMessageId : id;
}

bool ReadMessage(const MessageMeta &meta, void *message, BitReader &reader, const CodecContext *context)
{
    const std::uint32_t bodyBits = reader.ReadVarUInt32();
    if (reader.Failed() || bodyBits > reader.BitsRemaining())
    {
        // A length past the end of the buffer is either a truncated packet or a
        // hostile one, and both are the same answer.
        reader.Invalidate();
        return false;
    }

    const std::size_t bodyEnd = reader.BitsRead() + bodyBits;

    for (const FieldMeta &field : meta.fields)
    {
        if (!IsWireField(field))
            continue;
        if (!ReadField(field, FieldAddress(message, field.offset), reader, context))
        {
            Log::Error("BinaryCodec: cannot decode message field '{}::{}' (type {}, enumSize {})", meta.name,
                       field.name, FieldTypeName(field.type), field.enumSize);
            return false;
        }
        if (reader.Failed())
            return false;
    }

    // The declared length is the authority on where the next message starts, not
    // where this one's fields happened to stop. They agree for a matched pair;
    // when they do not, trusting the fields would misalign every message after
    // this one, so the cursor is put where the sender said the body ended.
    if (reader.BitsRead() != bodyEnd)
    {
        if (reader.BitsRead() > bodyEnd)
        {
            reader.Invalidate(); // read past the body: the two builds disagree
            return false;
        }
        std::size_t skip = bodyEnd - reader.BitsRead();
        while (skip > 0)
        {
            const std::uint32_t chunk = static_cast<std::uint32_t>(std::min<std::size_t>(skip, 64));
            (void)reader.ReadBits64(chunk);
            skip -= chunk;
        }
    }
    return !reader.Failed();
}

bool FieldsWithinBounds(std::span<const FieldMeta> fields, const void *object, std::string *outField)
{
    for (const FieldMeta &field : fields)
    {
        if (!field.hasMin && !field.hasMax)
            continue;

        // Bounds are only ever attached to numeric fields — reflectgen refuses
        // them elsewhere — so anything else here is a field that simply has no
        // range to be outside of.
        double value = 0.0;
        const void *address = FieldAddress(object, field.offset);
        switch (field.type)
        {
        case FieldType::Float:  value = *static_cast<const float *>(address); break;
        case FieldType::Double: value = *static_cast<const double *>(address); break;
        case FieldType::Int32:  value = *static_cast<const std::int32_t *>(address); break;
        case FieldType::UInt32: value = *static_cast<const std::uint32_t *>(address); break;
        case FieldType::Int64:  value = static_cast<double>(*static_cast<const std::int64_t *>(address)); break;
        case FieldType::UInt64: value = static_cast<double>(*static_cast<const std::uint64_t *>(address)); break;
        default: continue;
        }

        // NaN fails both comparisons, which is the answer we want: a value that
        // is not ordered against the bounds is not within them.
        const bool belowMin = field.hasMin && !(value >= static_cast<double>(field.minValue));
        const bool aboveMax = field.hasMax && !(value <= static_cast<double>(field.maxValue));
        if (belowMin || aboveMax)
        {
            if (outField != nullptr)
                *outField = field.name;
            return false;
        }
    }
    return true;
}

bool SkipMessageBody(BitReader &reader)
{
    const std::uint32_t bodyBits = reader.ReadVarUInt32();
    if (reader.Failed() || bodyBits > reader.BitsRemaining())
    {
        reader.Invalidate();
        return false;
    }

    std::size_t skip = bodyBits;
    while (skip > 0)
    {
        const std::uint32_t chunk = static_cast<std::uint32_t>(std::min<std::size_t>(skip, 64));
        (void)reader.ReadBits64(chunk);
        skip -= chunk;
    }
    return !reader.Failed();
}

ComponentId ReadComponentId(BitReader &reader)
{
    const ComponentId id = reader.ReadVarUInt32();
    return reader.Failed() ? kInvalidComponentId : id;
}

bool ReadComponent(const ComponentMeta &meta, void *component, BitReader &reader, FieldMask *appliedMask,
                   const CodecContext *context)
{
    if (appliedMask)
        *appliedMask = 0;

    const std::size_t fieldCount = CountCodecFields(meta);
    if (fieldCount > kMaxCodecFields)
    {
        Log::Error("BinaryCodec: '{}' has {} wire fields, over the {}-field limit", meta.name, fieldCount,
                   kMaxCodecFields);
        return false;
    }

    const FieldMask mask = reader.ReadBits64(static_cast<std::uint32_t>(fieldCount));
    if (reader.Failed())
        return false;

    std::size_t codecIndex = 0;
    for (const FieldMeta &field : meta.fields)
    {
        if (!IsWireField(field))
            continue;

        if ((mask & FieldMaskBit(codecIndex)) != 0)
        {
            if (!ReadField(field, FieldAddress(component, field.offset), reader, context))
            {
                Log::Error("BinaryCodec: cannot decode field '{}::{}' (type {}, enumSize {})", meta.name, field.name,
                           FieldTypeName(field.type), field.enumSize);
                return false;
            }
            // Bail on the first overrun rather than walking the rest of the
            // fields: every later read would fail too, and a truncated packet
            // should cost one check, not one per field.
            if (reader.Failed())
                return false;
        }
        ++codecIndex;
    }

    if (appliedMask)
        *appliedMask = mask;
    return true;
}

std::string ProtocolLayoutDescription(std::span<const ComponentMeta> components)
{
    std::string text;
    text.reserve(components.size() * 128u);

    text += "assisi-proto codec=";
    text += std::to_string(kCodecVersion);
    text += " components=";
    text += std::to_string(components.size());
    text += '\n';

    for (const ComponentMeta &meta : components)
    {
        text += std::to_string(meta.id);
        text += ' ';
        text += meta.name;
        // Whether the component replicates at all is wire semantics, not layout:
        // two builds that disagree about it exchange different component sets
        // while every field description matches, so nothing else here would
        // catch it.
        // The emitted spelling is deliberately *not* renamed alongside the flag:
        // this text is hashed, and changing a word here would repartition every
        // deployed build into incompatible pairs for no semantic reason. The
        // flag is `replicable` in C++; the wire calls it what it always called
        // it. TestReplication pins the hash across the rename to prove it.
        text += meta.replicable ? " replicated" : " local";
        text += '\n';

        // Only wire fields, and their codec index — so making a field transient
        // or norep shifts every later index and changes the hash, which is
        // correct: it changes the mask width and the payload order.
        AppendWireFields(text, meta.fields);
    }
    return text;
}

std::string MessageLayoutDescription(std::span<const MessageMeta> messages)
{
    std::string text;
    text.reserve(messages.size() * 128u);

    text += "messages=";
    text += std::to_string(messages.size());
    text += '\n';

    for (const MessageMeta &meta : messages)
    {
        text += std::to_string(meta.id);
        text += ' ';
        text += meta.name;

        // Direction and reliability are wire *semantics* rather than layout, and
        // they belong in the hash for the same reason `replicated` does: two
        // builds that disagree about which way a message travels, or about
        // whether it must arrive, have identical field descriptions and
        // completely different behaviour. Reclassifying a message is a protocol
        // change, and this is what makes it one.
        text += meta.direction == MessageDirection::Intent ? " intent" : " event";
        text += meta.reliability == MessageReliability::Reliable ? " reliable" : " unreliable";
        if (meta.independent)
            text += " independent";
        text += '\n';

        AppendWireFields(text, meta.fields);
    }
    return text;
}

std::string ProtocolLayoutDescription()
{
    return ProtocolLayoutDescription(ComponentRegistry::Instance().All()) +
           MessageLayoutDescription(MessageRegistry::Instance().All());
}

std::uint64_t ProtocolHash(std::span<const ComponentMeta> components)
{
    const std::string text = ProtocolLayoutDescription(components);
    return ContentHash64(std::as_bytes(std::span{text.data(), text.size()}));
}

std::uint64_t ProtocolHash()
{
    const std::string text = ProtocolLayoutDescription();
    return ContentHash64(std::as_bytes(std::span{text.data(), text.size()}));
}

std::string ProtocolSummary(std::span<const ComponentMeta> components)
{
    std::string summary = "assisi-proto/";
    summary += std::to_string(kCodecVersion);
    summary += " components=";
    summary += std::to_string(components.size());
    summary += " hash=";
    summary += ToHex64(ProtocolHash(components));
    return summary;
}

std::string ProtocolSummary()
{
    std::string summary = "assisi-proto/";
    summary += std::to_string(kCodecVersion);
    summary += " components=";
    summary += std::to_string(ComponentRegistry::Instance().Count());
    summary += " messages=";
    summary += std::to_string(MessageRegistry::Instance().Count());
    summary += " hash=";
    summary += ToHex64(ProtocolHash());
    return summary;
}

} // namespace Assisi::Core::Reflect
