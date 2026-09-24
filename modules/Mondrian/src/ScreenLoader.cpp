/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Mondrian/ScreenLoader.hpp>

#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/Ui.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/EventCatalog.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

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

/// Whether @p node's verb has what it takes: a target it can act on when it
/// takes one and none when it does not, and a count of moves that moves.
bool VerbFits(const ScreenDocument &document, const ScreenNode &node)
{
    if (VerbTakesTarget(node.verb))
    {
        if (node.target >= document.nodes.size() || !VerbActsOn(node.verb, document.nodes[node.target].widget))
        {
            return false;
        }
    }
    else if (node.target != kNoNode)
    {
        return false;
    }
    return !VerbTakesMoves(node.verb) || node.moves != 0;
}

/// Whether every name in @p document resolves, and every node's contents suit
/// where it sits — and the patterns, compiled, for the caller to apply.
///
/// Answered before anything is built: a screen joins its Ui in its own
/// constructor, so one abandoned part-way would stay in it. Compiling the
/// patterns here rather than while building is what keeps that true, and it
/// leaves the build loop with no failure it would have to unwind from.
///
/// One slot per node, null wherever a node has no pattern.
std::expected<std::vector<std::shared_ptr<const Pattern>>, ScreenLoadError> Resolve(const ScreenDocument &document,
                                                                                    const Core::EventCatalog &catalog)
{
    std::vector<std::shared_ptr<const Pattern>> patterns;
    patterns.resize(document.nodes.size());

    // Views into the document, which outlives this function.
    std::unordered_set<std::string_view> names;

    for (std::size_t index = 0; index < document.nodes.size(); ++index)
    {
        const ScreenNode &node = document.nodes[index];

        // Checked here rather than left to the tree, which would refuse the
        // second one only after the first half of the screen was built.
        if (!node.name.empty() && !names.insert(node.name).second)
        {
            return std::unexpected(ScreenLoadError::DuplicateName);
        }

        // The root is the screen itself. A control there would have nothing to
        // be created under, and an action there would never fire.
        if (index == 0 && (node.widget != BuiltinWidget::None || node.action != ActionKind::None))
        {
            return std::unexpected(ScreenLoadError::MisplacedNode);
        }

        if (static_cast<uint32_t>(node.widget) >= static_cast<uint32_t>(BuiltinWidget::Count))
        {
            return std::unexpected(ScreenLoadError::UnsupportedWidget);
        }

        if (node.action == ActionKind::Event && catalog.Find(node.eventName) == nullptr)
        {
            return std::unexpected(ScreenLoadError::UnknownEvent);
        }

        if (node.action == ActionKind::Verb && !VerbFits(document, node))
        {
            return std::unexpected(ScreenLoadError::BadTarget);
        }

        if (node.widget == BuiltinWidget::TextField && !node.pattern.empty())
        {
            std::expected<std::shared_ptr<const Pattern>, PatternError> compiled = CompilePattern(node.pattern);
            if (!compiled)
            {
                return std::unexpected(ScreenLoadError::BadPattern);
            }
            patterns[index] = *std::move(compiled);
        }
    }
    return patterns;
}

/// The node @p node describes, built by the same call a screen written in C++
/// would make. Every control gives its node a style of its own, which the
/// caller replaces whole with the document's: a document is fully resolved, so
/// what it carries is the answer and not an override.
NodeId Create(Screen &screen, NodeId parent, const ScreenNode &node)
{
    switch (node.widget)
    {
    case BuiltinWidget::Button:
        return screen.AddButton(parent, node.text).node;
    case BuiltinWidget::Toggle:
        return screen.AddToggle(parent, node.on).node;
    case BuiltinWidget::ContinuousSlider:
        return screen.AddContinuousSlider(parent, node.range, node.value).node;
    case BuiltinWidget::SteppedSlider:
        return screen.AddSteppedSlider(parent, node.range, node.steps, node.step).node;
    case BuiltinWidget::Scroll:
        // Which axes scroll is a style field, so the style the document carries
        // is already the answer this argument wants.
        return screen.AddScroll(parent, node.style, node.style.enabledScrollBars);
    case BuiltinWidget::TextField:
        return screen.AddTextField(parent, node.lines).node;
    case BuiltinWidget::None:
    case BuiltinWidget::Count:
        break;
    }
    return screen.Add(parent, node.style);
}

/// Everything a field carries beyond being a field. @p pattern is the one
/// Resolve compiled for this node, or null.
void ApplyTextField(Screen &screen, TextFieldId field, const ScreenNode &node, std::shared_ptr<const Pattern> pattern)
{
    screen.SetPlaceholder(field, node.placeholder);
    screen.SetMaxLength(field, node.maxLength);
    // Only where the file asked for one. A field is unbounded already, and
    // saying so again would leave it holding a line count that an unbounded
    // field never had when it was built in code.
    if (node.height != TextHeight::Unbounded)
    {
        screen.SetHeight(field, node.height, node.lineLimit);
    }
    // Before the text, because setting text settles the field against whatever
    // pattern it has by then.
    screen.SetPattern(field, std::move(pattern), node.check);
    if (!node.text.empty())
    {
        screen.SetText(field, node.text);
    }
    // Last: masking a field also turns copy and cut off, so anything after it
    // that touched the abilities would quietly turn them back on.
    screen.SetMask(field, node.mask);
}

/// Everything about a node that is neither its style nor its place.
void ApplyFlags(NodeTree &tree, NodeId id, const ScreenNode &node)
{
    tree.SetVisible(id, node.visible);
    tree.SetEnabled(id, node.enabled);
    tree.SetBlocksPointer(id, node.blocksPointer);
    tree.SetTakesKeyboard(id, node.takesKeyboard);
}

/// What node @p index does when it fires. @p ids is every node's id by its
/// place in the document, so a verb's target is one lookup here and none per
/// click.
void ApplyAction(Screen &screen, const std::vector<NodeId> &ids, std::size_t index, const ScreenDocument &document,
                 const Core::EventCatalog &catalog)
{
    const ScreenNode &node = document.nodes[index];
    const NodeId id = ids[index];
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
        case ScreenVerb::Step:
            // Resolve checked the target is a slider in the table.
            screen.OnActivate(id, [target = ids[node.target], moves = node.moves](Screen &self)
                              { self.Step(target, moves); });
            return;
        case ScreenVerb::Count:
            return;
        }
        return;
    case ActionKind::Event:
        // Resolved in Resolve, which ran before anything was built.
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
    case ScreenLoadError::BadPattern:
        return "holds a pattern this build does not compile";
    case ScreenLoadError::UnsupportedWidget:
        return "names a control this build does not have";
    case ScreenLoadError::MisplacedNode:
        return "puts a control or an action where one cannot go";
    case ScreenLoadError::DuplicateName:
        return "gives two nodes one name";
    case ScreenLoadError::BadTarget:
        return "gives a verb a target it cannot act on";
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

    std::expected<std::vector<std::shared_ptr<const Pattern>>, ScreenLoadError> patterns = Resolve(document, catalog);
    if (!patterns)
    {
        return std::unexpected(patterns.error());
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
        else
        {
            // Created by the call its control names, then styled — the same
            // order a screen built in C++ uses.
            id = Create(screen, ids[node.parent], node);
            tree.SetStyle(id, node.style);

            if (node.widget == BuiltinWidget::None)
            {
                // A control took its text where it was created, from the label
                // or the starting contents its call takes.
                tree.SetText(id, node.text);
            }
            if (node.widget == BuiltinWidget::TextField)
            {
                ApplyTextField(screen, {.node = id}, node, (*patterns)[index]);
            }
        }

        // Resolve refused any name two nodes carry, and every node on this
        // screen came from this document.
        const std::expected<void, NameError> named = tree.SetName(id, node.name);
        ASSISI_ASSERT(named.has_value(), "a name Resolve passed was already taken on a screen it built");
        ApplyFlags(tree, id, node);
        if (node.selectable)
        {
            screen.SetSelectable(id, true);
        }
        ids.push_back(id);
    }

    // After every node exists: a verb's target may come later in the document
    // than the button naming it.
    for (std::size_t index = 0; index < document.nodes.size(); ++index)
    {
        ApplyAction(screen, ids, index, document, catalog);
    }

    if (document.focus != kNoNode)
    {
        ui.SetFocus(screen, ids[document.focus]);
    }
    return loaded;
}

} // namespace Assisi::Mondrian
