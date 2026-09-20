/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ScreenLoader.hpp
/// @brief A document becomes a screen: one walk over the table, calling the
/// same node API a screen built in C++ calls.
///
/// This is the whole of what markup adds — it is a loader on top of the node
/// API and reaches nothing the API lacks. A screen that a file cannot express
/// is a gap in the node API rather than in the format.
///
/// Every name the document holds is resolved before the screen is built. A
/// screen joins its Ui in its own constructor, so one built and then abandoned
/// part-way would stay there: the check has to come first, not half way
/// through.

#include <Assisi/Mondrian/ScreenDocument.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Core
{
class EventCatalog;
}

namespace Assisi::Mondrian
{

class Screen;
class Ui;

/// @brief Why a document did not become a screen.
enum class ScreenLoadError : uint8_t
{
    /// The table describes no tree this build can walk: no root, or a parent at
    /// or past its own child.
    BadDocument,

    /// A node names an event no catalog entry has. The cook checks the same
    /// thing, so this is a stale package rather than a bad file.
    UnknownEvent,

    /// A node names a control this build does not build from a document. The
    /// element table and this switch grow together.
    UnsupportedWidget,

    /// A node carries something its place cannot hold — a control or an action
    /// on the root, which is the screen itself.
    MisplacedNode,

    Count
};

[[nodiscard]] std::string_view ToString(ScreenLoadError error) noexcept;

/// @brief A screen, and what it asked to have installed.
///
/// The systems travel beside the screen rather than inside it: installing one
/// means reaching a world, and the UI has no route to a world and should not.
/// Whoever owns the world takes this apart.
struct LoadedScreen
{
    std::unique_ptr<Screen> screen;
    std::vector<std::string> systems;
};

/// @brief Builds @p document into a screen on @p ui, hidden, owned by the
/// caller.
///
/// @p catalog is what an event name is resolved against — taken rather than
/// reached for, so a caller can hand over exactly the events a case is about.
[[nodiscard]] std::expected<LoadedScreen, ScreenLoadError> InstantiateScreen(Ui &ui, const ScreenDocument &document,
                                                                             const Core::EventCatalog &catalog);

} // namespace Assisi::Mondrian
