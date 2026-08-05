/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Reflect/FieldMeta.hpp
/// @brief Descriptor for a single reflected component field.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Assisi::Core::Reflect
{

enum class FieldType
{
    Float,
    Double,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Bool,
    Vec2,
    Vec3,
    Vec4,
    Quat,
    Mat4,
    Enum,            ///< An AENUM-annotated `enum class`; stored as its underlying integer (see enumSize).
    String,          ///< Core::ShortString — a fixed-capacity inline string, serialized as a string.
    EntityRef,       ///< ECS::Entity serialized as a stable serial index via SceneSerializer.
    AssetPath,       ///< Core::AssetPath — a fixed-capacity virtual asset path, serialized as a string.
    AssetPathVector, ///< std::vector<Core::AssetPath> — serialized as a JSON array of strings.
    AssetId,         ///< Core::AssetId — a stable GUID reference, serialized as { guid, path-hint }.
    AssetIdVector,   ///< std::vector<Core::AssetId> — serialized as a JSON array of { guid, path-hint }.
    /// Reflect::ComponentMask — a set of replicable component types. A bitset in
    /// memory; an array of component *names* in every codec, because the bit
    /// index (a replicable ordinal) is not stable across builds. Appended to this
    /// enum rather than inserted, so no existing value shifts.
    ComponentMask,
    Unknown,
};

/// @brief One enumerator of a reflected `enum class` (FieldType::Enum).
///
/// The value is the enumerator's integer value; an editor presents `name` and
/// writes `value` back. Serialization uses the value, so reordering enumerators
/// is safe but changing an existing enumerator's value migrates saved data.
struct EnumConstant
{
    std::string  name;
    std::int64_t value = 0;
};

/// @brief What an editor does to a "radio" listener field when its source enum
/// is not at one of the field's active values (see FieldMeta::radioSource).
enum class RadioBehavior
{
    None,   ///< Not a radio listener (the default).
    Grey,   ///< Disable (grey out) the field while inactive.
    Vanish, ///< Hide the field entirely while inactive.
};

struct FieldMeta
{
    std::string name;
    FieldType   type      = FieldType::Unknown;
    std::size_t offset    = 0;
    bool        transient = false; ///< If true, excluded from serialization.

    /// @brief AFIELD(norep): saved to disk, never sent over the network.
    ///
    /// The sibling of `transient`, one layer in: a transient field is excluded
    /// from *every* codec, a norep field only from the binary one. It is how a
    /// replicable component keeps server-only bookkeeping — a spawn cooldown, an
    /// aggro table — without either splitting the component in two or leaking
    /// the value to every client. Legal only on an ACOMP(replicable) component
    /// (reflectgen rejects it elsewhere, where it would silently mean nothing),
    /// and mutually exclusive with `transient`.
    ///
    /// Like `transient`, it shifts every later field's codec index, so it is
    /// part of the protocol hash — see BinaryCodec's IsWireField.
    bool norep = false;

    // Editor hints from AFIELD(min=..., max=...): inclusive bounds an editor
    // must clamp numeric edits to (e.g. a light radius that must not go
    // negative). Hints only — serialization does not enforce them.
    bool  hasMin   = false; ///< True when AFIELD supplied min=...
    bool  hasMax   = false; ///< True when AFIELD supplied max=...
    float minValue = 0.f;   ///< Inclusive lower bound; meaningful when hasMin.
    float maxValue = 0.f;   ///< Inclusive upper bound; meaningful when hasMax.

    // Populated only for FieldType::Enum: the enumerators to offer in an editor,
    // in declaration order. Empty for every other field type.
    std::vector<EnumConstant> enumConstants;

    // The enum's underlying storage, so an editor reads/writes the field at its
    // true width instead of assuming a 4-byte int (which would corrupt neighbours
    // for an 8/16-bit enum). enumSize is the byte width (1/2/4/8); 0 marks a
    // non-enum field. enumSigned selects sign-extension when reading.
    std::uint8_t enumSize   = 0;     ///< Underlying byte width; 0 = not an enum.
    bool         enumSigned = false; ///< Underlying type is signed (sign-extend on read).

    // Radio: declarative editor visibility driven by a sibling enum's value. A
    // field annotated AFIELD(radioListen = { source = enumField, value = ...,
    // behavior = grey|vanish }) is "active" only while that sibling enum equals
    // one of radioValues; otherwise the editor greys or hides it per
    // radioBehavior. A broadcaster enum (AFIELD(radioBroadcast)) carries no radio
    // metadata here — it is an ordinary enum field that listeners reference by
    // name. Sources may themselves be listeners: when a source is inactive its
    // listeners hide unconditionally, so the editor resolves visibility by
    // walking radioSource up the chain (reflectgen rejects cycles). radioSource is
    // empty for every non-listener field.
    std::string               radioSource;                          ///< Sibling enum field this field's visibility follows ("" = not a listener).
    std::vector<std::int64_t> radioValues;                          ///< Enum values at which this field is active; meaningful when radioSource set.
    RadioBehavior             radioBehavior = RadioBehavior::None;   ///< Editor treatment while inactive.

    /// @brief AFIELD(controlled): this message field must name an entity the
    /// sender controls.
    ///
    /// Only meaningful on an `EntityRef` field of an `AMSG(intent, …)`, and
    /// reflectgen rejects it anywhere else. It marks the *subject* of an intent
    /// — "the pawn I am telling you to do something with" — as distinct from
    /// any other entity the message merely mentions, like the thing being shot
    /// at. Without the distinction the dispatch site could either check nothing
    /// or check every reference, and checking every reference would forbid a
    /// client from ever naming an entity it does not own.
    ///
    /// The check itself lives at the single dispatch site: an intent whose
    /// controlled field names an entity the sender does not control is dropped
    /// and counted, deliberately *not* treated as an error — control transfer
    /// has a propagation delay, so an honest client can send one.
    ///
    /// Deliberately **not** in the protocol hash. It changes which messages are
    /// accepted, never how bytes decode, so two builds differing only here
    /// still parse each other perfectly and the server's rule governs — the
    /// same argument that keeps the game's neverReplicate list out of the hash.
    bool controlled = false;

    /// @brief AFIELD(instanceRef): this integer names a blueprint instance, and
    /// the number is only meaningful on the machine that made it.
    ///
    /// Instance ids are per-world counters handed out from 1 (see
    /// Runtime::InstanceTable) — a server's "instance 7" names nothing on a
    /// client. The wire carries the instance's `baseNetId` instead, and each side
    /// translates at the codec boundary through CodecContext's
    /// `instanceToWire`/`instanceFromWire`, exactly as an EntityRef translates
    /// through `entityToWire`/`entityFromWire`.
    ///
    /// Only meaningful on a `UInt32` field; reflectgen rejects it anywhere else.
    /// A null hook (a same-process round trip — a save game, a test) writes the
    /// raw value through, which is the right answer when both sides are the same
    /// machine.
    ///
    /// **In** the protocol hash, unlike `controlled`. It changes how the bytes of
    /// that field are to be interpreted — a build that translates and one that
    /// does not would exchange numbers from two different id spaces and agree
    /// about every one of them.
    bool instanceRef = false;
};

} // namespace Assisi::Core::Reflect