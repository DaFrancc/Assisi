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
/// _systems.Register(SystemPhase::Render, "DrawScene",
///     [this](RenderContext& ctx) {
///         Runtime::DrawScene(ctx.scene, ctx.view, ctx.projection, _shader);
///     });
///
/// // Dispatch
/// _systems.Run(SystemPhase::Update,  { *_scene, dt,  GetInput() });
/// _systems.Run(SystemPhase::Render,  { *_scene, 0.f, view, proj });
/// @endcode
///
/// Systems run in dependency order within each phase.
/// Systems with no ordering relationship run in registration order.

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Math/GLM.hpp>
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
    ECS::Scene           &scene;
    float                 dt;
    Window::InputContext &input;
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
    Render      = 4, ///< Camera, culling, draw calls.  Uses RenderContext.
    _Count
};

/// @brief Stores and dispatches system functions grouped by phase.
///
/// Builds an execution order from After()/Before() constraints on first Run()
/// and caches it.  The cache is invalidated automatically when new systems
/// are registered.
class SystemRegistry
{
  public:
    /// @brief Fluent handle for chaining ordering constraints after Register().
    class SystemHandle
    {
      public:
        /// @brief This system runs after the named system within the same phase.
        SystemHandle &After(std::string_view name);

        /// @brief This system runs before the named system within the same phase.
        SystemHandle &Before(std::string_view name);

      private:
        friend class SystemRegistry;

        SystemHandle(SystemRegistry *registry, bool isRender, std::size_t phaseIndex,
                     std::size_t entryIndex)
            : _registry(registry)
            , _isRender(isRender)
            , _phaseIndex(phaseIndex)
            , _entryIndex(entryIndex)
        {
        }

        SystemRegistry *_registry;
        bool            _isRender;
        std::size_t     _phaseIndex; ///< Unused when _isRender == true.
        std::size_t     _entryIndex; ///< Index into the entry vector — stable across push_backs.
    };

    /// @brief Register a game logic system for a non-Render phase.
    SystemHandle Register(SystemPhase                          phase,
                          std::string_view                     name,
                          std::function<void(SystemContext &)> fn);

    /// @brief Register a render system.  @p phase must be SystemPhase::Render.
    SystemHandle Register(SystemPhase                          phase,
                          std::string_view                     name,
                          std::function<void(RenderContext &)> fn);

    /// @brief Run all game logic systems for the given phase in dependency order.
    void Run(SystemPhase phase, SystemContext ctx);

    /// @brief Run all render systems in dependency order.  @p phase must be SystemPhase::Render.
    void Run(SystemPhase phase, RenderContext ctx);

  private:
    struct GameEntry
    {
        std::string                        name;
        std::function<void(SystemContext &)> fn;
        std::vector<std::string>           after;
        std::vector<std::string>           before;
    };

    struct RenderEntry
    {
        std::string                        name;
        std::function<void(RenderContext &)> fn;
        std::vector<std::string>           after;
        std::vector<std::string>           before;
    };

    /// Number of game-logic phases (everything except Render).
    static constexpr std::size_t kGamePhaseCount = static_cast<std::size_t>(SystemPhase::Render);

    static std::size_t Index(SystemPhase phase) { return static_cast<std::size_t>(phase); }

    /// @brief Topological sort (Kahn's algorithm) over any entry type with name/after/before.
    template <typename Entry>
    static std::vector<std::size_t> TopoSort(const std::vector<Entry> &entries,
                                             std::string_view          phaseName);

    void SortPhase(std::size_t phaseIndex);
    void SortRender();

    std::array<std::vector<GameEntry>, kGamePhaseCount>  _entries;
    std::array<std::vector<std::size_t>, kGamePhaseCount> _sorted;
    std::array<bool, kGamePhaseCount>                     _dirty{};

    std::vector<RenderEntry>  _renderEntries;
    std::vector<std::size_t>  _renderSorted;
    bool                      _renderDirty = false;
};

} // namespace Assisi::App