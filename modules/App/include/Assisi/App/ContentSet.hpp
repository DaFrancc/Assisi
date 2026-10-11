/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ContentSet.hpp
/// @brief One hash over every level and blueprint file this build can resolve.
///
/// A level's own content hash names *which* level. This names the whole set, and
/// it has to, for a reason that only shows up once blueprints replicate: a
/// blueprint spawned from C++ is named by no level and would never be hashed —
/// while spawning replicates as "expand this file", which makes every blueprint's
/// content load-bearing across the wire.
///
/// The check is deliberately strict. *Any* difference refuses a join, including
/// files neither machine ever loads, because a stray experimental `.abp` is
/// indistinguishable to the hash from a car whose wheels moved. That is a
/// development-time cost and never a shipping one, and it buys the property the
/// rest of the design leans on: after a successful join, both machines are known
/// to expand any blueprint identically, so a blueprint can be named on the wire
/// by its index in the sorted list rather than by path.

#include <cstdint>
#include <string>
#include <vector>

namespace Assisi::App
{

/// @brief The result of scanning the asset root.
struct ContentSet
{
    /// Every `.alvl` and `.abp` under the asset root, as virtual paths, sorted.
    /// The index into this is how a blueprint is named on the wire — two bytes,
    /// no path string, no per-file hash — which only works because a successful
    /// join has already proven both machines built the same list.
    std::vector<std::string> paths;

    /// FNV-1a over `(path, contentHash)` for every entry in order.
    std::uint64_t hash = 0;
};

/// @brief Every `.alvl` and `.abp` under the asset root, as sorted virtual paths.
///
/// The listing half of BuildContentSet, split out because the editor's blueprint
/// panel wants the names without paying to hash every file to get them. Both
/// extensions, because they are one format and the extension never gates
/// behaviour — instancing a level into a level is legal.
[[nodiscard]] std::vector<std::string> ScanContentPaths();

/// @brief Scans the asset root and hashes it.
///
/// Blocking, and called on the main thread the moment hosting or joining
/// starts, before the session's first step. It has to be done by then: until a
/// host holds the list it cannot name a blueprint on the wire, and an instance
/// spawned in that window, such as the host's own player, would be sent as
/// loose entities for good. It is never triggered by a level load, so a
/// single-player game never pays for it.
///
/// Each file is hashed through Core::HashTextFileNormalized, not by raw bytes:
/// git checks text out as CRLF on Windows, so raw bytes make a Windows checkout
/// and a Linux one of the same file refuse each other. The level hash normalizes
/// for the same reason.
///
/// Enumeration order is *not* stable across machines, so the list is sorted by
/// virtual path before combining — the ordering is part of the format, not an
/// artefact of how the two filesystems happened to walk the directory.
///
/// A file that cannot be read is folded in as a zero hash rather than skipped:
/// skipping it would let two machines with different unreadable files agree.
[[nodiscard]] ContentSet BuildContentSet();

} // namespace Assisi::App
