/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file BlueprintReplication.hpp
/// @brief The two halves that let NetSync replicate blueprints as blueprints.
///
/// NetSync knows how to carry an instance record and how to bind `base + i` to
/// entities. It deliberately does not know what a blueprint *is* — expansion is
/// Runtime's job, the instance table is World's, and the manifest is App's, none
/// of which NetSync depends on. These are the adapters that supply all three
/// (docs/blueprint-implementation-plan.md, stage 7b R4 / 7c).
///
/// **The manifest is the content set, unmodified.** A blueprint is named on the
/// wire by its index into `ContentSet::paths`, which costs two bytes instead of
/// a path string, and is only sound because a successful join has already proven
/// both machines built the same sorted list. Hand both sides the same vector the
/// handshake hashed — not a rebuilt one, which could differ if a file changed in
/// between.

#include <Assisi/App/ContentSet.hpp>
#include <Assisi/App/World.hpp>

#include <string>
#include <vector>

namespace Assisi::NetSync
{
class NetSession;
class ReplicationServer;
class ReplicationClient;
} // namespace Assisi::NetSync

namespace Assisi::App
{

/// @brief Teach @p server to describe this world's instances.
///
/// Without it every member of every instance replicates as an ordinary entity —
/// correct, and larger.
///
/// @p manifest must be sorted, which is what makes an index name the same file on
/// both machines. An unsorted one installs nothing and says so, leaving exactly
/// that member-by-member behaviour: a binary search over an unsorted list would
/// instead name the wrong file, and the far side would expand it.
void InstallInstanceInfoProvider(NetSync::ReplicationServer &server, World &world,
                                 std::vector<std::string> manifest);

/// @brief Teach @p client to expand an instance record into local members.
///
/// The expansion is the ordinary one — same loader, same composition, same
/// prepared form — which is what makes the two sides agree without the server
/// sending what the file already says.
///
/// Same sortedness contract as InstallInstanceInfoProvider, and for the same
/// reason: this is the side that turns an index back into a file.
void InstallInstanceExpander(NetSync::ReplicationClient &client, World &world,
                             std::vector<std::string> manifest);

/// @brief Hand a finished content-set scan to @p session: its hash, and its paths
/// as the manifest for whichever replication half the session has.
///
/// **The one call a session makes when the scan lands**, and one call rather than
/// two on purpose. The hash and the manifest are the same fact — "this is the
/// content both machines agreed on" — and a caller that sets the hash without
/// installing the manifest leaves blueprint replication with no caller at all.
/// Every session path runs this, windowed or headless; only the drawing differs.
///
/// The hash goes across unconditionally — it is what releases a withheld hello,
/// and refusing a join is not this function's call. The manifest is then subject
/// to the installers' own sortedness contract above.
void ApplyContentSet(NetSync::NetSession &session, World &world, ContentSet content);

} // namespace Assisi::App
