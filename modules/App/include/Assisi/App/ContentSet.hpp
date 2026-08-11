/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ContentSet.hpp
/// @brief One hash over every level and blueprint file this build can resolve.
///
/// A level's own content hash names *which* level. This names the whole set, and
/// it has to, for a reason that only shows up once blueprints replicate: a
/// blueprint spawned from C++ is named by no level and would never be hashed —
/// while spawning replicates as "expand this file", which makes every blueprint's
/// content load-bearing across the wire (docs/blueprint-system-concept.md §9).
///
/// The check is deliberately strict. *Any* difference refuses a join, including
/// files neither machine ever loads, because a stray experimental `.abp` is
/// indistinguishable to the hash from a car whose wheels moved. That is a
/// development-time cost and never a shipping one, and it buys the property the
/// rest of the design leans on: after a successful join, both machines are known
/// to expand any blueprint identically, so a blueprint can be named on the wire
/// by its index in the sorted list rather than by path.

#include <Assisi/Core/JobSystem.hpp>

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
/// Blocking, and deliberately not called on the main thread: run it as a job
/// (Core::Jobs::Pool::Worker) at the moment hosting or joining starts. It is never
/// triggered by a level load, so a single-player game never pays for it.
///
/// Each file is hashed through Core::HashTextFileNormalized, not by raw bytes.
/// The level hash already had to learn this the hard way (`562aa5d`): a Windows
/// laptop and a Linux desktop refused each other over 439 CR bytes, because git
/// checks text out as CRLF on Windows. Blueprints are JSON text and inherit the
/// trap exactly.
///
/// Enumeration order is *not* stable across machines, so the list is sorted by
/// virtual path before combining — the ordering is part of the format, not an
/// artefact of how the two filesystems happened to walk the directory.
///
/// A file that cannot be read is folded in as a zero hash rather than skipped:
/// skipping it would let two machines with different unreadable files agree.
[[nodiscard]] ContentSet BuildContentSet();

/// @brief A content-set hash being computed off the main thread, and the poll
/// that delivers it once.
///
/// **Hashing is asynchronous**, and triggered by hosting or joining — never by a
/// level load, so a single-player game never pays for it. A client that tries to
/// connect before its scan finishes joins in appearance only: the connection
/// waits on the hash and completes when it arrives, and the server cannot be
/// reached without one, so there is no race to lose (§9).
class ContentSetHashJob
{
  public:
    /// @brief Kicks the scan, unless one is already running or has landed.
    void Start(Core::JobSystem &jobs);

    /// @brief Delivers the scanned set exactly once, the first poll after it
    /// lands.
    ///
    /// The whole set, not just the hash. `paths` is the manifest blueprint
    /// replication is named by, and it has to be *this* vector rather than one
    /// rebuilt at install time: a file that changed in between would produce a
    /// different list, and the index the far side reads would name a different
    /// file (BlueprintReplication.hpp). Carrying it here is what lets the caller
    /// hand both halves the list the handshake actually hashed.
    ///
    /// @return true (with @p out written) on that one call; false otherwise.
    bool Poll(ContentSet &out);

    /// @brief Whether a scan has been kicked and not yet delivered.
    [[nodiscard]] bool IsRunning() const { return _running; }

    /// @brief Forgets any result so the next Start rescans. Call when leaving a
    /// session: the content on disk may have moved on since.
    void Reset();

  private:
    Core::Task<ContentSet> _task;
    bool                   _running = false;
};

} // namespace Assisi::App
