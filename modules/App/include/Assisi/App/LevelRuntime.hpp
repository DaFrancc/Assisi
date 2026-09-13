/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file LevelRuntime.hpp
/// @brief Turns serialized level data into a working scene: asset resolution +
///        physics body rebuild, composed over the modules that own each half.
///
/// SceneSerializer deserializes component *data*; Runtime::AssetResolve turns
/// mesh/material GUIDs into GPU pointers; PhysicsWorld::RebuildSceneBodies
/// turns RigidBodyDescriptors into Jolt bodies. This header is the layer that
/// composes them — it lives in App because App is the only module that links
/// both Runtime and Physics (Runtime deliberately does not link Physics).
///
/// Interim caveat (until asset-database S5, the cooker/PakProvider): a game
/// still builds its GUID→path index by scanning sidecars at startup via
/// AssetDatabase::Rebuild(). Rebuild *mints and writes* a sidecar for any
/// asset that lacks one — write-free only when every asset already has its
/// `.aast`. S5 replaces the scan with a baked index; only the material
/// reconcile (ReconcileMeshMaterials) is genuinely editor-only.

#include <Assisi/Core/AssetDatabase.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/NetSync/NetProtocol.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>
#include <Assisi/Render/AssetCache.hpp>
// The whole of what a LevelResult return needs, and none of SceneSerializer.hpp's
// nlohmann — see that split's own file comment.
#include <Assisi/Runtime/LevelError.hpp>
#include <Assisi/Runtime/SceneRenderer.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Assisi::Runtime
{
// Declared, not included: the definition lives in SceneSerializer.hpp, which
// drags in nlohmann/json — and this header is included (via World.hpp) almost
// everywhere. Callers that build a header include that themselves.
struct LevelHeader;
class InstanceTable;
} // namespace Assisi::Runtime

namespace Assisi::App
{

// Declared, not included, for the same reason as the two above: World.hpp is a
// heavier header than this one wants, and a caller with a World to load into has
// already included it.
struct World;

/// @brief Wires @p database into the two places that translate asset ids:
/// serialization's save-time path hint, and the cache's id↔path resolution
/// (mesh/material/texture loads and glTF import speak GUIDs; reserved
/// built-ins resolve without the database).
///
/// @p database is captured by reference and must outlive the installation
/// (it is engine-lifetime state in practice). Rebuild() mutates the same
/// object, so a re-scan needs no re-install — but calling this again with the
/// same database is harmless.
void InstallAssetResolvers(Render::AssetCache &cache, const Core::AssetDatabase &database);

/// @brief Rebuilds every transient from the scene's durable components, after
/// the scene's entities were replaced wholesale (level load or a play-session
/// restore): re-resolves each MeshRenderer's GPU pointers and rebuilds the
/// physics world from the RigidBodyDescriptors.
void RebindSceneAssetsAndPhysics(ECS::Scene &scene, Render::AssetCache &cache, const Core::AssetDatabase &database,
                                 Physics::PhysicsWorld &physics);

/// @brief What a load should do with the asset cache before resolving.
enum class AssetCacheReset : std::uint8_t
{
    /// Free every cached GPU asset first (and evict the renderer's binding sets)
    /// so the new level starts from a clean cache. Correct only when no OTHER
    /// world is resident — the clear frees resources their resolved pointers
    /// still reference.
    ClearFirst,

    /// Leave the cache alone: the new level's assets are added to whatever is
    /// already there, and assets shared with a resident level are reused rather
    /// than re-uploaded. Required for any load that happens while another world
    /// is alive: the Clear moves out of the load path and becomes a post-travel
    /// sweep.
    Keep,
};

/// @brief The engine-side pieces a load needs beyond the world it loads into.
///
/// References rather than the pointers WorldManager::Services holds, and that is
/// the distinction: that struct is what an *app installs*, and a headless one
/// installs none of the three below. This is what a load with a renderer in play
/// actually requires, so the null check happens once at the boundary that knows
/// (WorldManager::LoadLevel makes it, and takes the render-free path when it
/// fails) instead of at every use inside.
struct LevelServices
{
    Render::AssetCache &cache;
    const Core::AssetDatabase &database;
    Runtime::SceneRenderer &renderer;
};

/// @brief The optional half of a load: what to do with the asset cache, and what
/// the caller wants filled in besides the world itself.
struct LevelLoadOptions
{
    AssetCacheReset reset = AssetCacheReset::ClearFirst;

    /// Receives the level's non-entity metadata — notably the list of systems it
    /// names, which the caller applies to the world it loaded into.
    Runtime::LevelHeader *header = nullptr;
};

/// @brief Loads a level by virtual path (e.g. "levels/Materials.alvl") into
/// @p world and makes it runnable: deserialize into its scene, optionally drop
/// the old asset set and evict the renderer's cached bindings, then rebind
/// assets + physics. On failure it hands back the deserializer's own LevelError
/// rather than a bare bool: the caller's log line is the only place that reason
/// was ever going to reach a person.
///
/// Takes the whole World rather than its scene, physics and instance table
/// separately: a signature that lets the three come from *different* worlds has to
/// be read carefully to see that they don't.
///
/// **A failure does not mean the scene is as you left it.** The deserializer
/// refuses an unreadable file or an unsupported version before touching anything,
/// but every refusal *inside* the load — a duplicate name, an unreadable
/// component, a reference to an entity the file never declares — happens after it
/// has already emptied the scene. LevelFailure::sceneReplaced says which; the
/// error kind alone cannot, and a caller holding entity handles has to look.
/// Note that on the replaced path the physics bodies are *not* rebuilt either,
/// since the rebind below never runs — the caller owns putting that right, e.g.
/// with BuildSceneBodies over whatever it decides the scene now is.
///
/// ## Call this only at a safe point — never mid-frame.
///
/// Clearing the cache frees GPU assets **including the bindless descriptor
/// table** that this frame's already-recorded draws still reference; calling
/// this between BeginFrame and EndFrame faults the GPU. Call it before the
/// frame's draws are recorded — e.g. from OnUpdate, or marshalled to the
/// main-thread drain via Jobs().RunOnMain (which runs just before OnUpdate).
/// A load requested from UI code must be deferred, not applied in place.
///
/// Editor-state bookkeeping (undo-history wipe, selection/eyedropper reset,
/// play-state reset) is deliberately not here — it belongs to the caller that
/// has that state.
[[nodiscard]] Runtime::LevelResult LoadLevel(World &world, std::string_view virtualPath,
                                             const LevelServices &services,
                                             const LevelLoadOptions &options = {});

/// @brief LoadLevel from an absolute filesystem path instead of a virtual one.
///
/// Same safe-point rules, same everything — the failure contract above included,
/// which means a failure here can also be sitting over an emptied scene, and the
/// LevelFailure answers it here too.
///
/// It exists for levels that are not assets: the temp snapshot a play-in-editor
/// host writes so its client processes can load the scene it is actually
/// simulating, unsaved edits included. Asset *references inside* the level still
/// resolve through the asset system as usual; it is only the level file itself
/// that lives outside it.
[[nodiscard]] Runtime::LevelResult LoadLevelFile(World &world, const std::filesystem::path &path,
                                                 const LevelServices &services,
                                                 const LevelLoadOptions &options = {});

/// @brief The simulation half of LoadLevel: deserialize the level and rebuild
/// its physics bodies, with no asset cache and no renderer involved.
///
/// This is what a dedicated server loads. It takes no Render types at all, which
/// is the point — a headless process has no GPU assets to resolve and no binding
/// caches to evict, and asking it for an AssetCache just to throw one away would
/// be a lie about what it needs. Mesh/material GUIDs stay in the scene as
/// authored data (the server replicates them; it never resolves them), so a
/// client joining later gets the same references the level declared.
///
/// The safe-point warning on LoadLevel does not apply here: nothing GPU-owned
/// is freed. Fails with the deserializer's LevelError if the level didn't resolve
/// or deserialize — and, as on LoadLevel, that failure may have emptied the scene
/// on its way out — the same LevelFailure::sceneReplaced says so.
///
/// @note This header still *includes* the Render/Runtime headers for LoadLevel
/// above, so including it does not yet give a caller a render-free dependency
/// footprint — only a render-free call. Untangling the header is part of the
/// App core/presentation split committed to in the networking design notes.
[[nodiscard]] Runtime::LevelResult LoadLevelSim(World &world, std::string_view virtualPath);

/// @brief Per-frame streaming upgrade: while the cache has async loads in
/// flight (and for one frame after the last finishes, to pick up the final
/// result), re-resolves every MeshRenderer so its transient pointers upgrade
/// in place — a null meshBuffer (billboard placeholder) becomes the mesh, the
/// fallback material becomes the real one.
///
/// @p wereLoading is the caller's persistent flag (start it false); the
/// function reads and updates it to implement the one-frame tail. Call once
/// per update tick.
void UpgradeStreamingAssets(ECS::Scene &scene, Render::AssetCache &cache, const Core::AssetDatabase &database,
                            bool &wereLoading);

/// @brief Resolve @p virtualPath and hash it the way every peer must, or nullopt
/// if it cannot be resolved or read.
///
/// One spelling, in the engine, because this number is *compared between
/// machines*: a host and a client that hash the same file differently refuse each
/// other, and the difference need only be as subtle as whether CRLF is folded. A
/// server hashing raw bytes against an editor folding newlines refuses the same
/// file on the same machine, so there is one implementation rather than a copy
/// per caller.
///
/// Lives here rather than in any app because every target needs it: the editor
/// hosting, a dedicated server hosting, and whatever renders a shipped build.
[[nodiscard]] std::optional<std::uint64_t> HashLevelFile(std::string_view virtualPath);

/// @brief Why a joining peer cannot build the level the host named.
enum class JoinLevelError : std::uint8_t
{
    NoLevel,         ///< the host is not running a level file at all
    Unresolvable,    ///< this build has no such file
    Unreadable,      ///< resolved, and could not be read
    ContentMismatch, ///< the local copy differs from the host's
    SystemsMissing,  ///< the file names a system this build does not declare
};

/// @brief A sentence naming @p error for @p path, to show or log.
///
/// Shared so the two do not diverge into "differs from the host's" and "does not
/// match", which is the sort of difference that makes a bug report unsearchable.
[[nodiscard]] std::string JoinLevelErrorMessage(JoinLevelError error, std::string_view path);

/// @brief The file a joining peer should load for @p level, or why it cannot.
///
/// Everything a join must be sure of before it touches the scene: that the host
/// named a level, that this build has it, that the bytes match the host's, and
/// that this build declares the systems the file asks for. Whichever target is
/// joining — editor, dedicated server, or a shipped client — is asking the same
/// question, so it is asked once here.
///
/// Both addressings are handled. A virtual path resolves through the asset
/// system; an absolute one is a play-in-editor snapshot on this machine and is
/// read where it lies.
[[nodiscard]] std::expected<std::filesystem::path, JoinLevelError>
ResolveJoinLevel(const NetSync::LevelIdentity &level);

/// @brief How many entities a join stripped, and how many parent links it had to
/// drop with them.
struct StrippedEntities
{
    std::size_t entities = 0;
    std::size_t orphans  = 0;
};

/// @brief Drop the level's own copies of everything the host owns, after a join
/// has loaded that level locally.
///
/// A joined client loads the same file the host did, so every replicated object
/// exists twice: once as the file's authored copy, once as the mirror arriving on
/// the wire. These are the authored ones.
///
/// Three steps, and the last two are why this is shared rather than written twice.
/// A stripped entity's rigid body has to leave the physics world *before* the
/// entity does, or the body outlives every handle to it. And a child of a stripped
/// entity is left holding a dead parent, which transform propagation reads as a
/// root and places at its local pose — visibly adrift in a windowed client,
/// silently mis-simulated in a headless one.
StrippedEntities StripReplicatedEntities(ECS::Scene &scene, Physics::PhysicsWorld &physics);

} // namespace Assisi::App
