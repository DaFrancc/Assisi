/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SceneSerializerHeader.hpp
/// @brief Reading the level header's non-entity fields, shared by the two entry
///        points that read them.

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace Assisi::Runtime
{

/// @brief The systems a level file names, in file order.
///
/// **One function because two callers must never disagree.** A level's systems
/// are read twice on the way into the editor: once by `App::LevelSystemsAreDeclared`
/// (via `ReadLevelSystems`) *before* anything is torn down, and again by `Load`
/// filling `LevelHeader::systems` for the `ApplySystems` call that happens after
/// the scene has already been replaced. Only the first can refuse cheaply — it
/// runs while the level on screen is still the one that was there. The second
/// runs past the point of no return, where a refusal costs the open level.
///
/// So the early check is only a real gate while it sees exactly what the late one
/// will. Two copies of this loop in two translation units would agree by
/// coincidence rather than by construction, and editing either one moves the
/// disagreement into the window between them. Pinned by
/// `App/tests/TestLevelSystemsPrecheck.cpp`.
///
/// Entries that are not strings are skipped rather than refused, and repeats are
/// kept rather than merged — the install is a union, and it is not this reader's
/// job to decide either. What matters far more than which rule this picks is that
/// there is one place picking it.
inline std::vector<std::string> ParseSystemNames(const nlohmann::json &doc)
{
    std::vector<std::string> out;
    const auto               systems = doc.find("systems");
    if (systems == doc.end() || !systems->is_array())
        return out;

    for (const auto &name : *systems)
    {
        if (name.is_string())
            out.push_back(name.get<std::string>());
    }
    return out;
}

} // namespace Assisi::Runtime
