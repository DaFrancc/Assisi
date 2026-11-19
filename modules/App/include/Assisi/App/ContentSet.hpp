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
///
/// A game running from its package has no asset root. Once it names its package
/// with SetContentPackage, the set lists the package's levels and blueprints and
/// hashes each by the hash its pak index records for the cooked bytes. Those are
/// not the hashes of the source text, so a packaged game agrees with another
/// running the same package and never with a build running from its asset tree.

#include <cstdint>
#include <string>
#include <vector>

namespace Assisi::Core
{
class PakProvider;
}

namespace Assisi::App
{

/// @brief The result of scanning the asset root, or the package.
struct ContentSet
{
    /// Every `.alvl` and `.abp` the build holds, as virtual paths, sorted.
    /// The index into this is how a blueprint is named on the wire — two bytes,
    /// no path string, no per-file hash — which only works because a successful
    /// join has already proven both machines built the same list.
    std::vector<std::string> paths;

    /// FNV-1a over `(path, contentHash)` for every entry in order.
    std::uint64_t hash = 0;
};

/// @brief Reads content from @p package instead of the asset root, or from the
///        asset root again when null.
///
/// A game calls this once its package is mounted, and with null before the
/// package goes. Everything that asks what content this build holds goes through
/// it: the content set, and the level hash a host advertises and a joiner checks.
void SetContentPackage(const Core::PakProvider *package);

/// @brief The package SetContentPackage named, or null when this build reads its
///        asset root.
[[nodiscard]] const Core::PakProvider *ContentPackage();

/// @brief Every `.alvl` and `.abp` this build holds, as sorted virtual paths.
///
/// The listing half of BuildContentSet, split out because the editor's blueprint
/// panel wants the names without paying to hash every file to get them. Both
/// extensions, because they are one format and the extension never gates
/// behaviour — instancing a level into a level is legal.
[[nodiscard]] std::vector<std::string> ScanContentPaths();

/// @brief Scans the asset root, or the package, and hashes what it holds.
///
/// Blocking, and called on the main thread the moment hosting or joining
/// starts, before the session's first step. It has to be done by then: until a
/// host holds the list it cannot name a blueprint on the wire, and an instance
/// spawned in that window, such as the host's own player, would be sent as
/// loose entities for good. It is never triggered by a level load, so a
/// single-player game never pays for it.
///
/// Each source file is hashed through Core::HashTextFileNormalized, not by raw
/// bytes: git checks text out as CRLF on Windows, so raw bytes make a Windows
/// checkout and a Linux one of the same file refuse each other. The level hash
/// normalizes for the same reason. A packaged file is hashed by its pak index's
/// content hash, which costs no read at all.
///
/// Enumeration order is *not* stable across machines, so the list is sorted by
/// virtual path before combining — the ordering is part of the format, not an
/// artefact of how the two filesystems happened to walk the directory.
///
/// A file that cannot be read is folded in as a zero hash rather than skipped:
/// skipping it would let two machines with different unreadable files agree.
[[nodiscard]] ContentSet BuildContentSet();

} // namespace Assisi::App
