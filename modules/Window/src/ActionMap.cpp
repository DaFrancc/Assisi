/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file ActionMap.cpp

#include <Assisi/Window/ActionMap.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Window/InputBindings.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace Assisi::Window
{

// ---------------------------------------------------------------------------
// Static tables — Key name ↔ enum
// ---------------------------------------------------------------------------

namespace
{

struct KeyEntry
{
    std::string_view name;
    Key key;
};

static constexpr std::array kKeyTable{
    KeyEntry{"Space", Key::Space},
    KeyEntry{"Apostrophe", Key::Apostrophe},
    KeyEntry{"Comma", Key::Comma},
    KeyEntry{"Minus", Key::Minus},
    KeyEntry{"Period", Key::Period},
    KeyEntry{"Slash", Key::Slash},
    KeyEntry{"Num0", Key::Num0},
    KeyEntry{"Num1", Key::Num1},
    KeyEntry{"Num2", Key::Num2},
    KeyEntry{"Num3", Key::Num3},
    KeyEntry{"Num4", Key::Num4},
    KeyEntry{"Num5", Key::Num5},
    KeyEntry{"Num6", Key::Num6},
    KeyEntry{"Num7", Key::Num7},
    KeyEntry{"Num8", Key::Num8},
    KeyEntry{"Num9", Key::Num9},
    KeyEntry{"Semicolon", Key::Semicolon},
    KeyEntry{"Equal", Key::Equal},
    KeyEntry{"A", Key::A},
    KeyEntry{"B", Key::B},
    KeyEntry{"C", Key::C},
    KeyEntry{"D", Key::D},
    KeyEntry{"E", Key::E},
    KeyEntry{"F", Key::F},
    KeyEntry{"G", Key::G},
    KeyEntry{"H", Key::H},
    KeyEntry{"I", Key::I},
    KeyEntry{"J", Key::J},
    KeyEntry{"K", Key::K},
    KeyEntry{"L", Key::L},
    KeyEntry{"M", Key::M},
    KeyEntry{"N", Key::N},
    KeyEntry{"O", Key::O},
    KeyEntry{"P", Key::P},
    KeyEntry{"Q", Key::Q},
    KeyEntry{"R", Key::R},
    KeyEntry{"S", Key::S},
    KeyEntry{"T", Key::T},
    KeyEntry{"U", Key::U},
    KeyEntry{"V", Key::V},
    KeyEntry{"W", Key::W},
    KeyEntry{"X", Key::X},
    KeyEntry{"Y", Key::Y},
    KeyEntry{"Z", Key::Z},
    KeyEntry{"Escape", Key::Escape},
    KeyEntry{"Enter", Key::Enter},
    KeyEntry{"Tab", Key::Tab},
    KeyEntry{"Backspace", Key::Backspace},
    KeyEntry{"Insert", Key::Insert},
    KeyEntry{"Delete", Key::Delete},
    // The arrows carry the suffix because a binding name space covers keys and
    // mouse buttons together, and bare Left/Right are both.
    KeyEntry{"RightArrow", Key::Right},
    KeyEntry{"LeftArrow", Key::Left},
    KeyEntry{"DownArrow", Key::Down},
    KeyEntry{"UpArrow", Key::Up},
    KeyEntry{"F1", Key::F1},
    KeyEntry{"F2", Key::F2},
    KeyEntry{"F3", Key::F3},
    KeyEntry{"F4", Key::F4},
    KeyEntry{"F5", Key::F5},
    KeyEntry{"F6", Key::F6},
    KeyEntry{"F7", Key::F7},
    KeyEntry{"F8", Key::F8},
    KeyEntry{"F9", Key::F9},
    KeyEntry{"F10", Key::F10},
    KeyEntry{"F11", Key::F11},
    KeyEntry{"F12", Key::F12},
    KeyEntry{"LeftShift", Key::LeftShift},
    KeyEntry{"LeftControl", Key::LeftControl},
    KeyEntry{"LeftAlt", Key::LeftAlt},
    KeyEntry{"RightShift", Key::RightShift},
    KeyEntry{"RightControl", Key::RightControl},
    KeyEntry{"RightAlt", Key::RightAlt},
};

struct ButtonEntry
{
    std::string_view name;
    MouseButton button;
};

static constexpr std::array kButtonTable{
    ButtonEntry{"LeftMouse", MouseButton::Left},
    ButtonEntry{"RightMouse", MouseButton::Right},
    ButtonEntry{"MiddleMouse", MouseButton::Middle},
};

} // namespace

// ---------------------------------------------------------------------------
// ActionBinding
// ---------------------------------------------------------------------------

bool ActionBinding::IsDown(const InputContext &ctx, ConsumedInput consumed) const noexcept
{
    return std::visit(
        [&](auto v)
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Key>)
                return ctx.IsKeyDown(v, consumed);
            else
                return ctx.IsMouseButtonDown(v, consumed);
        },
        input);
}

bool ActionBinding::IsPressed(const InputContext &ctx, ConsumedInput consumed) const noexcept
{
    return std::visit(
        [&](auto v)
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Key>)
                return ctx.IsKeyPressed(v, consumed);
            else
                return ctx.IsMouseButtonPressed(v, consumed);
        },
        input);
}

bool ActionBinding::IsReleased(const InputContext &ctx, ConsumedInput consumed) const noexcept
{
    return std::visit(
        [&](auto v)
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Key>)
                return ctx.IsKeyReleased(v, consumed);
            else
                return ctx.IsMouseButtonReleased(v, consumed);
        },
        input);
}

uint32_t ActionBinding::TapCount(const InputContext &ctx) const noexcept
{
    return std::visit(
        [&](auto v)
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Key>)
                return ctx.TapCount(v);
            else
                return ctx.ClickCount(v);
        },
        input);
}

// ---------------------------------------------------------------------------
// ActionMap — static data
// ---------------------------------------------------------------------------

const std::vector<ActionBinding> ActionMap::_emptyBindings{};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void ActionMap::Bind(std::string_view action, Key key)
{
    _actions[std::string(action)].push_back(ActionBinding::FromKey(key));
}

void ActionMap::Bind(std::string_view action, MouseButton button)
{
    _actions[std::string(action)].push_back(ActionBinding::FromMouseButton(button));
}

void ActionMap::Unbind(std::string_view action)
{
    // find() takes the heterogeneous (allocation-free) path; erase-by-key
    // would have to materialise a std::string, defeating the table's
    // transparent-lookup design.
    const ActionTable::iterator it = _actions.find(action);
    if (it != _actions.end())
    {
        _actions.erase(it);
    }
}

void ActionMap::Clear()
{
    _actions.clear();
}

// ---------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------

bool ActionMap::IsActionDown(std::string_view action, const InputContext &input, ConsumedInput consumed) const
{
    const auto it = _actions.find(action);
    if (it == _actions.end())
    {
        return false;
    }
    return std::ranges::any_of(it->second, [&](const ActionBinding &b) { return b.IsDown(input, consumed); });
}

bool ActionMap::IsActionPressed(std::string_view action, const InputContext &input, ConsumedInput consumed) const
{
    const auto it = _actions.find(action);
    if (it == _actions.end())
    {
        return false;
    }
    return std::ranges::any_of(it->second, [&](const ActionBinding &b) { return b.IsPressed(input, consumed); });
}

bool ActionMap::IsActionReleased(std::string_view action, const InputContext &input, ConsumedInput consumed) const
{
    const auto it = _actions.find(action);
    if (it == _actions.end())
    {
        return false;
    }
    return std::ranges::any_of(it->second, [&](const ActionBinding &b) { return b.IsReleased(input, consumed); });
}

uint32_t ActionMap::ActionTapCount(std::string_view action, const InputContext &input) const
{
    const auto it = _actions.find(action);
    if (it == _actions.end())
    {
        return 0;
    }
    uint32_t longest = 0;
    for (const ActionBinding &binding : it->second)
    {
        longest = std::max(longest, binding.TapCount(input));
    }
    return longest;
}

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------

void ActionMap::Apply(const InputBindings &bindings)
{
    for (const auto &[actionName, names] : bindings.actions)
    {
        const std::string_view action = actionName.View();

        // Replace this action's bindings rather than adding to them. Apply is
        // called once per layer — the shipped file, then the player's overrides
        // — and a player who rebinds Jump to one key means *only* that key. Any
        // action the override does not name is left exactly as the layer below
        // set it, which is what makes the two calls a merge.
        Unbind(action);

        for (const Assisi::Core::ShortString &name : names)
        {
            const std::optional<ActionBinding> binding = BindingFromName(name.View());
            if (!binding)
            {
                Core::Log::Warn("ActionMap: unknown input name '{}' in action '{}' - skipped.", name.View(), action);
                continue;
            }
            _actions[std::string(action)].push_back(*binding);
        }
    }
}

InputBindings ActionMap::ToBindings() const
{
    InputBindings bindings;
    for (const auto &[name, actionBindings] : _actions)
    {
        std::vector<Assisi::Core::ShortString> &names = bindings.actions[Assisi::Core::ShortString(name)];
        for (const ActionBinding &binding : actionBindings)
        {
            names.emplace_back(BindingName(binding));
        }
    }
    return bindings;
}

std::optional<ActionBinding> ActionMap::BindingFromName(std::string_view name) noexcept
{
    if (const std::optional<Key> key = KeyFromName(name))
    {
        return ActionBinding::FromKey(*key);
    }
    if (const std::optional<MouseButton> button = MouseButtonFromName(name))
    {
        return ActionBinding::FromMouseButton(*button);
    }
    return std::nullopt;
}

std::string_view ActionMap::BindingName(const ActionBinding &binding) noexcept
{
    return std::visit(
        [](auto v)
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Key>)
                return KeyName(v);
            else
                return MouseButtonName(v);
        },
        binding.input);
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

const std::vector<ActionBinding> &ActionMap::GetBindings(std::string_view action) const
{
    const auto it = _actions.find(action);
    return (it != _actions.end()) ? it->second : _emptyBindings;
}

// ---------------------------------------------------------------------------
// Name ↔ enum helpers
// ---------------------------------------------------------------------------

std::string_view ActionMap::KeyName(Key key) noexcept
{
    for (const auto &entry : kKeyTable)
    {
        if (entry.key == key)
            return entry.name;
    }
    return {};
}

std::optional<Key> ActionMap::KeyFromName(std::string_view name) noexcept
{
    for (const auto &entry : kKeyTable)
    {
        if (entry.name == name)
            return entry.key;
    }
    return std::nullopt;
}

std::string_view ActionMap::MouseButtonName(MouseButton button) noexcept
{
    for (const auto &entry : kButtonTable)
    {
        if (entry.button == button)
            return entry.name;
    }
    return {};
}

std::optional<MouseButton> ActionMap::MouseButtonFromName(std::string_view name) noexcept
{
    for (const auto &entry : kButtonTable)
    {
        if (entry.name == name)
            return entry.button;
    }
    return std::nullopt;
}

} // namespace Assisi::Window