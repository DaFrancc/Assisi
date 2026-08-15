/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Reflect/BinaryCodec.hpp
/// @brief Reflection-driven binary codec for component state — the network wire
///        format.
///
/// JSON stays the level-file format (readable, diffable, name-keyed). The network
/// gets this: a compact little-endian bitstream driven by the same `FieldMeta`
/// the editor and the JSON serializer already walk, so a component becomes
/// replicable by being reflected, with no hand-written per-type codec to forget
/// to update.
///
/// **Per-component block:**
/// @code
///   [ ComponentId varint ][ field-changed bitmask ][ payloads of set fields ]
/// @endcode
/// The bitmask carries exactly one bit per *wire* field — non-transient and
/// non-`norep`, in declaration order — so its width is part of the protocol
/// hash, and a field flipping to `transient` or `norep` is a protocol change,
/// correctly.
///
/// **There is no separate "full snapshot" format.** Full state is a delta against
/// the empty baseline: pass `kAllFields`. That is the Quake 3 unification — spawn,
/// delta, keyframe and late-join all run one code path, so the rarely-exercised
/// one cannot rot.
///
/// **Wire identity is `ComponentId`, not name.** The registry sorts by name and
/// assigns dense ids, so ids agree across same-build binaries; `ProtocolHash()`
/// verifies that agreement at handshake instead of trusting it.
///
/// **Trust boundary.** Decoding runs on untrusted bytes. `ReadComponent` never
/// reads outside the buffer, never throws, and reports failure through the
/// reader's sticky `Failed()` — a truncated or bit-flipped packet leaves the
/// component partially written with zeroes and the connection to be dropped by
/// the caller, never memory corruption. See TestBinaryCodec.cpp's fuzz cases.
///
/// **Core does not link glm and must not include ECS.** The glm-typed fields
/// (Vec2/3/4, Quat, Mat4) are therefore encoded as their raw float arrays at the
/// field offset — 2/3/4/4/16 floats — and `EntityRef` as the raw 64 bits of
/// `ECS::Entity` (`uint32 index` then `uint32 generation`, verified against
/// modules/ECS/include/Assisi/ECS/Entity.hpp). Both choices keep Core's
/// dependency surface exactly where it is; the cost is two hardcoded layout
/// facts in this file, which TestBinaryCodec.cpp pins by redeclaring the entity
/// handle and the glm arrays it expects.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/Reflect/ComponentId.hpp>
#include <Assisi/Core/Reflect/ComponentMeta.hpp>
#include <Assisi/Core/Reflect/MessageMeta.hpp>

namespace Assisi::Core::Reflect
{

/// @brief One bit per wire field, in declaration order.
///
/// Bit N corresponds to the N-th field that survives the IsWireField filter —
/// not to `meta.fields[N]`. Use FieldMaskBit()/CountCodecFields() rather than
/// counting by hand.
using FieldMask = std::uint64_t;

/// @brief Whether @p field occupies a slot in the wire's field mask.
///
/// Two exclusions, for two different reasons: `transient` fields are runtime-only
/// and serialize nowhere, `norep` fields serialize to disk but never to the
/// network (see FieldMeta::norep). Both shift every later codec index, which is
/// why both are folded into the protocol hash.
[[nodiscard]] constexpr bool IsWireField(const FieldMeta &field)
{
    return !field.transient && !field.norep;
}

/// @brief Every field set — a full-state block (a delta against the empty
/// baseline). Bits past the component's field count are ignored.
inline constexpr FieldMask kAllFields = ~FieldMask{0};

/// @brief Maximum wire fields a component may have to be network-encodable.
///
/// The mask is one machine word, which keeps the per-block overhead at one
/// bitfield read. No engine component is close to this; a component that grows
/// past it wants splitting anyway (it is one replication unit — all-or-nothing
/// for delta granularity). Encoding one asserts and refuses rather than silently
/// dropping the tail.
inline constexpr std::size_t kMaxCodecFields = 64;

/// @brief Bit width of a serialized `EntityRef`.
///
/// `ECS::Entity` is `{ uint32_t index; uint32_t generation; }`. Core cannot
/// include the ECS header to say so, so the width is hardcoded here and the
/// packing is fixed by contract: `index` in the low 32 bits, `generation` in the
/// high 32. Both ends of the wire and the Stage 5 remap hook agree on that
/// layout.
inline constexpr std::uint32_t kEntityRefBits = 64;

/// @brief Hard cap on the element count of a vector-typed field read off the wire.
///
/// The count prefix is attacker-controlled. The reader additionally checks the
/// count against the bits actually remaining, which is the tighter bound for
/// AssetId vectors (128 bits each); this cap bounds the pathological case where
/// the buffer really is that large.
inline constexpr std::size_t kMaxVectorElements = 4096;

/// @brief Optional per-call hooks the codec routes reference-typed fields through.
///
/// Two jobs: `EntityRef` and `InstanceRef` translation. The codec deliberately
/// does *not* know about NetIds: local `(index, generation)` handles and local
/// instance ids are not stable across machines, but the map that fixes that is
/// replication state (Stage 5), not codec state. Threading it as a hook means
/// Stage 5 substitutes NetIds without this file changing at all — and leaves the
/// codec independently testable with no replication session in scope.
///
/// A null hook (or a null context) writes the raw handle bits through unchanged,
/// which is exactly what a same-process round trip — save games, tests, the
/// editor — wants.
struct CodecContext
{
    /// Encode side: local packed handle → wire id. Stage 5 supplies NetId lookup.
    std::function<std::uint64_t(std::uint64_t)> entityToWire;
    /// Decode side: wire id → local packed handle. The inverse of entityToWire.
    std::function<std::uint64_t(std::uint64_t)> entityFromWire;

    /// Encode side: local blueprint instance id → the instance's `baseNetId`.
    /// Applied to `AFIELD(instanceRef)` UInt32 fields, for the same reason
    /// entityToWire exists: the local number names nothing on the other machine.
    std::function<std::uint32_t(std::uint32_t)> instanceToWire;
    /// Decode side: `baseNetId` → local instance id. The inverse.
    std::function<std::uint32_t(std::uint32_t)> instanceFromWire;
};

/// @brief Number of fields the codec encodes for @p meta — the fields that pass
/// IsWireField, which is also the bitmask's width.
[[nodiscard]] std::size_t CountCodecFields(const ComponentMeta &meta);

/// @brief Mask with only field @p codecIndex set. Returns 0 for an out-of-range
/// index, so `mask & FieldMaskBit(i)` is safe to fold in a loop.
[[nodiscard]] constexpr FieldMask FieldMaskBit(std::size_t codecIndex)
{
    return codecIndex < kMaxCodecFields ? (FieldMask{1} << codecIndex) : FieldMask{0};
}

/// @brief Writes a complete component block: id varint, field mask, payloads.
///
/// @param meta      The component's reflected descriptor. `meta.id` must be
///                  finalized (it is, any time after startup).
/// @param component Pointer to a live instance of that component type.
/// @param writer    Destination; appended to, never reset.
/// @param mask      Which fields to include — `kAllFields` for full state. Bits
///                  above the component's field count are ignored. The caller
///                  owns this decision because only it knows the baseline
///                  (Stage 5's per-connection acked state).
/// @param context   Optional reference-remap hooks; null means raw handles.
/// @return false if the component could not be encoded — an unfinalized id, more
///         than kMaxCodecFields fields, or a `FieldType::Unknown` field. Failure
///         is loud (asserts in debug, logs an error always) and leaves a partial
///         block in the writer: an unencodable component is a build-time
///         reflection bug, not a runtime condition to recover from, and silently
///         shipping a component whose fields the receiver will misparse is the
///         one outcome worse than refusing.
bool WriteComponent(const ComponentMeta &meta, const void *component, BitWriter &writer,
                    FieldMask mask = kAllFields, const CodecContext *context = nullptr);

/// @brief Reads the `ComponentId` prefix of a block.
///
/// Split from ReadComponent because the id is exactly what the caller needs to
/// find the `ComponentMeta` to pass *to* ReadComponent — the dispatch has to
/// happen between the two. So the asymmetry is deliberate: WriteComponent emits
/// the whole block; the read side is `ReadComponentId` → resolve → `ReadComponent`.
/// @return the decoded id, or kInvalidComponentId if the reader failed.
[[nodiscard]] ComponentId ReadComponentId(BitReader &reader);

/// @brief Reads a component block's mask and payloads (everything after the id
/// prefix) into @p component, leaving unmasked fields untouched.
///
/// Untouched is the point: a delta carries only what changed, so the destination
/// must be the receiver's current state and the decode is an in-place patch.
///
/// @param appliedMask Optional out-param receiving the mask that was on the
///                    wire, so a caller can tell which fields it just patched
///                    (Stage 5 uses it for interpolation bookkeeping).
/// @return false if the reader failed at any point, or a field type could not be
///         decoded. On failure the component holds whatever was patched before
///         the failure — the caller drops the connection rather than trusting it.
bool ReadComponent(const ComponentMeta &meta, void *component, BitReader &reader,
                   FieldMask *appliedMask = nullptr, const CodecContext *context = nullptr);

// ── Messages ──────────────────────────────────────────────────────────────────
// A message is a reflected struct, so it encodes through the same field walk a
// component does. Two things differ, and both come from what a message *is*:
// there is no baseline to delta against (an event is not a value that has a
// previous version), so every field always goes; and the payload is
// length-prefixed, so a reader that does not recognise the id can step over the
// body instead of losing the rest of the packet.
//
// The length prefix buys *skip*, not tolerance. With dense ids, a peer holding a
// different message set would step over the right number of bytes and then
// dispatch the wrong type for every id after the divergence. Real forward and
// backward tolerance needs stable, name-derived ids and a handshake policy that
// permits the mismatch in the first place; the prefix keeps that a policy change
// rather than a format rewrite.

/// @brief Writes a complete message block: id varint, byte length, then every
/// field.
///
/// @return false if the message could not be encoded — an unfinalized id, too
/// many fields, or a `FieldType::Unknown` field. Loud, for the same reason
/// WriteComponent is: shipping a block the receiver will misparse is worse than
/// refusing to ship one.
bool WriteMessage(const MessageMeta &meta, const void *message, BitWriter &writer,
                  const CodecContext *context = nullptr);

/// @brief Reads the `MessageId` prefix of a block. The caller resolves it to a
/// `MessageMeta` and then calls ReadMessage — or, for an id it does not know,
/// SkipMessageBody.
[[nodiscard]] MessageId ReadMessageId(BitReader &reader);

/// @brief Reads a message body into @p message, which must be a
/// default-constructed instance of @p meta's type.
///
/// Unlike ReadComponent this is not a patch: a message has no baseline, so
/// every field on the wire is written and the caller starts from a fresh value.
bool ReadMessage(const MessageMeta &meta, void *message, BitReader &reader,
                 const CodecContext *context = nullptr);

/// @brief Whether every `AFIELD(min=/max=)` bound on @p fields holds for the
/// value at @p object.
///
/// **Reject, do not clamp.** The input path clamps its commands, because a stick
/// can legitimately saturate and the honest reading of an out-of-range axis is
/// "all the way". A message is the opposite: an out-of-range field means the
/// sender is lying or the two builds disagree, and clamping converts a
/// detectable attack into a silently accepted one. So this reports, and the
/// caller drops.
///
/// @param outField Optionally receives the name of the first field that failed,
///   so the log line says which one rather than only that something did.
[[nodiscard]] bool FieldsWithinBounds(std::span<const FieldMeta> fields, const void *object,
                                      std::string *outField = nullptr);

/// @brief Step over the body of a message whose id this build does not know.
///
/// Reads the length prefix and advances past that many bits, leaving the reader
/// positioned at the next message. Fails the reader if the length runs past the
/// buffer — a hostile length must not be able to rewind or overrun.
bool SkipMessageBody(BitReader &reader);

// ── Protocol identity ─────────────────────────────────────────────────────────
// Two builds must agree on the component table and every field's wire encoding
// before a single snapshot is exchanged. Layout agreement alone is not enough:
// two builds quantizing a Vec3 differently corrupt *silently*, so the
// quantization parameters are inside the hash too.

/// @brief Bumped whenever the encoding rules here change in a way that is not
/// visible in the component table — bit order, block framing, varint form, a
/// field type's representation. A pure-layout change does not need it; the
/// component table already covers that.
inline constexpr std::uint8_t kCodecVersion = 1;

/// @brief The canonical protocol layout text: codec version, then every
/// component in id order with its replication policy and its wire fields' name,
/// type, and quantization parameters.
///
/// This is what gets hashed, and it is deliberately readable — when two builds
/// disagree, diffing the two descriptions names the offending field, which a
/// 64-bit mismatch never could.
///
/// Field *offsets* are excluded on purpose: they are local memory layout, not
/// wire layout. Two builds with different struct padding are perfectly
/// wire-compatible, and hashing offsets would reject them for nothing.
[[nodiscard]] std::string ProtocolLayoutDescription(std::span<const ComponentMeta> components);

/// @brief The message half of the same text: every registered `AMSG` in id
/// order, with its direction, reliability, independence, and wire fields.
[[nodiscard]] std::string MessageLayoutDescription(std::span<const MessageMeta> messages);

/// @brief ProtocolLayoutDescription over the whole ComponentRegistry *and* the
/// whole MessageRegistry, in id order.
///
/// Messages are in here because that is what makes declaring one a versioning
/// event: add a message, reorder its fields, or flip it from unreliable to
/// reliable, and the hash moves, so a mismatched pair refuses to connect rather
/// than misparsing each other.
[[nodiscard]] std::string ProtocolLayoutDescription();

/// @brief FNV-1a 64 of ProtocolLayoutDescription — the value exchanged at
/// handshake; a mismatch rejects the connection.
///
/// Stable across runs and machines for an identical build (the registry's
/// name-sorted determinism guarantees the ordering), and different whenever a
/// component or field is added, removed, renamed, retyped, reordered, or
/// re-quantized. Reuses Core::ContentHash64 rather than adding a second hash to
/// the codebase.
[[nodiscard]] std::uint64_t ProtocolHash(std::span<const ComponentMeta> components);

/// @brief ProtocolHash over the whole ComponentRegistry.
[[nodiscard]] std::uint64_t ProtocolHash();

/// @brief Short human-readable protocol string sent alongside the hash, e.g.
/// `"assisi-proto/1 components=37 hash=1a2b3c4d5e6f7081"`.
///
/// A bare 64-bit mismatch tells a player nothing; this makes the rejection
/// diagnosable from a log line ("their component count is 36, ours is 37") without
/// shipping the full layout description over the wire.
[[nodiscard]] std::string ProtocolSummary(std::span<const ComponentMeta> components);

/// @brief ProtocolSummary over the whole ComponentRegistry.
[[nodiscard]] std::string ProtocolSummary();

} // namespace Assisi::Core::Reflect
