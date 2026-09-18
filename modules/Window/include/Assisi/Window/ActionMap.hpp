/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ActionMap.hpp
/// @brief Named input action mapping — abstracts hardware keys from game logic.
///
/// Define named actions (e.g. "Jump", "MoveForward") and bind them to one or
/// more keyboard keys or mouse buttons.  Game systems query actions by name
/// instead of specific hardware inputs, enabling rebindable controls and a
/// clean injection point for networked input in the future.
///
/// Bindings arrive as an InputBindings, which is what a config file holds:
/// @code
/// {
///   "actions": {
///     "Jump":        ["Space"],
///     "MoveForward": ["W", "UpArrow"],
///     "Fire":        ["LeftMouse"]
///   }
/// }
/// @endcode
/// Keys and mouse buttons share one name space, so a binding is a bare name and
/// nothing says which device it came from. That is why the arrow keys are
/// `LeftArrow`/`RightArrow` and the mouse buttons `LeftMouse`/`RightMouse` — a
/// bare `Left` would be both.
///
/// @par Usage in a system:
/// @code
/// _systems.Register(SystemPhase::Update, "PlayerMove",
///     [](SystemContext& ctx) {
///         if (ctx.actions && ctx.actions->IsActionDown("MoveForward", *ctx.input))
///             player.velocity.z -= speed * ctx.dt;
///     })
///     .ActiveWorldOnly();  // input is single; worlds are many
/// @endcode
/// The null check is not defensive noise: ctx.actions/ctx.input are null on a
/// headless server. A system that must also run there reads its input from
/// replicated commands instead of polling here.

#include <Assisi/Core/StringHash.hpp>
#include <Assisi/Window/InputBindings.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/Key.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace Assisi::Window
{

/// @brief A single hardware input source that can trigger an action.
///
/// Holds either a keyboard Key or a MouseButton.
struct ActionBinding
{
    std::variant<Key, MouseButton> input;

    [[nodiscard]] static ActionBinding FromKey(Key k) noexcept { return {k}; }
    [[nodiscard]] static ActionBinding FromMouseButton(MouseButton b) noexcept { return {b}; }

    /// @brief True while the bound input is held this frame.
    [[nodiscard]] bool IsDown(const InputContext &ctx) const noexcept;

    /// @brief True on the first frame the bound input transitions from up to down.
    [[nodiscard]] bool IsPressed(const InputContext &ctx) const noexcept;

    /// @brief True on the first frame the bound input transitions from down to up.
    [[nodiscard]] bool IsReleased(const InputContext &ctx) const noexcept;
};

/// @brief Action-name → bindings map with heterogeneous (allocation-free) lookup.
using ActionTable = std::unordered_map<std::string, std::vector<ActionBinding>,
                                       Core::TransparentStringHash, std::equal_to<>>;

/// @brief Maps named actions to one or more @ref ActionBinding values.
///
/// Multiple bindings per action are OR-ed: any one binding being active
/// satisfies the query.  Adding a second binding for the same action does not
/// remove existing ones — call Unbind() first if you want to replace them.
class ActionMap
{
public:
    // -------------------------------------------------------------------------
    // Registration
    // -------------------------------------------------------------------------

    /// @brief Bind a keyboard key to the named action (additive).
    void Bind(std::string_view action, Key key);

    /// @brief Bind a mouse button to the named action (additive).
    void Bind(std::string_view action, MouseButton button);

    /// @brief Remove all bindings for the named action.
    void Unbind(std::string_view action);

    /// @brief Remove all actions and bindings.
    void Clear();

    // -------------------------------------------------------------------------
    // Query — delegate to the InputContext polled this frame.
    // Returns false for unregistered action names.
    // -------------------------------------------------------------------------

    /// @brief True while any bound input for the action is held.
    [[nodiscard]] bool IsActionDown(std::string_view action, const InputContext &input) const;

    /// @brief True on the first frame any bound input for the action is pressed.
    [[nodiscard]] bool IsActionPressed(std::string_view action, const InputContext &input) const;

    /// @brief True on the first frame any bound input for the action is released.
    [[nodiscard]] bool IsActionReleased(std::string_view action, const InputContext &input) const;

    // -------------------------------------------------------------------------
    // Serialisation
    // -------------------------------------------------------------------------

    /// @brief Bind every action @p bindings names, replacing that action's
    ///        existing bindings and leaving every other action alone.
    ///
    /// Per-action replacement is what layers a player's overrides over the
    /// shipped defaults: apply the shipped bindings, then apply the overrides,
    /// and an action the player never touched keeps what shipped while one they
    /// rebound holds only what they chose. Adding instead would leave the old
    /// key live alongside the new one; replacing the whole map would delete
    /// every action the override does not mention.
    ///
    /// A name this build does not recognise is skipped with a warning; its
    /// siblings in the same action still bind.
    void Apply(const InputBindings &bindings);

    /// @brief Every binding held here, in the form a config file stores.
    ///        Round-trips through Apply() on a cleared map.
    [[nodiscard]] InputBindings ToBindings() const;

    // -------------------------------------------------------------------------
    // Introspection
    // -------------------------------------------------------------------------

    /// @brief Returns all bindings for the named action, or an empty vector if unregistered.
    [[nodiscard]] const std::vector<ActionBinding> &GetBindings(std::string_view action) const;

    /// @brief Returns the full action → bindings map.
    [[nodiscard]] const ActionTable &GetAllActions() const { return _actions; }

    // -------------------------------------------------------------------------
    // The binding name space — one namespace over keys and mouse buttons, and
    // the only place a config file's spelling is decided.
    // -------------------------------------------------------------------------

    /// @brief Resolve a binding name to the input it names, whichever device
    ///        that is. std::nullopt if this build knows no such input.
    [[nodiscard]] static std::optional<ActionBinding> BindingFromName(std::string_view name) noexcept;

    /// @brief The name @p binding is written as. Empty if unknown.
    [[nodiscard]] static std::string_view BindingName(const ActionBinding &binding) noexcept;

    /// @brief Canonical name for a Key (e.g. Key::W → "W"). Empty if unknown.
    [[nodiscard]] static std::string_view KeyName(Key key) noexcept;

    /// @brief Parse a Key from its name string. std::nullopt if unrecognised.
    [[nodiscard]] static std::optional<Key> KeyFromName(std::string_view name) noexcept;

    /// @brief Canonical name for a MouseButton (e.g. MouseButton::Left → "LeftMouse").
    [[nodiscard]] static std::string_view MouseButtonName(MouseButton button) noexcept;

    /// @brief Parse a MouseButton from its name string. std::nullopt if unrecognised.
    [[nodiscard]] static std::optional<MouseButton> MouseButtonFromName(std::string_view name) noexcept;

private:
    ActionTable _actions;

    static const std::vector<ActionBinding> _emptyBindings;
};

} // namespace Assisi::Window