/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file LevelError.hpp
/// @brief Why a level would not load, and the result type that carries it.
///
/// Split out of SceneSerializer.hpp so a caller can *report* a load failure
/// without paying for nlohmann/json to describe one. That header is included
/// through World.hpp almost everywhere, which is why it forward-declares rather
/// than includes wherever it can; a function returning LevelResult needs the
/// enum complete, and this is the whole of what it needs.

#include <expected>
#include <string_view>

namespace Assisi::Runtime
{
// (LoadOptions lives in SceneSerializer.hpp, next to the loads it configures.)

/// @brief Why a level file could not be loaded, or an instance could not be placed.
///
/// Says what *kind* of thing is wrong; which entity, which member and which name
/// are logged where they are known, exactly as BlueprintError does in
/// Blueprint.hpp. Every one of these is a file that means something other than
/// what it says — the alternative to refusing is a silently mis-wired scene.
enum class LevelError
{
    FileUnreadable,      ///< The asset system or filesystem could not read the file.
    MalformedJson,       ///< Read, but not parseable as JSON.
    UnsupportedVersion,  ///< A `version` this build does not read. Refused before the scene is cleared.
    NoInstanceTable,     ///< The file places instances and the caller passed no table to put them in.
    MissingName,         ///< An entity or instance entry with no `name`.
    MissingSource,       ///< An instance entry that names no `source`.
    InvalidName,         ///< A name ValidateName refuses — empty, too long, or holding a `/`.
    DuplicateName,       ///< Two entities, or two claims on one member path, under one name.
    NonUniformScale,     ///< An instance placement that shears; cannot compose exactly (§3).
    BlueprintUnusable,   ///< An instance names a blueprint that will not load; see BlueprintError.
    UnresolvedReference, ///< An EntityRef naming an entity or member the file does not declare.
    MalformedComponent,  ///< A component field is present but unreadable — a string where a number goes.
    ContextBusy,         ///< A serialization context is already active on this thread (a caller bug).
    InstanceNotLive,     ///< Re-expansion was asked for an instance id no longer in the table.
    NameAlreadyLive,     ///< Placing would put two instances of one name in a world.
};

/// @brief One line saying what is wrong, for a log or a field hint.
[[nodiscard]] std::string_view Describe(LevelError error);

/// @brief A refused load: what was wrong with the file, and whether the attempt
/// had already replaced the caller's scene before it gave up.
///
/// A refusal before the clear leaves the caller's entity handles valid; one after
/// it leaves an empty scene those handles silently alias into (round-7 B20).
/// @ref kind cannot answer which — `MissingName` is returned from both sides of
/// the clear — so it is reported separately.
struct LevelFailure
{
    LevelError kind;

    /// True if the caller's scene is gone. Set where the clearing happens.
    bool sceneReplaced = false;

    /// So `result.error() == LevelError::DuplicateName` keeps working.
    [[nodiscard]] friend bool operator==(const LevelFailure &failure, LevelError error)
    {
        return failure.kind == error;
    }
};

/// @brief One line saying what is wrong, for a log or a field hint.
[[nodiscard]] inline std::string_view Describe(const LevelFailure &failure) { return Describe(failure.kind); }

/// @brief A load either worked or it did not; there is nothing to hand back on
/// success beyond that. The scene is the output.
using LevelResult = std::expected<void, LevelFailure>;

} // namespace Assisi::Runtime
