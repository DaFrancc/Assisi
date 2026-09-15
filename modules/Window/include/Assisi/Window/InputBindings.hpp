/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file InputBindings.hpp
/// @brief The on-disk form of an action map: action name → the inputs that fire it.
///
/// Held apart from ActionMap because the two answer different questions.
/// ActionMap is asked "is Jump down this frame" and stores a decoded Key or
/// MouseButton to answer it fast; this is what a file says, and stores the
/// names, so an input the running build does not recognise survives a load and
/// a save instead of being silently dropped from the player's config.
///
/// A binding name comes from one namespace covering keys and mouse buttons
/// alike — `"W"`, `"Space"`, `"LeftArrow"`, `"LeftMouse"` — so an action is a
/// flat list and nothing has to say which kind of device it meant.
/// ActionMap::BindingFromName owns that namespace.

#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Core/Reflect/AssetDocument.hpp>
#include <Assisi/Core/ShortString.hpp>

#include <expected>
#include <map>
#include <string_view>
#include <vector>

namespace Assisi::Window
{

/// @brief Every action a config file binds, and what it binds each to.
///
/// Ordered rather than hashed: a map written out in memory order encodes
/// differently between two runs of the same binary, which would make re-saving
/// an unchanged config a diff.
AASSET()
struct InputBindings
{
    AFIELD() std::map<Assisi::Core::ShortString, std::vector<Assisi::Core::ShortString>> actions;
};

/// @brief The bindings a build ships, read from the asset root.
///
/// These are defaults. What the player changed lives in the writable user root
/// and is applied over the top — see ActionMap::Apply, which is where the two
/// layers meet.
[[nodiscard]] std::expected<InputBindings, Core::Reflect::AssetDocumentError>
LoadInputBindings(std::string_view assetPath = "config/input.json");

} // namespace Assisi::Window
