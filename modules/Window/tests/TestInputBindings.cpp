/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Core/Reflect/AssetDocument.hpp>
#include <Assisi/Window/InputBindings.hpp>

using namespace Assisi::Window;
using Assisi::Core::ShortString;
using Assisi::Core::Reflect::AssetDocumentError;

TEST_CASE("InputBindings: a document fills the action map")
{
    // Two levels of container — a map of action name to a list of input names —
    // read through the generated reflection. This fails if the reflect wiring is
    // missing from the link, which would otherwise show up only as every game
    // launching with no controls.
    InputBindings bindings;
    const auto applied = Assisi::Core::Reflect::ApplyAssetDocument(
        R"({ "version": 1, "type": "InputBindings",
             "actions": { "Jump": ["Space"], "MoveForward": ["W", "UpArrow"] } })",
        bindings);

    REQUIRE(applied.has_value());
    REQUIRE(bindings.actions.size() == 2);
    CHECK(bindings.actions.at(ShortString("Jump")).size() == 1);

    const std::vector<ShortString> &forward = bindings.actions.at(ShortString("MoveForward"));
    REQUIRE(forward.size() == 2);
    CHECK(forward[0].View() == "W");
    CHECK(forward[1].View() == "UpArrow");
}

TEST_CASE("InputBindings: a document survives a serialize -> apply round-trip")
{
    InputBindings written;
    written.actions[ShortString("Fire")]   = {ShortString("LeftMouse")};
    written.actions[ShortString("Strafe")] = {ShortString("A"), ShortString("LeftArrow")};

    const auto text = Assisi::Core::Reflect::SerializeAssetDocument(written);
    REQUIRE(text.has_value());

    InputBindings read;
    REQUIRE(Assisi::Core::Reflect::ApplyAssetDocument(*text, read).has_value());
    CHECK(read.actions == written.actions);
}

TEST_CASE("InputBindings: a document of another asset type is refused entirely")
{
    // Not merely ignored: a wrong-type document must not apply the fields whose
    // names happen to coincide and leave the rest defaulted, because the result
    // is indistinguishable from a corrupt file.
    InputBindings bindings;
    bindings.actions[ShortString("Jump")] = {ShortString("Space")};

    const auto applied = Assisi::Core::Reflect::ApplyAssetDocument(
        R"({ "version": 1, "type": "MaterialData", "actions": { "Fire": ["W"] } })", bindings);

    REQUIRE_FALSE(applied.has_value());
    CHECK(applied.error() == AssetDocumentError::WrongType);
    CHECK(bindings.actions.size() == 1);
    CHECK(bindings.actions.count(ShortString("Jump")) == 1);
}

TEST_CASE("InputBindings: text that will not parse leaves the instance alone")
{
    InputBindings bindings;
    bindings.actions[ShortString("Jump")] = {ShortString("Space")};

    const auto applied = Assisi::Core::Reflect::ApplyAssetDocument("{ not json", bindings);

    REQUIRE_FALSE(applied.has_value());
    CHECK(applied.error() == AssetDocumentError::ParseFailed);
    CHECK(bindings.actions.size() == 1);
}
