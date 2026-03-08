#pragma once

/// @file ActionMap.hpp
/// @brief Named input action mapping — abstracts hardware keys from game logic.
///
/// Define named actions (e.g. "Jump", "MoveForward") and bind them to one or
/// more keyboard keys or mouse buttons.  Game systems query actions by name
/// instead of specific hardware inputs, enabling rebindable controls and a
/// clean injection point for networked input in the future.
///
/// Bindings can be loaded from the "input.actions" section of game.json:
/// @code
/// {
///   "input": {
///     "actions": {
///       "Jump":        [{"key": "Space"}],
///       "MoveForward": [{"key": "W"}, {"key": "Up"}],
///       "Fire":        [{"button": "Left"}]
///     }
///   }
/// }
/// @endcode
///
/// @par Usage in a system:
/// @code
/// _systems.Register(SystemPhase::Update, "PlayerMove",
///     [](SystemContext& ctx) {
///         if (ctx.actions.IsActionDown("MoveForward", ctx.input))
///             player.velocity.z -= speed * ctx.dt;
///     });
/// @endcode

#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/Key.hpp>

#include <nlohmann/json_fwd.hpp>

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

    /// @brief Populate bindings from a JSON object.
    ///
    /// @p j must be an object where each key is an action name and each value
    /// is an array of binding objects, e.g.:
    /// @code
    /// { "Jump": [{"key": "Space"}], "Fire": [{"button": "Left"}] }
    /// @endcode
    /// Unknown key/button name strings are skipped with a log warning.
    /// Existing bindings are not cleared before loading.
    void LoadFromJson(const nlohmann::json &j);

    /// @brief Serialise all bindings to a JSON object suitable for round-tripping
    ///        through LoadFromJson().
    [[nodiscard]] nlohmann::json ToJson() const;

    // -------------------------------------------------------------------------
    // Introspection
    // -------------------------------------------------------------------------

    /// @brief Returns all bindings for the named action, or an empty span if unregistered.
    [[nodiscard]] const std::vector<ActionBinding> &GetBindings(std::string_view action) const;

    /// @brief Returns the full action → bindings map.
    [[nodiscard]] const std::unordered_map<std::string, std::vector<ActionBinding>> &
    GetAllActions() const
    {
        return _actions;
    }

    // -------------------------------------------------------------------------
    // Name ↔ enum helpers (used by LoadFromJson / ToJson)
    // -------------------------------------------------------------------------

    /// @brief Canonical name for a Key (e.g. Key::W → "W"). Empty if unknown.
    [[nodiscard]] static std::string_view KeyName(Key key) noexcept;

    /// @brief Parse a Key from its name string. std::nullopt if unrecognised.
    [[nodiscard]] static std::optional<Key> KeyFromName(std::string_view name) noexcept;

    /// @brief Canonical name for a MouseButton (e.g. MouseButton::Left → "Left").
    [[nodiscard]] static std::string_view MouseButtonName(MouseButton button) noexcept;

    /// @brief Parse a MouseButton from its name string. std::nullopt if unrecognised.
    [[nodiscard]] static std::optional<MouseButton> MouseButtonFromName(std::string_view name) noexcept;

  private:
    std::unordered_map<std::string, std::vector<ActionBinding>> _actions;

    static const std::vector<ActionBinding> _emptyBindings;
};

} // namespace Assisi::Window