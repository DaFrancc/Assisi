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
/// // Render system — needs view/projection
/// _systems.RegisterRender("DrawScene",
///     [this](RenderContext& ctx) {
///         Runtime::DrawScene(ctx.scene, ctx.view, ctx.projection, _shader);
///     });
///
/// // Dispatch
/// _systems.Run(SystemPhase::Update, { *_scene, dt, GetInput() });
/// _systems.RunRender({ *_scene, 0.f, view, proj });
/// @endcode
///
/// Systems run in dependency order within each phase.
/// Systems with no ordering relationship run in registration order.

#include <Assisi/Core/EventQueue.hpp>
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

/// @brief Passed to game logic systems (PreUpdate, FixedUpdate, Update, PostUpdate).
struct SystemContext
{
    ECS::Scene            &scene;
    float                  dt;
    Window::InputContext  &input;
    Window::ActionMap     &actions;
    Core::EventQueue      &events;
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
    _Count
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

      private:
        friend class SystemRegistry;

        /// Records a dependency: @p before selects the before-list over the after-list.
        using AddDependency = std::function<void(bool before, std::string_view name)>;

        explicit SystemHandle(AddDependency addDependency)
            : _addDependency(std::move(addDependency))
        {
        }

        AddDependency _addDependency;
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
        };

        std::vector<Entry>       entries;
        std::vector<std::size_t> sorted; ///< Indices into @ref entries, in execution order.
        bool                     dirty = false;
    };

    /// Number of game-logic phases (everything except Render).
    static constexpr std::size_t kGamePhaseCount = static_cast<std::size_t>(SystemPhase::_Count);

    static std::size_t      Index(SystemPhase phase) { return static_cast<std::size_t>(phase); }
    static std::string_view PhaseName(std::size_t gamePhaseIndex);

    /// @brief Append a system to @p phase and return a handle bound to its slot.
    template <typename Ctx>
    SystemHandle Add(Phase<Ctx> &phase, std::string_view name, std::function<void(Ctx &)> fn);

    /// @brief Topological sort (Kahn's algorithm) over any entry type with name/after/before.
    template <typename Entry>
    static std::vector<std::size_t> TopoSort(const std::vector<Entry> &entries,
                                             std::string_view          phaseName);

    /// @brief Re-sort @p phase if dirty, then run its systems in dependency order.
    template <typename Ctx>
    void RunPhase(Phase<Ctx> &phase, std::string_view phaseName, Ctx &ctx);

    std::array<Phase<SystemContext>, kGamePhaseCount> _gamePhases;
    Phase<RenderContext>                              _renderPhase;
};

} // namespace Assisi::App