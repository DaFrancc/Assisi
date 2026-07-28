/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file App/SystemRegistry.hpp
/// @brief Ordered per-phase system registry with dependency-based scheduling.
///
/// Two context types exist depending on the phase:
///   - SystemContext  — game logic phases (PreUpdate, FixedUpdate, Update, PostUpdate)
///   - RenderContext  — render phase only (adds view/projection matrices)
///
/// @par Example
/// @code
/// // Game logic system
/// _systems.Register(SystemPhase::Update, "Damage", &DamageSystem)
///         .After("Physics");
///
/// // Input-consuming system: one InputContext, N resident worlds — running it
/// // everywhere would apply the same keypresses in every world.
/// _systems.Register(SystemPhase::Update, "PlayerMove", &PlayerMoveSystem)
///         .ActiveWorldOnly();
///
/// // Render system — needs view/projection
/// _systems.RegisterRender("DrawScene",
///     [this](RenderContext& ctx) {
///         Runtime::DrawScene(ctx.scene, ctx.view, ctx.projection, _shader);
///     });
///
/// // Dispatch
/// _systems.Run(SystemPhase::Update, {world, dt, &input, &actions, events, isActive});
/// _systems.RunRender({ world.scene, 0.f, view, proj });
/// @endcode
///
/// Systems run in dependency order within each phase.
/// Systems with no ordering relationship run in registration order.

#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/Reflect/ComponentId.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Window/ActionMap.hpp>
#include <Assisi/Window/InputContext.hpp>

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::App
{

struct World;

/// @brief Passed to game logic systems (PreUpdate, FixedUpdate, Update, PostUpdate).
///
/// Carries the **world**, not a bare scene: a system reaches its entities
/// through `ctx.world.scene` and its bodies through `ctx.world.physics`, which
/// is what makes the same system function usable in whichever worlds install it
/// (docs/world-system-binding-design-notes.md §2).
struct SystemContext
{
    World                &world;
    float                 dt;

    /// Null in headless hosts (dedicated server, tests): an InputContext needs a
    /// live window, so anything that can run windowless must be able to say "no
    /// input". Systems that read input either declare ActiveWorldOnly() and get
    /// gated out of such hosts anyway, or null-check.
    Window::InputContext *input;
    Window::ActionMap    *actions;

    Core::EventQueue     &events;

    /// True when `world` is the one the app treats as active — the world being
    /// rendered and driven by input. Systems registered ActiveWorldOnly() are
    /// skipped when this is false; prefer that over testing this by hand.
    bool                  isActiveWorld = true;
};

/// @brief Passed to render systems (Render phase only).
struct RenderContext
{
    ECS::Scene  &scene;
    float        dt;
    glm::mat4    view;
    glm::mat4    projection;
};

/// @brief Execution phase that determines when a system runs and which context it receives.
enum class SystemPhase
{
    PreUpdate   = 0, ///< After input is polled; before physics and game logic.
    FixedUpdate = 1, ///< Fixed timestep; may run multiple times per render frame.
    Update      = 2, ///< Once per render frame; main game logic.
    PostUpdate  = 3, ///< After game logic; transform propagation and cleanup.
    Count
};

// The Render phase is not a SystemPhase value: render systems take a different
// context (RenderContext) and are registered/run through RegisterRender/RunRender.
// A phase argument that could only ever be Render would be an apology, not an API.

/// @brief Stores and dispatches system functions grouped by phase.
///
/// Builds an execution order from After()/Before() constraints on first Run()
/// and caches it.  The cache is invalidated automatically when new systems
/// are registered.
class SystemRegistry
{
  public:
    /// @brief Fluent handle for chaining ordering constraints after registration.
    ///
    /// Type-erased over the context type: it captures where to append the
    /// dependency at registration time, so After()/Before() are defined once
    /// regardless of whether the system is a game or render system.
    class SystemHandle
    {
      public:
        /// @brief This system runs after the named system within the same phase.
        SystemHandle &After(std::string_view name);

        /// @brief This system runs before the named system within the same phase.
        SystemHandle &Before(std::string_view name);

        /// @brief Skip this system in worlds that are not the active one.
        ///
        /// For anything that consumes input or drives the one camera/HUD: the app
        /// has a single InputContext but may have several worlds simulating, so an
        /// ungated controller system would apply the same keypresses in all of
        /// them (docs/multi-scene-design-notes.md §1). Meaningless on render
        /// systems, which only ever run for the world being drawn — calling it
        /// there logs an error and changes nothing.
        SystemHandle &ActiveWorldOnly();

        /// @brief Skip this system while the scene holds none of @p Ts.
        ///
        /// What makes it affordable to install a system that a given world may
        /// never need — an open-world profile installs everything, and the
        /// regions that stream in decide what actually runs
        /// (docs/world-system-binding-design-notes.md §5). Idle cost is a couple
        /// of array loads per phase, so frame cost tracks resident entities
        /// rather than how many systems were registered.
        ///
        /// "Any", not "all": a system reading Water OR Lava wants to run when
        /// either is present. Declare only what the system cannot work without —
        /// a component it merely writes to a few entities is not a gate.
        template <typename... Ts> SystemHandle &RequireAny()
        {
            static_assert(sizeof...(Ts) > 0, "RequireAny<> needs at least one component type.");
            (Require(Core::Reflect::ComponentIdOf<Ts>()), ...);
            return *this;
        }

      private:
        friend class SystemRegistry;

        /// Records a dependency: @p before selects the before-list over the after-list.
        using AddDependency = std::function<void(bool before, std::string_view name)>;
        /// Marks the entry active-world-only. Null for render-phase handles.
        using SetActiveOnly = std::function<void()>;
        /// Appends one component id to the entry's activation gate.
        using AddRequirement = std::function<void(Core::Reflect::ComponentId)>;

        /// Non-template half of RequireAny, so the fold above stays a one-liner.
        void Require(Core::Reflect::ComponentId id);

        SystemHandle(AddDependency addDependency, SetActiveOnly setActiveOnly,
                     AddRequirement addRequirement)
            : _addDependency(std::move(addDependency))
            , _setActiveOnly(std::move(setActiveOnly))
            , _addRequirement(std::move(addRequirement))
        {
        }

        AddDependency  _addDependency;
        SetActiveOnly  _setActiveOnly;
        AddRequirement _addRequirement;
    };

    /// @brief Register a game logic system for a non-Render phase.
    SystemHandle Register(SystemPhase                          phase,
                          std::string_view                     name,
                          std::function<void(SystemContext &)> fn);

    /// @brief Register a render system (runs in the Render phase, receives a RenderContext).
    SystemHandle RegisterRender(std::string_view name, std::function<void(RenderContext &)> fn);

    /// @brief Run all game logic systems for the given phase in dependency order.
    void Run(SystemPhase phase, SystemContext ctx);

    /// @brief Run all render systems in dependency order.
    void RunRender(RenderContext ctx);

    /// @brief Whether any render system has been registered. Lets a host that
    /// never calls RunRender (the editor owns rendering) warn instead of
    /// silently dropping them.
    [[nodiscard]] bool HasRenderSystems() const { return !_renderPhase.entries.empty(); }

    /// @brief Drops every registered system, in every phase.
    ///
    /// Registration is otherwise append-only, and re-registering a name corrupts
    /// the ordering graph (After()/Before() bind to the first entry of a name).
    /// So a world whose systems are being *re-targeted* — the editor opening a
    /// different level into the world it already edits — must clear before
    /// applying the incoming level's profile, never stack one on the other.
    ///
    /// Handles returned by earlier Register() calls are dead afterwards: they
    /// address slots that no longer exist. Registering fresh systems hands back
    /// fresh handles, which is the only supported order.
    void Clear();

  private:
    /// @brief One phase's worth of systems taking context type @p Ctx, plus its
    /// cached execution order.  Game and render phases are the same machinery
    /// differing only in Ctx — this template is what collapses the duplication.
    template <typename Ctx>
    struct Phase
    {
        struct Entry
        {
            std::string               name;
            std::function<void(Ctx &)> fn;
            std::vector<std::string>  after;
            std::vector<std::string>  before;
            /// Set by SystemHandle::ActiveWorldOnly(). Always false for render
            /// entries, which only ever run for the world being drawn.
            bool                      activeOnly = false;
            /// Set by SystemHandle::RequireAny(). Empty means "always eligible";
            /// otherwise the system runs only while the scene holds at least one
            /// of these components.
            std::vector<Core::Reflect::ComponentId> requireAny;
        };

        std::vector<Entry>       entries;
        std::vector<std::size_t> sorted; ///< Indices into @ref entries, in execution order.
        bool                     dirty = false;
    };

    /// Number of game-logic phases (everything except Render).
    static constexpr std::size_t kGamePhaseCount = static_cast<std::size_t>(SystemPhase::Count);

    static std::size_t      Index(SystemPhase phase) { return static_cast<std::size_t>(phase); }
    static std::string_view PhaseName(std::size_t gamePhaseIndex);

    /// @brief Append a system to @p phase and return a handle bound to its slot.
    /// @p supportsActiveOnly is false for the render phase, whose handles reject
    /// ActiveWorldOnly().
    template <typename Ctx>
    SystemHandle Add(Phase<Ctx> &phase, std::string_view name, std::function<void(Ctx &)> fn,
                     bool supportsActiveOnly);

    /// @brief Topological sort (Kahn's algorithm) over any entry type with name/after/before.
    template <typename Entry>
    static std::vector<std::size_t> TopoSort(const std::vector<Entry> &entries,
                                             std::string_view          phaseName);

    /// @brief Re-sort @p phase if dirty, then run its systems in dependency order,
    /// omitting active-world-only entries when @p skipActiveOnly and entries whose
    /// RequireAny components are all absent from @p gateScene.
    template <typename Ctx>
    void RunPhase(Phase<Ctx> &phase, std::string_view phaseName, Ctx &ctx, bool skipActiveOnly,
                  const ECS::Scene &gateScene);

    std::array<Phase<SystemContext>, kGamePhaseCount> _gamePhases;
    Phase<RenderContext>                              _renderPhase;
};

} // namespace Assisi::App