/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <Assisi/Window/ActionMap.hpp>
#include <Assisi/Window/Key.hpp>

using namespace Assisi::Window;

TEST_CASE("ActionMap: key name <-> enum round-trips")
{
    CHECK(ActionMap::KeyName(Key::W) == "W");
    CHECK(ActionMap::KeyFromName("W") == Key::W);
    CHECK(ActionMap::KeyFromName("Space") == Key::Space);
    CHECK_FALSE(ActionMap::KeyFromName("NotAKey").has_value());

    CHECK(ActionMap::MouseButtonName(MouseButton::Left) == "Left");
    CHECK(ActionMap::MouseButtonFromName("Left") == MouseButton::Left);
    CHECK_FALSE(ActionMap::MouseButtonFromName("Nope").has_value());
}

TEST_CASE("ActionMap: JSON survives a ToJson -> LoadFromJson round-trip")
{
    ActionMap original;
    original.Bind("Jump", Key::Space);
    original.Bind("MoveForward", Key::W);
    original.Bind("MoveForward", Key::Up); // second binding on the same action
    original.Bind("Fire", MouseButton::Left);

    const nlohmann::json j = original.ToJson();

    ActionMap loaded;
    loaded.LoadFromJson(j);

    const auto &actions = loaded.GetAllActions();
    REQUIRE(actions.size() == 3);
    CHECK(loaded.GetBindings("Jump").size() == 1);
    CHECK(loaded.GetBindings("MoveForward").size() == 2);
    CHECK(loaded.GetBindings("Fire").size() == 1);

    // Round-tripped JSON must be identical to the original's serialization.
    CHECK(loaded.ToJson() == j);
}

TEST_CASE("ActionMap: unknown key/button names are skipped on load")
{
    const nlohmann::json j = {
        {"Good", {{{"key", "Space"}}}},
        {"BadKey", {{{"key", "Nonexistent"}}}},
        {"BadButton", {{{"button", "Nonexistent"}}}},
    };

    ActionMap map;
    map.LoadFromJson(j);

    CHECK(map.GetBindings("Good").size() == 1);
    CHECK(map.GetBindings("BadKey").empty());
    CHECK(map.GetBindings("BadButton").empty());
}

TEST_CASE("ActionMap: querying an unregistered action returns no bindings")
{
    ActionMap map;
    CHECK(map.GetBindings("DoesNotExist").empty());
}
