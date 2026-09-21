/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ScreenDocument.hpp
/// @brief A screen as data: one flat table of nodes, and what the screen says
/// about itself.
///
/// This is what a screen file becomes and what the loader reads, with nothing
/// of the markup left in it — no elements, no attributes, no templates. A
/// document is plain data any code can build, which is what keeps the loader
/// testable without a parser and the parser testable without a Ui.
///
/// The table is flat and in preorder, and every node's parent sits at a lower
/// index than the node itself. One pass over it therefore builds the tree with
/// no lookups and no second walk, and a parent index that points forward is a
/// document the reader refuses rather than a cycle the loader walks.

#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/Style.hpp>
#include <Assisi/Mondrian/TextEdit.hpp>
#include <Assisi/Mondrian/Widget.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Assisi::Mondrian
{

/// @brief No node. The root's parent, and a screen that focuses nothing.
inline constexpr uint32_t kNoNode = UINT32_MAX;

/// @brief What a node does when it is clicked or accepted.
enum class ActionKind : uint8_t
{
    None,  ///< nothing; the node is not a control, or is one nobody bound
    Verb,  ///< something the UI does to itself, which needs no game code
    Event, ///< pushes the event named in the node, looked up in the catalog
    Count
};

/// @brief What the UI does to itself, for a control whose whole effect is on
/// the screen it is on.
///
/// A closed set rather than a name looked up somewhere: these reach nothing
/// outside the UI, so there is nothing for a game to register and nothing a
/// file can get wrong beyond misspelling one, which the cook catches.
enum class ScreenVerb : uint8_t
{
    Hide, ///< hides the screen this node is on
    Count
};

/// @brief One node of a screen, as the file described it.
struct ScreenNode
{
    /// Fully resolved: every attribute the file wrote, over the defaults.
    ///
    /// Which fields were *written* is not recorded, because nothing yet reads a
    /// style from anywhere else. When a named style supplies a base for these to
    /// override, this table gains that set — and the payload version is what
    /// makes it a change rather than a migration.
    Style style;

    /// What Screen::Find looks this node up by. Empty for an unnamed node.
    std::string name;

    /// Text content: a text node's words, a button's label.
    std::string text;

    /// The named style this node asks for, or empty. Carried and not resolved:
    /// there is nowhere yet for a name to resolve to, and a file written today
    /// should not have to be edited when there is.
    std::string styleName;

    /// The event this node pushes, by catalog name. Set only for ActionKind::Event.
    std::string eventName;

    /// What a field shows while it holds nothing.
    std::string placeholder;

    /// The expression a field's text must match, or empty for no rule. The
    /// expression itself and never a name for one: a file may write either, and
    /// a name is expanded where it is read, so nothing downstream needs a table
    /// to look one up in.
    std::string pattern;

    /// What a slider's ends mean, and how far one press moves it. A stepped
    /// slider moves a whole step at a time and ignores the step here.
    SliderRange range;

    /// Where a continuous slider starts, within its range.
    float value = 0.f;

    uint32_t maxLength = kUnlimitedLength;

    /// How many lines UpTo and Exactly mean; unused while Unbounded.
    uint32_t lineLimit = 1;

    /// How many positions a stepped slider has, and which it starts on.
    int32_t steps = 1;
    int32_t step = 0;

    /// Index into the document's own table. kNoNode on the root alone.
    uint32_t parent = kNoNode;

    /// Which control this node is, or None for a plain box. It decides which of
    /// the fields above mean anything; the rest ride at their defaults, as they
    /// do on the live node this becomes.
    BuiltinWidget widget = BuiltinWidget::None;

    ActionKind action = ActionKind::None;

    /// Meaningful only for ActionKind::Verb.
    ScreenVerb verb = ScreenVerb::Hide;

    TextLines lines = TextLines::Single;
    TextMask mask = TextMask::None;
    TextCheck check = TextCheck::OnCommit;
    TextHeight height = TextHeight::Unbounded;

    /// Whether a toggle starts on.
    bool on = false;

    bool visible = true;
    bool enabled = true;
    /// Stops the pointer without taking focus: a panel's background, or a sheet
    /// over the whole screen.
    bool blocksPointer = false;
    bool takesKeyboard = false;
    /// Plain text a player may select and copy.
    bool selectable = false;
};

/// @brief A whole screen: its nodes, its traits, and what it needs installed.
struct ScreenDocument
{
    /// What the screen is called, which is how a system finds it again.
    std::string name;

    /// Preorder, parent before child. Index 0 is the root, which every document
    /// has and which the loader applies to the tree's existing root rather than
    /// creating.
    std::vector<ScreenNode> nodes;

    /// The systems this screen needs installed to work. Handed back by the
    /// loader for whoever owns the world to install; the UI cannot install
    /// anything and does not try.
    std::vector<std::string> systems;

    /// Which node takes focus when the screen is shown, or kNoNode.
    uint32_t focus = kNoNode;

    int32_t sortKey = kSortMenu;

    ScreenTraits traits;
};

} // namespace Assisi::Mondrian
