/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Window/ActionMap.hpp>
#include <Assisi/Window/InputBindings.hpp>
#include <Assisi/Window/Key.hpp>

using namespace Assisi::Window;
using Assisi::Core::ShortString;

namespace
{
/// @brief An InputBindings holding one action bound to @p names.
InputBindings OneAction(const char *action, std::vector<ShortString> names)
{
    InputBindings bindings;
    bindings.actions[ShortString(action)] = std::move(names);
    return bindings;
}
} // namespace

TEST_CASE("ActionMap: key name <-> enum round-trips")
{
    CHECK(ActionMap::KeyName(Key::W) == "W");
    CHECK(ActionMap::KeyFromName("W") == Key::W);
    CHECK(ActionMap::KeyFromName("Space") == Key::Space);
    CHECK_FALSE(ActionMap::KeyFromName("NotAKey").has_value());

    CHECK(ActionMap::MouseButtonName(MouseButton::Left) == "LeftMouse");
    CHECK(ActionMap::MouseButtonFromName("LeftMouse") == MouseButton::Left);
    CHECK_FALSE(ActionMap::MouseButtonFromName("Nope").has_value());
}

TEST_CASE("ActionMap: a binding name means one input, whichever device it is on")
{
    // The arrow keys and the mouse buttons carry their suffixes precisely so
    // that one name space can cover both. A bare "Left" resolving to either
    // would mean a config file's meaning depended on lookup order.
    const std::optional<ActionBinding> leftArrow = ActionMap::BindingFromName("LeftArrow");
    REQUIRE(leftArrow.has_value());
    CHECK(std::holds_alternative<Key>(leftArrow->input));
    CHECK(std::get<Key>(leftArrow->input) == Key::Left);

    const std::optional<ActionBinding> leftMouse = ActionMap::BindingFromName("LeftMouse");
    REQUIRE(leftMouse.has_value());
    CHECK(std::holds_alternative<MouseButton>(leftMouse->input));
    CHECK(std::get<MouseButton>(leftMouse->input) == MouseButton::Left);

    CHECK_FALSE(ActionMap::BindingFromName("Left").has_value());
    CHECK_FALSE(ActionMap::BindingFromName("Right").has_value());

    CHECK(ActionMap::BindingName(ActionBinding::FromKey(Key::Left)) == "LeftArrow");
    CHECK(ActionMap::BindingName(ActionBinding::FromMouseButton(MouseButton::Left)) == "LeftMouse");
}

TEST_CASE("ActionMap: bindings survive a ToBindings -> Apply round-trip")
{
    ActionMap original;
    original.Bind("Jump", Key::Space);
    original.Bind("MoveForward", Key::W);
    original.Bind("MoveForward", Key::Up); // second binding on the same action
    original.Bind("Fire", MouseButton::Left);

    const InputBindings written = original.ToBindings();

    ActionMap loaded;
    loaded.Apply(written);

    REQUIRE(loaded.GetAllActions().size() == 3);
    CHECK(loaded.GetBindings("Jump").size() == 1);
    CHECK(loaded.GetBindings("MoveForward").size() == 2);
    CHECK(loaded.GetBindings("Fire").size() == 1);

    // What comes back out must be what went in, or a save after a load would
    // rewrite a file nobody edited.
    CHECK(loaded.ToBindings().actions == written.actions);
}

TEST_CASE("ActionMap: a second Apply replaces the actions it names and leaves the rest")
{
    // The whole of the shipped-default-then-user-override seam. Applying the
    // override must not add to Jump (both keys would stay live), and must not
    // clear MoveForward (an override naming one action would wipe every other).
    ActionMap map;
    InputBindings shipped;
    shipped.actions[ShortString("Jump")]        = {ShortString("Space")};
    shipped.actions[ShortString("MoveForward")] = {ShortString("W"), ShortString("UpArrow")};
    map.Apply(shipped);

    map.Apply(OneAction("Jump", {ShortString("F")}));

    REQUIRE(map.GetBindings("Jump").size() == 1);
    CHECK(std::get<Key>(map.GetBindings("Jump").front().input) == Key::F);
    CHECK(map.GetBindings("MoveForward").size() == 2);
}

TEST_CASE("ActionMap: an unknown name is skipped and its siblings still bind")
{
    ActionMap map;
    map.Apply(OneAction("Move", {ShortString("W"), ShortString("Nonexistent"), ShortString("S")}));

    CHECK(map.GetBindings("Move").size() == 2);
}

TEST_CASE("ActionMap: querying an unregistered action returns no bindings")
{
    ActionMap map;
    CHECK(map.GetBindings("DoesNotExist").empty());
}
