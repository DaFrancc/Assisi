/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Mondrian/ScreenLoader.hpp>

#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/Ui.hpp>

#include <Assisi/Core/EventCatalog.hpp>

#include <cstddef>
#include <utility>

namespace Assisi::Mondrian
{
namespace
{

/// Whether the table is a tree this build can walk in one pass. The blob reader
/// checks the same thing, and this is not a duplicate of it: a document can be
/// built in code and handed straight here, never having been bytes.
bool IsWalkable(const ScreenDocument &document)
{
    if (document.nodes.empty())
    {
        return false;
    }
    if (document.focus != kNoNode && document.focus >= document.nodes.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < document.nodes.size(); ++index)
    {
        const uint32_t parent = document.nodes[index].parent;
        const bool rootParent = index == 0 && parent == kNoNode;
        const bool childParent = index > 0 && parent < index;
        if (!rootParent && !childParent)
        {
            return false;
        }
    }
    return true;
}

/// Whether every name in @p document resolves, and every node's contents suit
/// where it sits. Answered before anything is built: a screen joins its Ui in
/// its own constructor, so one abandoned part-way would stay in it.
std::expected<void, ScreenLoadError> Resolvable(const ScreenDocument &document, const Core::EventCatalog &catalog)
{
    for (std::size_t index = 0; index < document.nodes.size(); ++index)
    {
        const ScreenNode &node = document.nodes[index];

        // The root is the screen itself. A control there would have nothing to
        // be created under, and an action there would never fire.
        if (index == 0 && (node.widget != BuiltinWidget::None || node.action != ActionKind::None))
        {
            return std::unexpected(ScreenLoadError::MisplacedNode);
        }

        if (node.widget != BuiltinWidget::None && node.widget != BuiltinWidget::Button)
        {
            return std::unexpected(ScreenLoadError::UnsupportedWidget);
        }

        if (node.action == ActionKind::Event && catalog.Find(node.eventName) == nullptr)
        {
            return std::unexpected(ScreenLoadError::UnknownEvent);
        }
    }
    return {};
}

/// Everything about a node that is neither its style nor its place.
void ApplyFlags(NodeTree &tree, NodeId id, const ScreenNode &node)
{
    tree.SetVisible(id, node.visible);
    tree.SetEnabled(id, node.enabled);
    tree.SetBlocksPointer(id, node.blocksPointer);
    tree.SetTakesKeyboard(id, node.takesKeyboard);
}

void ApplyAction(Screen &screen, NodeId id, const ScreenNode &node, const Core::EventCatalog &catalog)
{
    switch (node.action)
    {
    case ActionKind::None:
        return;
    case ActionKind::Verb:
        switch (node.verb)
        {
        case ScreenVerb::Hide:
            // Carried on the node rather than announced: closing a screen
            // touches nothing but the UI, so it works wherever the screen is
            // shown with nothing named anywhere.
            screen.OnActivate(id, [](Screen &self) { self.Hide(); });
            return;
        case ScreenVerb::Count:
            return;
        }
        return;
    case ActionKind::Event:
        // Resolved in Resolvable, which ran before anything was built.
        screen.Tree().SetOnActivate(id, catalog.Find(node.eventName)->push);
        return;
    case ActionKind::Count:
        return;
    }
}

} // namespace

std::string_view ToString(ScreenLoadError error) noexcept
{
    switch (error)
    {
    case ScreenLoadError::BadDocument:
        return "describes no tree that can be walked";
    case ScreenLoadError::UnknownEvent:
        return "names an event this build does not declare";
    case ScreenLoadError::UnsupportedWidget:
        return "names a control this build does not build from a document";
    case ScreenLoadError::MisplacedNode:
        return "puts a control or an action where one cannot go";
    case ScreenLoadError::Count:
        break;
    }
    return "unknown";
}

std::expected<LoadedScreen, ScreenLoadError> InstantiateScreen(Ui &ui, const ScreenDocument &document,
                                                               const Core::EventCatalog &catalog)
{
    if (!IsWalkable(document))
    {
        return std::unexpected(ScreenLoadError::BadDocument);
    }
    if (const std::expected<void, ScreenLoadError> resolvable = Resolvable(document, catalog); !resolvable)
    {
        return std::unexpected(resolvable.error());
    }

    LoadedScreen loaded;
    loaded.screen = std::make_unique<Screen>(ui, document.traits, document.sortKey, document.name);
    loaded.systems = document.systems;

    Screen &screen = *loaded.screen;
    NodeTree &tree = screen.Tree();

    // Index in the table to id in the tree. The table is preorder with every
    // parent before its children, so a parent's id is always already here.
    std::vector<NodeId> ids;
    ids.reserve(document.nodes.size());

    for (std::size_t index = 0; index < document.nodes.size(); ++index)
    {
        const ScreenNode &node = document.nodes[index];

        NodeId id;
        if (index == 0)
        {
            // The tree comes with a root, which is never created and never
            // destroyed. The document's first node describes that one.
            id = screen.Root();
            tree.SetStyle(id, node.style);
        }
        else if (node.widget == BuiltinWidget::Button)
        {
            // Created with its label, then styled — the same order a screen
            // built in C++ uses, because AddButton gives the node a style of
            // its own that the document's replaces whole.
            id = screen.AddButton(ids[node.parent], node.text).node;
            tree.SetStyle(id, node.style);
        }
        else
        {
            id = screen.Add(ids[node.parent], node.style);
            tree.SetText(id, node.text);
        }

        tree.SetName(id, node.name);
        ApplyFlags(tree, id, node);
        if (node.selectable)
        {
            screen.SetSelectable(id, true);
        }
        ApplyAction(screen, id, node, catalog);
        ids.push_back(id);
    }

    if (document.focus != kNoNode)
    {
        ui.SetFocus(screen, ids[document.focus]);
    }
    return loaded;
}

} // namespace Assisi::Mondrian
