/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PrePlayState.hpp
/// @brief What Run captures of the edited world, and what Stop puts back.
///
/// A play session is allowed to overwrite the world it started from. A join
/// loads the *host's* level straight into it — clearing and refilling the
/// instance table, retargeting the level path, installing the host's systems —
/// and gameplay spawns whatever else it likes. Stop undoes all of that.
///
/// Entities are not here: EditorApp captures those separately, at exact
/// (index, generation) identity, because reviving them in place is what keeps
/// the editing undo history's stored handles valid across a session. This holds
/// everything else a session can move.
///
/// **One struct, one capture, one restore — rather than a member per field.** A
/// field a per-field capture forgets is restored by nothing: Join → Stop then
/// leaves the author editing their own level with the host's instance rows in it,
/// flagged `authored`, with no dirty marker and nothing on screen to say so, and
/// the next Save writes those rows into their file. Anything a session can move
/// goes here, where the capture and the restore cannot drift apart.

#include <string>
#include <vector>

#include <Assisi/Runtime/Blueprint.hpp>

namespace Assisi::Editor
{

/// @brief The edited world as Run found it.
struct PrePlayState
{
    /// The level this world *is*. A join replaces it with the host's, so without
    /// this a Save after Stop writes the editing scene out over the host's
    /// filename.
    std::string levelPath;

    /// The systems installed on it. Restored through WorldManager::ApplySystems
    /// rather than by assignment — see RestorePrePlayState's return value.
    std::vector<std::string> systemNames;

    /// Every live instance, with its ids and its id allocator. The whole table
    /// rather than its contents: ids are what the undo history recorded against
    /// (InstanceTable::RestoreAt lands a revived instance on the exact id its
    /// members' tags still name), so an allocator that regressed would hand a
    /// spoken-for id to something else.
    Runtime::InstanceTable instances;
};

/// @brief Everything Stop will have to put back, as it stands right now.
[[nodiscard]] PrePlayState CapturePrePlayState(const std::string &levelPath,
                                               const std::vector<std::string> &systemNames,
                                               const Runtime::InstanceTable &instances);

/// @brief Puts @p captured back into the world the session has been using.
///
/// Both the path and the table are assigned unconditionally: a spawn during play
/// moves the table without touching the level path, and a join moves both, so
/// testing before assigning would only be a way to miss one.
///
/// @param liveSystemNames what the world's system list is *now*, which is the
///        only way to tell whether the session retargeted it.
/// @return true if the systems have to be reinstalled as well. The caller owns
///         that: installing a system list runs code and can fail, which is a
///         different kind of thing from restoring a value, and the failure has
///         to be reported where there is something to report it to.
bool RestorePrePlayState(const PrePlayState &captured, std::string &levelPath,
                         const std::vector<std::string> &liveSystemNames,
                         Runtime::InstanceTable &instances);

} // namespace Assisi::Editor
