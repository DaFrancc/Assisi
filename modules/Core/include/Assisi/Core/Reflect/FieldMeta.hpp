/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Reflect/FieldMeta.hpp
/// @brief Descriptor for a single reflected component field.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Assisi::Core::Reflect
{

enum class FieldType : std::uint8_t
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
    /// ECS::InstanceId — which blueprint instance something belongs to. A
    /// `std::uint32_t` in memory and its own type on purpose: the number is a
    /// per-world counter, so it means nothing on another machine and the codec has
    /// to translate it (CodecContext::instanceToWire) exactly as it translates an
    /// EntityRef. Appended rather than inserted, so no existing value shifts.
    InstanceRef,
    /// Core::EntityName — a wider inline string than String's, and its own type
    /// because the codec reads into the buffer by capacity: decoding one as a
    /// String would truncate it. Appended rather than inserted, so no existing
    /// value shifts.
    EntityName,
    /// Math::Color3 / Math::Color4 — linear RGB(A). Identical to Vec3/Vec4 in
    /// memory and in every codec; separate types so an editor offers a colour
    /// picker for a colour and drag boxes for a direction, without a per-field
    /// hint that could be attached to the wrong vector. Appended rather than
    /// inserted, so no existing value shifts.
    Color3,
    Color4,
    Unknown,
};

/// @brief One enumerator of a reflected `enum class` (FieldType::Enum).
///
/// The value is the enumerator's integer value; an editor presents `name` and
/// writes `value` back. Serialization uses the value, so reordering enumerators
/// is safe but changing an existing enumerator's value migrates saved data.
struct EnumConstant
{
    std::string name;
    std::int64_t value = 0;
};

/// @brief What an editor does to a "radio" listener field when its source enum
/// is not at one of the field's active values (see FieldMeta::radioSource).
enum class RadioBehavior : std::uint8_t
{
    None,   ///< Not a radio listener (the default).
    Grey,   ///< Disable (grey out) the field while inactive.
    Vanish, ///< Hide the field entirely while inactive.
};

// Every member carries a default initializer, the empty ones included, and that
// is load-bearing rather than tidiness: reflectgen writes a FieldMeta with
// designated initializers and names only the members an annotation asked for, and
// -Wmissing-field-initializers objects to an omitted member that has no default of
// its own. So a member added here without one breaks every generated file at once.
struct FieldMeta
{
    std::string name{};
    FieldType type      = FieldType::Unknown;
    std::size_t offset    = 0;
    bool transient = false;        ///< If true, excluded from serialization.

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
    bool hasMin   = false;  ///< True when AFIELD supplied min=...
    bool hasMax   = false;  ///< True when AFIELD supplied max=...
    float minValue = 0.f;   ///< Inclusive lower bound; meaningful when hasMin and minField is empty.
    float maxValue = 0.f;   ///< Inclusive upper bound; meaningful when hasMax and maxField is empty.

    /// A sibling numeric field this bound is read from instead of minValue /
    /// maxValue, named by AFIELD(min = otherField) / AFIELD(max = otherField).
    /// Empty when the bound is a literal, which is the ordinary case.
    ///
    /// For the bounds one field of a component imposes on another: a spot light's
    /// inner cone cannot open wider than its outer cone, and the number that caps
    /// it is whatever the outer angle is at the time. A literal cannot say that —
    /// any constant is either too small to allow legal values or too large to
    /// exclude illegal ones.
    ///
    /// Resolution is one step and never recurses: the named field's own bounds are
    /// not consulted. So two fields may name each other, and that pair means what
    /// it reads as — neither may cross the other.
    ///
    /// **Never read these directly** — ResolveFieldBounds is what turns a FieldMeta
    /// and an object into the numbers that apply. Reading minValue while minField
    /// is set silently clamps to zero.
    std::string minField{};
    std::string maxField{};

    // Populated only for FieldType::Enum: the enumerators to offer in an editor,
    // in declaration order. Empty for every other field type.
    std::vector<EnumConstant> enumConstants{};

    // The enum's underlying storage, so an editor reads/writes the field at its
    // true width instead of assuming a 4-byte int (which would corrupt neighbours
    // for an 8/16-bit enum). enumSize is the byte width (1/2/4/8); 0 marks a
    // non-enum field. enumSigned selects sign-extension when reading.
    std::uint8_t enumSize   = 0;     ///< Underlying byte width; 0 = not an enum.
    bool enumSigned = false;         ///< Underlying type is signed (sign-extend on read).

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
    std::string radioSource{};                                      ///< Sibling enum field this field's visibility follows ("" = not a listener).
    std::vector<std::int64_t> radioValues{};                        ///< Enum values at which this field is active; meaningful when radioSource set.
    RadioBehavior radioBehavior = RadioBehavior::None;               ///< Editor treatment while inactive.

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

    /// @brief AFIELD(subject): this event is *about* the entity this field names,
    /// and relevancy scopes its delivery by that entity.
    ///
    /// Only meaningful on an `EntityRef` field of an `AMSG(event, …)` that is not
    /// `independent`, and reflectgen rejects it anywhere else — including on an
    /// intent, which has exactly one recipient and so has nothing to scope.
    /// Exactly one field per event carries it, which reflectgen also enforces:
    /// the subject is what relevancy filters on, what the recipient's queue holds
    /// the message for until it has been told about that entity, and what evicts
    /// the message when the entity dies. None of those three has an answer for
    /// two subjects — intersection starves the message, union leaks it.
    ///
    /// Other `EntityRef` fields on the same event are ordinary: they travel and
    /// translate like any reference, they simply do not decide who is told. A
    /// recipient that has never heard of one of them decodes it as `NullEntity`,
    /// since a message — unlike a component — has no deferred-reference patch-up.
    ///
    /// Marked rather than inferred from declaration order: an inferred subject
    /// would let reordering two fields silently change the audience.
    ///
    /// Deliberately **not** in the protocol hash, for the same reason as
    /// `controlled`: it changes who a message is sent to, never how its bytes
    /// decode, so two builds differing only here still parse each other.
    bool subject = false;
};

/// @brief The bounds a field is held to right now: literals as written, and
/// sibling-field bounds as whatever that sibling currently holds.
///
/// Doubles rather than floats because the widest bounded field is 64-bit, and a
/// bound narrowed to float would move — outward for a max, which lets an illegal
/// value through, and that is the direction that matters.
struct FieldBounds
{
    bool hasMin = false;
    bool hasMax = false;
    double minValue = 0.0;
    double maxValue = 0.0;
};

/// @brief Read @p field of @p object as a double, for any numeric FieldType.
///
/// False for every other type, leaving @p out untouched — a Vec3 or a string has
/// no single number to be compared against a bound.
[[nodiscard]] bool ReadNumericField(const FieldMeta &field, const void *object, double &out);

/// @brief What @p field's bounds actually are for this @p object.
///
/// The one way to read a bound. A bound naming a sibling is resolved against
/// @p siblings — the field list of the component @p field belongs to — and a name
/// that resolves to nothing, or to a field with no number in it, drops that bound
/// rather than inventing one: reflectgen has already refused the annotations that
/// could get here wrong, so the remaining case is a caller passing the wrong
/// component's fields, and a bound of zero would be a quiet clamp to zero.
[[nodiscard]] FieldBounds ResolveFieldBounds(const FieldMeta &field, std::span<const FieldMeta> siblings,
                                             const void *object);

} // namespace Assisi::Core::Reflect