/// @file ActionMap.cpp

#include <Assisi/Window/ActionMap.hpp>

#include <Assisi/Core/Logger.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>

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
    Key              key;
};

static constexpr std::array kKeyTable{
    KeyEntry{"Space",        Key::Space},
    KeyEntry{"Apostrophe",   Key::Apostrophe},
    KeyEntry{"Comma",        Key::Comma},
    KeyEntry{"Minus",        Key::Minus},
    KeyEntry{"Period",       Key::Period},
    KeyEntry{"Slash",        Key::Slash},
    KeyEntry{"Num0",         Key::Num0},
    KeyEntry{"Num1",         Key::Num1},
    KeyEntry{"Num2",         Key::Num2},
    KeyEntry{"Num3",         Key::Num3},
    KeyEntry{"Num4",         Key::Num4},
    KeyEntry{"Num5",         Key::Num5},
    KeyEntry{"Num6",         Key::Num6},
    KeyEntry{"Num7",         Key::Num7},
    KeyEntry{"Num8",         Key::Num8},
    KeyEntry{"Num9",         Key::Num9},
    KeyEntry{"Semicolon",    Key::Semicolon},
    KeyEntry{"Equal",        Key::Equal},
    KeyEntry{"A",            Key::A},
    KeyEntry{"B",            Key::B},
    KeyEntry{"C",            Key::C},
    KeyEntry{"D",            Key::D},
    KeyEntry{"E",            Key::E},
    KeyEntry{"F",            Key::F},
    KeyEntry{"G",            Key::G},
    KeyEntry{"H",            Key::H},
    KeyEntry{"I",            Key::I},
    KeyEntry{"J",            Key::J},
    KeyEntry{"K",            Key::K},
    KeyEntry{"L",            Key::L},
    KeyEntry{"M",            Key::M},
    KeyEntry{"N",            Key::N},
    KeyEntry{"O",            Key::O},
    KeyEntry{"P",            Key::P},
    KeyEntry{"Q",            Key::Q},
    KeyEntry{"R",            Key::R},
    KeyEntry{"S",            Key::S},
    KeyEntry{"T",            Key::T},
    KeyEntry{"U",            Key::U},
    KeyEntry{"V",            Key::V},
    KeyEntry{"W",            Key::W},
    KeyEntry{"X",            Key::X},
    KeyEntry{"Y",            Key::Y},
    KeyEntry{"Z",            Key::Z},
    KeyEntry{"Escape",       Key::Escape},
    KeyEntry{"Enter",        Key::Enter},
    KeyEntry{"Tab",          Key::Tab},
    KeyEntry{"Backspace",    Key::Backspace},
    KeyEntry{"Insert",       Key::Insert},
    KeyEntry{"Delete",       Key::Delete},
    KeyEntry{"Right",        Key::Right},
    KeyEntry{"Left",         Key::Left},
    KeyEntry{"Down",         Key::Down},
    KeyEntry{"Up",           Key::Up},
    KeyEntry{"F1",           Key::F1},
    KeyEntry{"F2",           Key::F2},
    KeyEntry{"F3",           Key::F3},
    KeyEntry{"F4",           Key::F4},
    KeyEntry{"F5",           Key::F5},
    KeyEntry{"F6",           Key::F6},
    KeyEntry{"F7",           Key::F7},
    KeyEntry{"F8",           Key::F8},
    KeyEntry{"F9",           Key::F9},
    KeyEntry{"F10",          Key::F10},
    KeyEntry{"F11",          Key::F11},
    KeyEntry{"F12",          Key::F12},
    KeyEntry{"LeftShift",    Key::LeftShift},
    KeyEntry{"LeftControl",  Key::LeftControl},
    KeyEntry{"LeftAlt",      Key::LeftAlt},
    KeyEntry{"RightShift",   Key::RightShift},
    KeyEntry{"RightControl", Key::RightControl},
    KeyEntry{"RightAlt",     Key::RightAlt},
};

struct ButtonEntry
{
    std::string_view name;
    MouseButton      button;
};

static constexpr std::array kButtonTable{
    ButtonEntry{"Left",   MouseButton::Left},
    ButtonEntry{"Right",  MouseButton::Right},
    ButtonEntry{"Middle", MouseButton::Middle},
};

} // namespace

// ---------------------------------------------------------------------------
// ActionBinding
// ---------------------------------------------------------------------------

bool ActionBinding::IsDown(const InputContext &ctx) const noexcept
{
    return std::visit(
        [&](auto v)
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Key>)
                return ctx.IsKeyDown(v);
            else
                return ctx.IsMouseButtonDown(v);
        },
        input);
}

bool ActionBinding::IsPressed(const InputContext &ctx) const noexcept
{
    return std::visit(
        [&](auto v)
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Key>)
                return ctx.IsKeyPressed(v);
            else
                return ctx.IsMouseButtonPressed(v);
        },
        input);
}

bool ActionBinding::IsReleased(const InputContext &ctx) const noexcept
{
    return std::visit(
        [&](auto v)
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Key>)
                return ctx.IsKeyReleased(v);
            else
                return ctx.IsMouseButtonReleased(v);
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
    _actions.erase(std::string(action));
}

void ActionMap::Clear()
{
    _actions.clear();
}

// ---------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------

bool ActionMap::IsActionDown(std::string_view action, const InputContext &input) const
{
    const auto it = _actions.find(std::string(action));
    if (it == _actions.end())
        return false;
    return std::ranges::any_of(it->second, [&](const ActionBinding &b) { return b.IsDown(input); });
}

bool ActionMap::IsActionPressed(std::string_view action, const InputContext &input) const
{
    const auto it = _actions.find(std::string(action));
    if (it == _actions.end())
        return false;
    return std::ranges::any_of(it->second,
                               [&](const ActionBinding &b) { return b.IsPressed(input); });
}

bool ActionMap::IsActionReleased(std::string_view action, const InputContext &input) const
{
    const auto it = _actions.find(std::string(action));
    if (it == _actions.end())
        return false;
    return std::ranges::any_of(it->second,
                               [&](const ActionBinding &b) { return b.IsReleased(input); });
}

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------

void ActionMap::LoadFromJson(const nlohmann::json &j)
{
    if (!j.is_object())
    {
        Core::Log::Warn("ActionMap::LoadFromJson: expected a JSON object, skipping.");
        return;
    }

    for (const auto &[actionName, bindingsJson] : j.items())
    {
        if (!bindingsJson.is_array())
            continue;

        for (const auto &entry : bindingsJson)
        {
            if (entry.contains("key"))
            {
                const auto keyStr = entry.at("key").get<std::string>();
                const auto key    = KeyFromName(keyStr);
                if (key)
                    Bind(actionName, *key);
                else
                    Core::Log::Warn("ActionMap: unknown key name '{}' in action '{}' — skipped.",
                                    keyStr, actionName);
            }
            else if (entry.contains("button"))
            {
                const auto btnStr = entry.at("button").get<std::string>();
                const auto button = MouseButtonFromName(btnStr);
                if (button)
                    Bind(actionName, *button);
                else
                    Core::Log::Warn(
                        "ActionMap: unknown button name '{}' in action '{}' — skipped.", btnStr,
                        actionName);
            }
        }
    }
}

nlohmann::json ActionMap::ToJson() const
{
    auto j = nlohmann::json::object();
    for (const auto &[name, bindings] : _actions)
    {
        auto arr = nlohmann::json::array();
        for (const auto &b : bindings)
        {
            nlohmann::json entry;
            std::visit(
                [&](auto v)
                {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, Key>)
                        entry["key"] = std::string(KeyName(v));
                    else
                        entry["button"] = std::string(MouseButtonName(v));
                },
                b.input);
            arr.push_back(entry);
        }
        j[name] = arr;
    }
    return j;
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

const std::vector<ActionBinding> &ActionMap::GetBindings(std::string_view action) const
{
    const auto it = _actions.find(std::string(action));
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