/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SystemCatalog.hpp
/// @brief Every system this build declares, by the name a file uses to ask for it.
///
/// **Profiles are gone.** A file lists the individual systems it needs, by name —
/// closer to a module import than an include. Longer, and more straightforward: a
/// profile was a second vocabulary a level had to know, defined somewhere else,
/// and "which systems does profile X install?" was answerable only by reading the
/// game's C++ (docs/blueprint-system-concept.md §8).
///
/// A system is `(phase, name, function, ordering, scope)`, and data can supply
/// only the name — so the definitions live here, and the catalog is *generated*
/// from `ASYSTEM` declarations rather than hand-written. Linking a module
/// registers its systems; there is no registration function to keep in step, and
/// nothing to forget to call.
///
/// Three rules the format depends on:
///
///   - **An unknown name is a hard error at load**, never a warning. A level that
///     names a system this build does not have is a level that will run without
///     it — the exact silent failure the whole design opens with.
///   - **The list is a union, not a concatenation.** Naming a system twice, or
///     two nested blueprints both naming `Bounce`, installs it once.
///   - **File order carries no meaning.** Run order comes from `after`/`before`
///     on the system itself, so a level cannot accidentally reorder anything.

#include <Assisi/App/SystemRegistry.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::App
{

struct World;

/// @brief One system, as its declaration described it.
struct SystemDefinition
{
    /// What a file says to get this system. Defaults to the function name with a
    /// trailing "System" stripped, so `BounceSystem` is `Bounce` in a level.
    std::string name;

    /// Ignored when @ref isRender — a render system runs through RunRender, which
    /// has no phase of its own.
    SystemPhase phase = SystemPhase::Update;
    bool        isRender = false;

    /// Exactly one of these is set, decided by the phase rather than by the
    /// author: reflectgen refuses a Render system that takes a SystemContext, and
    /// vice versa.
    std::function<void(SystemContext &)> run;
    std::function<void(RenderContext &)> runRender;

    std::vector<std::string> after;
    std::vector<std::string> before;

    /// One InputContext, N resident worlds — a system that reads input must not
    /// apply the same keypresses in every one.
    bool activeWorldOnly = false;
};

/// @brief The process-wide table of declared systems.
///
/// Populated by static initializers in the generated OBJECT libraries, which
/// cmake/AssisiReflect.cmake pulls fully into the final link precisely so a
/// registration nobody references still runs — the same path `ACOMP` already
/// rides.
class SystemCatalog
{
  public:
    static SystemCatalog &Instance();

    /// @brief Adds a definition. Called only by generated code.
    void Register(SystemDefinition definition);

    /// @brief The definition @p name refers to, or nullptr.
    [[nodiscard]] const SystemDefinition *Find(std::string_view name) const;

    /// @brief Every declared system, in name order. For the editor's picker and
    /// for diagnostics naming what *is* available when a level names something
    /// that is not.
    [[nodiscard]] std::span<const SystemDefinition> All() const { return _definitions; }

    /// @brief Installs @p names into @p world, skipping any already present.
    ///
    /// Idempotent per name, which is what makes the list a union: two nested
    /// blueprints both naming `Bounce` install it once, and a spawn into a world
    /// that already has it costs a lookup.
    ///
    /// @param context Named in the error message when a name is unknown — the
    ///        level path, or the blueprint that asked.
    /// @return false if any name is unknown. Nothing is installed in that case:
    ///         a half-installed world is worse than a refused load, because it
    ///         runs and looks nearly right.
    bool Install(World &world, std::span<const std::string> names, std::string_view context) const;

  private:
    std::vector<SystemDefinition> _definitions;
};

/// @brief Queues @p names to be installed into @p world at the next frame safe
/// point.
///
/// **Deferral is required, not preferred.** Spawning a blueprint usually happens
/// *inside* a system, and SystemRegistry invalidates its cached execution order on
/// every registration — so registering mid-walk mutates what is being iterated.
/// The installs land at `DrainMain`, where deferred loads and travel already do,
/// before the frame's systems run. The cost is one frame: the car exists
/// immediately and drives from the next.
///
/// Not `FlushDestroyed`: that is a different point, at end of frame, called per
/// host rather than by the engine. Installs want the one that runs before the
/// walk over systems begins.
void QueueSystemInstall(World &world, std::span<const std::string> names, std::string_view context);

/// @brief Applies every queued install. Called by the frame loop at DrainMain;
/// no-op when nothing is queued.
///
/// **Nothing is ever uninstalled.** SystemRegistry::RequireAny already makes an
/// idle system cost a couple of array loads per phase, so there is almost nothing
/// to reclaim, and loading a level flushes everything anyway. Revisit if
/// streaming ever arrives.
void DrainSystemInstalls();

/// @brief True when every system the level at @p virtualPath names is declared
/// by this build. Logs each offender.
///
/// Reads the file's system list *without* loading it, so a caller can refuse
/// before replacing a scene it cannot put back. Every load path wants this,
/// including the headless server: a dedicated server that installs no systems
/// still must not accept a file naming behaviour this build cannot supply —
/// silently serving a level it cannot run is the same failure as the editor
/// silently opening one.
///
/// A file that cannot be read or parsed is *not* declared valid: the caller
/// gets false and the read logs why.
[[nodiscard]] bool LevelSystemsAreDeclared(std::string_view virtualPath);

/// @brief Drops every queued install without applying it. For a world that is
/// about to be destroyed, whose queue entries would otherwise name freed memory.
void CancelSystemInstalls(const World &world);

} // namespace Assisi::App
